#include "RedisCommandHandler.h"

#include <cctype>
#include <cstring>
#include <regex>
#include <string>

namespace {

// 接收缓冲 / 单参数上限，防止恶意输入耗尽内存
constexpr size_t kMaxBuffer = 64 * 1024 * 1024;
constexpr long long kMaxArgs = 1024 * 1024;
constexpr long long kMaxArgLen = 32 * 1024 * 1024;

const char* kWrongType =
    "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n";

// 将 KEYS 通配符模式（支持 * 与 ?）编译为正则
std::string globToRegex(const std::string& pattern) {
    std::string re;
    re.reserve(pattern.size() * 2);
    for (char c : pattern) {
        if (c == '*') {
            re += ".*";
        } else if (c == '?') {
            re += '.';
        } else {
            if (strchr(".+(){}|^$\\[]", c) != nullptr) re += '\\';
            re += c;
        }
    }
    return re;
}

std::vector<std::string> parseRespCommand(const std::string& input) {
    std::vector<std::string> tokens;
    if (input.empty()) return tokens;

    // 内联（inline）命令：按空白切分
    if (input[0] != '*') {
        std::istringstream iss(input);
        std::string token;
        while (iss >> token) tokens.push_back(token);
        return tokens;
    }

    // RESP 数组命令
    size_t pos = 1;
    size_t crlf = input.find("\r\n", pos);
    if (crlf == std::string::npos) return tokens;
    int numElements = 0;
    try {
        numElements = std::stoi(input.substr(pos, crlf - pos));
    } catch (...) {
        return tokens;
    }
    pos = crlf + 2;
    if (numElements < 0) return tokens;

    for (int i = 0; i < numElements; i++) {
        if (pos >= input.size() || input[pos] != '$') break;
        pos++;
        crlf = input.find("\r\n", pos);
        if (crlf == std::string::npos) break;
        int len = 0;
        try {
            len = std::stoi(input.substr(pos, crlf - pos));
        } catch (...) {
            return tokens;
        }
        pos = crlf + 2;
        if (len < 0 || pos + static_cast<size_t>(len) + 2 > input.size()) break;
        tokens.emplace_back(input, pos, static_cast<size_t>(len));
        pos += static_cast<size_t>(len) + 2;
    }
    return tokens;
}

std::string simpleError(const std::string& msg) { return "-ERR " + msg + "\r\n"; }

std::string wrongNumber(const char* cmd) {
    return simpleError(std::string("wrong number of arguments for '") + cmd + "' command");
}

std::string bulk(const std::string& value) {
    return "$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
}

std::string nilBulk() { return "$-1\r\n"; }

std::string integerReply(long long v) { return ":" + std::to_string(v) + "\r\n"; }

// ---------- 命令处理 ----------

std::string handlePing(const std::vector<std::string>&, RedisDatabase&) {
    return "+PONG\r\n";
}

std::string handleEcho(const std::vector<std::string>& tokens, RedisDatabase&) {
    if (tokens.size() < 2) return wrongNumber("echo");
    return "+" + tokens[1] + "\r\n";
}

std::string handleFlushAll(const std::vector<std::string>&, RedisDatabase& db) {
    db.flushAll();
    return "+OK\r\n";
}

std::string handleSet(const std::vector<std::string>& tokens, RedisDatabase& db) {
    if (tokens.size() < 3) return wrongNumber("set");
    std::string t = db.type(tokens[1]);
    if (t == "list" || t == "hash") return kWrongType;
    db.set(tokens[1], tokens[2]);
    return "+OK\r\n";
}

std::string handleGet(const std::vector<std::string>& tokens, RedisDatabase& db) {
    if (tokens.size() < 2) return wrongNumber("get");
    std::string t = db.type(tokens[1]);
    if (t == "list" || t == "hash") return kWrongType;
    std::string value;
    if (db.get(tokens[1], value)) return bulk(value);
    return nilBulk();
}

std::string handleKeys(const std::vector<std::string>& tokens, RedisDatabase& db) {
    std::string pattern = tokens.size() >= 2 ? tokens[1] : "*";
    auto allKeys = db.keys();

    std::vector<std::string> matched;
    if (pattern == "*") {
        matched = std::move(allKeys);
    } else {
        std::regex re(globToRegex(pattern));
        for (auto& k : allKeys) {
            if (std::regex_match(k, re)) matched.push_back(std::move(k));
        }
    }
    std::sort(matched.begin(), matched.end());

    std::ostringstream oss;
    oss << "*" << matched.size() << "\r\n";
    for (const auto& key : matched) oss << bulk(key);
    return oss.str();
}

std::string handleType(const std::vector<std::string>& tokens, RedisDatabase& db) {
    if (tokens.size() < 2) return wrongNumber("type");
    return "+" + db.type(tokens[1]) + "\r\n";
}

std::string handleDel(const std::vector<std::string>& tokens, RedisDatabase& db) {
    if (tokens.size() < 2) return wrongNumber("del");
    return integerReply(db.del(tokens[1]) ? 1 : 0);
}

std::string handleExpire(const std::vector<std::string>& tokens, RedisDatabase& db) {
    if (tokens.size() < 3) return wrongNumber("expire");
    int seconds = 0;
    try {
        seconds = std::stoi(tokens[2]);
    } catch (...) {
        return simpleError("value is not an integer or out of range");
    }
    return integerReply(db.expire(tokens[1], seconds) ? 1 : 0);
}

std::string handleRename(const std::vector<std::string>& tokens, RedisDatabase& db) {
    if (tokens.size() < 3) return wrongNumber("rename");
    if (db.rename(tokens[1], tokens[2])) return "+OK\r\n";
    return simpleError("no such key");
}

std::string handleLget(const std::vector<std::string>& tokens, RedisDatabase& db) {
    if (tokens.size() < 2) return wrongNumber("lget");
    std::string t = db.type(tokens[1]);
    if (t == "string" || t == "hash") return kWrongType;
    auto elems = db.lget(tokens[1]);
    std::ostringstream oss;
    oss << "*" << elems.size() << "\r\n";
    for (const auto& elem : elems) oss << bulk(elem);
    return oss.str();
}

std::string handleLlen(const std::vector<std::string>& tokens, RedisDatabase& db) {
    if (tokens.size() < 2) return wrongNumber("llen");
    std::string t = db.type(tokens[1]);
    if (t == "string" || t == "hash") return kWrongType;
    return integerReply(db.llen(tokens[1]));
}

std::string handleLpush(const std::vector<std::string>& tokens, RedisDatabase& db) {
    if (tokens.size() < 3) return wrongNumber("lpush");
    std::string t = db.type(tokens[1]);
    if (t == "string" || t == "hash") return kWrongType;
    for (size_t i = 2; i < tokens.size(); ++i) db.lpush(tokens[1], tokens[i]);
    return integerReply(db.llen(tokens[1]));
}

std::string handleRpush(const std::vector<std::string>& tokens, RedisDatabase& db) {
    if (tokens.size() < 3) return wrongNumber("rpush");
    std::string t = db.type(tokens[1]);
    if (t == "string" || t == "hash") return kWrongType;
    for (size_t i = 2; i < tokens.size(); ++i) db.rpush(tokens[1], tokens[i]);
    return integerReply(db.llen(tokens[1]));
}

std::string handleLpop(const std::vector<std::string>& tokens, RedisDatabase& db) {
    if (tokens.size() < 2) return wrongNumber("lpop");
    std::string t = db.type(tokens[1]);
    if (t == "string" || t == "hash") return kWrongType;
    std::string value;
    if (db.lpop(tokens[1], value)) return bulk(value);
    return nilBulk();
}

std::string handleRpop(const std::vector<std::string>& tokens, RedisDatabase& db) {
    if (tokens.size() < 2) return wrongNumber("rpop");
    std::string t = db.type(tokens[1]);
    if (t == "string" || t == "hash") return kWrongType;
    std::string value;
    if (db.rpop(tokens[1], value)) return bulk(value);
    return nilBulk();
}

std::string handleLrem(const std::vector<std::string>& tokens, RedisDatabase& db) {
    if (tokens.size() < 4) return wrongNumber("lrem");
    std::string t = db.type(tokens[1]);
    if (t == "string" || t == "hash") return kWrongType;
    int count = 0;
    try {
        count = std::stoi(tokens[2]);
    } catch (...) {
        return simpleError("value is not an integer or out of range");
    }
    return integerReply(db.lrem(tokens[1], count, tokens[3]));
}

std::string handleLindex(const std::vector<std::string>& tokens, RedisDatabase& db) {
    if (tokens.size() < 3) return wrongNumber("lindex");
    std::string t = db.type(tokens[1]);
    if (t == "string" || t == "hash") return kWrongType;
    int index = 0;
    try {
        index = std::stoi(tokens[2]);
    } catch (...) {
        return simpleError("value is not an integer or out of range");
    }
    std::string value;
    if (db.lindex(tokens[1], index, value)) return bulk(value);
    return nilBulk();
}

std::string handleLset(const std::vector<std::string>& tokens, RedisDatabase& db) {
    if (tokens.size() < 4) return wrongNumber("lset");
    std::string t = db.type(tokens[1]);
    if (t == "string" || t == "hash") return kWrongType;
    int index = 0;
    try {
        index = std::stoi(tokens[2]);
    } catch (...) {
        return simpleError("value is not an integer or out of range");
    }
    if (db.lset(tokens[1], index, tokens[3])) return "+OK\r\n";
    return simpleError("index out of range");
}

std::string handleHset(const std::vector<std::string>& tokens, RedisDatabase& db) {
    if (tokens.size() < 4) return wrongNumber("hset");
    std::string t = db.type(tokens[1]);
    if (t == "string" || t == "list") return kWrongType;
    return integerReply(db.hset(tokens[1], tokens[2], tokens[3]) ? 1 : 0);
}

std::string handleHget(const std::vector<std::string>& tokens, RedisDatabase& db) {
    if (tokens.size() < 3) return wrongNumber("hget");
    std::string t = db.type(tokens[1]);
    if (t == "string" || t == "list") return kWrongType;
    std::string value;
    if (db.hget(tokens[1], tokens[2], value)) return bulk(value);
    return nilBulk();
}

std::string handleHexists(const std::vector<std::string>& tokens, RedisDatabase& db) {
    if (tokens.size() < 3) return wrongNumber("hexists");
    std::string t = db.type(tokens[1]);
    if (t == "string" || t == "list") return kWrongType;
    return integerReply(db.hexists(tokens[1], tokens[2]) ? 1 : 0);
}

std::string handleHdel(const std::vector<std::string>& tokens, RedisDatabase& db) {
    if (tokens.size() < 3) return wrongNumber("hdel");
    std::string t = db.type(tokens[1]);
    if (t == "string" || t == "list") return kWrongType;
    return integerReply(db.hdel(tokens[1], tokens[2]) ? 1 : 0);
}

std::string handleHgetall(const std::vector<std::string>& tokens, RedisDatabase& db) {
    if (tokens.size() < 2) return wrongNumber("hgetall");
    std::string t = db.type(tokens[1]);
    if (t == "string" || t == "list") return kWrongType;
    auto hash = db.hgetAll(tokens[1]);
    std::ostringstream oss;
    oss << "*" << hash.size() * 2 << "\r\n";
    for (const auto& p : hash) {
        oss << bulk(p.first);
        oss << bulk(p.second);
    }
    return oss.str();
}

std::string handleHkeys(const std::vector<std::string>& tokens, RedisDatabase& db) {
    if (tokens.size() < 2) return wrongNumber("hkeys");
    std::string t = db.type(tokens[1]);
    if (t == "string" || t == "list") return kWrongType;
    auto fields = db.hkeys(tokens[1]);
    std::sort(fields.begin(), fields.end());
    std::ostringstream oss;
    oss << "*" << fields.size() << "\r\n";
    for (const auto& f : fields) oss << bulk(f);
    return oss.str();
}

std::string handleHvals(const std::vector<std::string>& tokens, RedisDatabase& db) {
    if (tokens.size() < 2) return wrongNumber("hvals");
    std::string t = db.type(tokens[1]);
    if (t == "string" || t == "list") return kWrongType;
    auto vals = db.hvals(tokens[1]);
    std::sort(vals.begin(), vals.end());
    std::ostringstream oss;
    oss << "*" << vals.size() << "\r\n";
    for (const auto& v : vals) oss << bulk(v);
    return oss.str();
}

std::string handleHlen(const std::vector<std::string>& tokens, RedisDatabase& db) {
    if (tokens.size() < 2) return wrongNumber("hlen");
    std::string t = db.type(tokens[1]);
    if (t == "string" || t == "list") return kWrongType;
    return integerReply(db.hlen(tokens[1]));
}

std::string handleHmset(const std::vector<std::string>& tokens, RedisDatabase& db) {
    if (tokens.size() < 4 || (tokens.size() % 2 == 1)) {
        return simpleError("wrong number of arguments for 'hmset' command");
    }
    std::string t = db.type(tokens[1]);
    if (t == "string" || t == "list") return kWrongType;
    std::vector<std::pair<std::string, std::string>> field_values;
    for (size_t i = 2; i + 1 < tokens.size(); i += 2)
        field_values.emplace_back(tokens[i], tokens[i + 1]);
    db.hmset(tokens[1], field_values);
    return "+OK\r\n";
}

}  // namespace

int RedisCommandHandler::extractCommand(std::string& buffer, std::string& out) {
    out.clear();
    for (;;) {
        if (buffer.size() > kMaxBuffer) {
            buffer.clear();
            return -1;
        }
        // 跳过协议分隔用的空行
        size_t start = buffer.find_first_not_of("\r\n");
        if (start == std::string::npos) {
            buffer.clear();
            return 0;
        }
        if (start > 0) buffer.erase(0, start);

        if (buffer[0] == '*') {
            size_t pos = 0;
            size_t crlf = buffer.find("\r\n", pos);
            if (crlf == std::string::npos) return 0;  // 半包：等待更多数据

            long long nargs = 0;
            try {
                nargs = std::stoll(buffer.substr(1, crlf - 1));
            } catch (...) {
                buffer.erase(0, crlf + 2);
                return -1;
            }
            if (nargs <= 0 || nargs > kMaxArgs) {
                buffer.erase(0, crlf + 2);
                return -1;
            }
            pos = crlf + 2;

            for (long long i = 0; i < nargs; ++i) {
                if (pos >= buffer.size()) return 0;  // 半包
                if (buffer[pos] != '$') {
                    size_t e = buffer.find("\r\n", pos);
                    if (e == std::string::npos)
                        buffer.clear();
                    else
                        buffer.erase(0, e + 2);
                    return -1;
                }
                size_t lcrlf = buffer.find("\r\n", pos + 1);
                if (lcrlf == std::string::npos) return 0;  // 半包
                long long len = 0;
                try {
                    len = std::stoll(buffer.substr(pos + 1, lcrlf - pos - 1));
                } catch (...) {
                    buffer.erase(0, lcrlf + 2);
                    return -1;
                }
                if (len < 0 || len > kMaxArgLen) {
                    buffer.erase(0, lcrlf + 2);
                    return -1;
                }
                size_t dataStart = lcrlf + 2;
                if (dataStart + static_cast<size_t>(len) + 2 > buffer.size()) return 0;  // 半包
                // 数据之后必须紧跟 CRLF，否则视为协议错误
                if (buffer[dataStart + static_cast<size_t>(len)] != '\r' ||
                    buffer[dataStart + static_cast<size_t>(len) + 1] != '\n') {
                    size_t e = buffer.find("\r\n", pos);
                    if (e == std::string::npos)
                        buffer.clear();
                    else
                        buffer.erase(0, e + 2);
                    return -1;
                }
                pos = dataStart + static_cast<size_t>(len) + 2;
            }
            out = buffer.substr(0, pos);
            buffer.erase(0, pos);
            return 1;
        }

        // 内联命令：整行到 \n 为止
        size_t nl = buffer.find('\n');
        if (nl == std::string::npos) return 0;  // 半包
        out = buffer.substr(0, nl + 1);
        buffer.erase(0, nl + 1);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        if (out.empty()) continue;  // 空行，继续提取
        return 1;
    }
}

std::string RedisCommandHandler::processCommand(const std::string& commandLine) {
    try {
        auto tokens = parseRespCommand(commandLine);
        if (tokens.empty()) return simpleError("empty command");

        std::string cmd = tokens[0];
        std::transform(cmd.begin(), cmd.end(), cmd.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        RedisDatabase& db = RedisDatabase::getInstance();

        if (cmd == "PING") return handlePing(tokens, db);
        if (cmd == "ECHO") return handleEcho(tokens, db);
        if (cmd == "FLUSHALL") return handleFlushAll(tokens, db);
        if (cmd == "SET") return handleSet(tokens, db);
        if (cmd == "GET") return handleGet(tokens, db);
        if (cmd == "KEYS") return handleKeys(tokens, db);
        if (cmd == "TYPE") return handleType(tokens, db);
        if (cmd == "DEL" || cmd == "UNLINK") return handleDel(tokens, db);
        if (cmd == "EXPIRE") return handleExpire(tokens, db);
        if (cmd == "RENAME") return handleRename(tokens, db);
        if (cmd == "LGET") return handleLget(tokens, db);
        if (cmd == "LLEN") return handleLlen(tokens, db);
        if (cmd == "LPUSH") return handleLpush(tokens, db);
        if (cmd == "RPUSH") return handleRpush(tokens, db);
        if (cmd == "RPOP") return handleRpop(tokens, db);
        if (cmd == "LPOP") return handleLpop(tokens, db);
        if (cmd == "LREM") return handleLrem(tokens, db);
        if (cmd == "LINDEX") return handleLindex(tokens, db);
        if (cmd == "LSET") return handleLset(tokens, db);
        if (cmd == "HSET") return handleHset(tokens, db);
        if (cmd == "HGET") return handleHget(tokens, db);
        if (cmd == "HEXISTS") return handleHexists(tokens, db);
        if (cmd == "HDEL") return handleHdel(tokens, db);
        if (cmd == "HGETALL") return handleHgetall(tokens, db);
        if (cmd == "HKEYS") return handleHkeys(tokens, db);
        if (cmd == "HVALS") return handleHvals(tokens, db);
        if (cmd == "HLEN") return handleHlen(tokens, db);
        if (cmd == "HMSET") return handleHmset(tokens, db);
        return simpleError("unknown command");
    } catch (...) {
        return simpleError("protocol error");
    }
}
