#include "Commands.h"

#include <algorithm>
#include <cctype>
#include <limits>

namespace redis {

namespace {

/// 命令名转大写：必须走 unsigned char，否则高位字节是 UB（原实现用 ::toupper(char)）。
std::string upperAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

void emitArray(const std::vector<std::string>& items, resp::Response& out) {
    out.arrayHeader(items.size());
    for (const auto& item : items) out.bulk(item);
}

}  // namespace

Store& database() {
    static Store* instance = new Store();  // 故意泄漏：见头文件说明
    return *instance;
}

void emit(const Reply& reply, resp::Response& out) {
    switch (reply.code) {
        case Code::Ok:
            out.status("OK");
            break;
        case Code::Pong:
            out.status("PONG");
            break;
        case Code::Integer:
            out.integer(reply.number);
            break;
        case Code::Bulk:
            out.bulk(reply.text);
            break;
        case Code::Nil:
            out.nil();
            break;
        case Code::Array:
            emitArray(reply.items, out);
            break;
        case Code::WrongType:
            out.error(resp::wrongType());
            break;
        case Code::NotInteger:
            out.error(resp::valueNotInteger());
            break;
        case Code::NoSuchKey:
            out.error("ERR no such key");
            break;
        case Code::SyntaxError:
            out.error("ERR syntax error");
            break;
        case Code::IndexOutOfRange:
            out.error("ERR index out of range");
            break;
        case Code::InvalidExpire:
            out.error("ERR invalid expire time in 'set' command");
            break;
        case Code::UnknownCommand:
            out.error(resp::unknownCommand(reply.detail));
            break;
        case Code::EmptyCommand:
            out.error("ERR empty command");
            break;
        case Code::DbError:
            out.error("ERR internal error");
            break;
    }
}

const std::unordered_map<std::string, Executor::CommandSpec>& Executor::table() {
    // 函数内静态：C++11 起初始化线程安全，不需要额外加锁。
    static const std::unordered_map<std::string, CommandSpec> kCommands = {
        {"PING", {&Executor::cmdPing, -1, false}},
        {"ECHO", {&Executor::cmdEcho, -2, false}},
        {"FLUSHALL", {&Executor::cmdFlushAll, -1, true}},
        {"SET", {&Executor::cmdSet, -3, true}},
        {"GET", {&Executor::cmdGet, 2, false}},
        {"KEYS", {&Executor::cmdKeys, -1, false}},
        {"TYPE", {&Executor::cmdType, 2, false}},
        {"EXISTS", {&Executor::cmdExists, -2, false}},
        {"DEL", {&Executor::cmdDel, -2, true}},
        {"UNLINK", {&Executor::cmdDel, -2, true}},
        {"EXPIRE", {&Executor::cmdExpire, -3, true}},
        {"EXPIREAT", {&Executor::cmdExpireAt, -3, true}},
        // 可选的第 4 个参数（NX/XX/GT/LT）由 handler 内部校验
        {"TTL", {&Executor::cmdTtl, 2, false}},
        {"PTTL", {&Executor::cmdPttl, 2, false}},
        {"PERSIST", {&Executor::cmdPersist, 2, true}},
        {"RENAME", {&Executor::cmdRename, 3, true}},
        {"LPUSH", {&Executor::cmdLpush, -3, true}},
        {"RPUSH", {&Executor::cmdRpush, -3, true}},
        {"LPOP", {&Executor::cmdLpop, -2, true}},  // LPOP key [count]
        {"RPOP", {&Executor::cmdRpop, -2, true}},  // RPOP key [count]
        {"LLEN", {&Executor::cmdLlen, 2, false}},
        {"LGET", {&Executor::cmdLget, 2, false}},
        {"LRANGE", {&Executor::cmdLrange, 4, false}},
        {"LINDEX", {&Executor::cmdLindex, 3, false}},
        {"LSET", {&Executor::cmdLset, 4, true}},
        {"LREM", {&Executor::cmdLrem, 4, true}},
        {"HSET", {&Executor::cmdHset, -4, true}},
        {"HMSET", {&Executor::cmdHmset, -4, true}},
        {"HGET", {&Executor::cmdHget, 3, false}},
        {"HEXISTS", {&Executor::cmdHexists, 3, false}},
        {"HDEL", {&Executor::cmdHdel, -3, true}},
        {"HGETALL", {&Executor::cmdHgetall, 2, false}},
        {"HKEYS", {&Executor::cmdHkeys, 2, false}},
        {"HVALS", {&Executor::cmdHvals, 2, false}},
        {"HLEN", {&Executor::cmdHlen, 2, false}},
        {"DBSIZE", {&Executor::cmdDbsize, 1, false}},
        {"QUIT", {&Executor::cmdQuit, 1, false}},
        {"SHUTDOWN", {&Executor::cmdShutdown, -1, true}},
    };
    return kCommands;
}

bool commandClosesConnection(const std::string& command_name) {
    return upperAscii(command_name) == "QUIT";
}

bool commandShutsDownServer(const std::string& command_name) {
    return upperAscii(command_name) == "SHUTDOWN";
}

void Executor::execute(const std::vector<std::string>& args, resp::Response& out) {
    if (args.empty()) {
        out.error("ERR empty command");
        return;
    }
    const std::string name = upperAscii(args[0]);
    const auto& commands = table();
    const auto it = commands.find(name);
    if (it == commands.end()) {
        out.error(resp::unknownCommand(args[0]));
        return;
    }

    const CommandSpec& spec = it->second;
    const long long argc = static_cast<long long>(args.size());
    // arity > 0：参数个数必须恰好等于它；arity < 0：至少 |arity| 个。
    const bool arity_ok =
        (spec.arity > 0) ? (argc == spec.arity) : (argc >= -spec.arity);
    if (!arity_ok) {
        out.error(resp::wrongArity(name.c_str()));
        return;
    }

    (this->*(spec.handler))(args, out);
}

// ---------- 连接与服务器 ----------

void Executor::cmdPing(const std::vector<std::string>& args, resp::Response& out) {
    if (args.size() >= 2)
        out.bulk(args[1]);
    else
        out.status("PONG");
}

void Executor::cmdEcho(const std::vector<std::string>& args, resp::Response& out) {
    out.bulk(args[1]);
}

void Executor::cmdFlushAll(const std::vector<std::string>&, resp::Response& out) {
    store_.flushAll();
    out.status("OK");
}

void Executor::cmdDbsize(const std::vector<std::string>&, resp::Response& out) {
    out.integer(static_cast<long long>(store_.size()));
}

void Executor::cmdQuit(const std::vector<std::string>&, resp::Response& out) {
    out.status("OK");  // 应答后由连接层关闭 socket
}

void Executor::cmdShutdown(const std::vector<std::string>&, resp::Response& out) {
    // 真正的退出动作由服务器层执行（先落盘再关闭监听套接字）。
    out.status("OK");
}

// ---------- 字符串 ----------

void Executor::cmdSet(const std::vector<std::string>& args, resp::Response& out) {
    bool nx = false, xx = false, keep_ttl = false;
    long long expire_ms = 0;

    for (size_t i = 3; i < args.size(); ++i) {
        const std::string opt = upperAscii(args[i]);
        if (opt == "NX") {
            nx = true;
        } else if (opt == "XX") {
            xx = true;
        } else if (opt == "KEEPTTL") {
            keep_ttl = true;
        } else if (opt == "EX" || opt == "PX") {
            if (i + 1 >= args.size()) {
                out.error("ERR syntax error");
                return;
            }
            long long value = 0;
            if (!parseInt(args[++i], value) || value <= 0) {
                out.error("ERR invalid expire time in 'set' command");
                return;
            }
            expire_ms = (opt == "EX") ? value * 1000 : value;
        } else {
            out.error("ERR syntax error");
            return;
        }
    }
    if (nx && xx) {
        out.error("ERR syntax error");
        return;
    }

    // 类型检查 + 写入在 Store 内一次完成，不存在 TOCTOU 窗口。
    emit(store_.set(args[1], args[2], nx, xx, keep_ttl, expire_ms), out);
}

void Executor::cmdGet(const std::vector<std::string>& args, resp::Response& out) {
    emit(store_.get(args[1]), out);
}

void Executor::cmdKeys(const std::vector<std::string>& args, resp::Response& out) {
    const std::string pattern = args.size() >= 2 ? args[1] : "*";
    emitArray(store_.keys(pattern), out);
}

void Executor::cmdType(const std::vector<std::string>& args, resp::Response& out) {
    out.status(store_.type(args[1]));
}

void Executor::cmdExists(const std::vector<std::string>& args, resp::Response& out) {
    std::vector<std::string> keys(args.begin() + 1, args.end());
    out.integer(store_.exists(keys));
}

void Executor::cmdDel(const std::vector<std::string>& args, resp::Response& out) {
    std::vector<std::string> keys(args.begin() + 1, args.end());
    out.integer(store_.del(keys));
}

// ---------- 过期 ----------

void Executor::cmdExpire(const std::vector<std::string>& args, resp::Response& out) {
    long long seconds = 0;
    if (!parseInt(args[2], seconds)) {
        out.error(resp::valueNotInteger());
        return;
    }
    emit(store_.expire(args[1], seconds), out);
}

void Executor::cmdExpireAt(const std::vector<std::string>& args, resp::Response& out) {
    long long when = 0;
    if (!parseInt(args[2], when)) {
        out.error(resp::valueNotInteger());
        return;
    }
    emit(store_.expireAt(args[1], when), out);
}

void Executor::cmdTtl(const std::vector<std::string>& args, resp::Response& out) {
    emit(store_.ttl(args[1]), out);
}

void Executor::cmdPttl(const std::vector<std::string>& args, resp::Response& out) {
    emit(store_.pttl(args[1]), out);
}

void Executor::cmdPersist(const std::vector<std::string>& args, resp::Response& out) {
    emit(store_.persist(args[1]), out);
}

void Executor::cmdRename(const std::vector<std::string>& args, resp::Response& out) {
    emit(store_.rename(args[1], args[2]), out);
}

// ---------- 列表 ----------

void Executor::cmdLpush(const std::vector<std::string>& args, resp::Response& out) {
    std::vector<std::string> values(args.begin() + 2, args.end());
    emit(store_.lpush(args[1], values), out);
}

void Executor::cmdRpush(const std::vector<std::string>& args, resp::Response& out) {
    std::vector<std::string> values(args.begin() + 2, args.end());
    emit(store_.rpush(args[1], values), out);
}

void Executor::cmdLpop(const std::vector<std::string>& args, resp::Response& out) {
    long long count = 1;
    if (args.size() == 3 && (!parseInt(args[2], count) || count <= 0)) {
        out.error("ERR value is out of range, must be positive");
        return;
    }
    emit(store_.lpop(args[1], count), out);
}

void Executor::cmdRpop(const std::vector<std::string>& args, resp::Response& out) {
    long long count = 1;
    if (args.size() == 3 && (!parseInt(args[2], count) || count <= 0)) {
        out.error("ERR value is out of range, must be positive");
        return;
    }
    emit(store_.rpop(args[1], count), out);
}

void Executor::cmdLlen(const std::vector<std::string>& args, resp::Response& out) {
    emit(store_.llen(args[1]), out);
}

void Executor::cmdLget(const std::vector<std::string>& args, resp::Response& out) {
    emit(store_.lget(args[1]), out);
}

void Executor::cmdLrange(const std::vector<std::string>& args, resp::Response& out) {
    long long start = 0, stop = 0;
    if (!parseInt(args[2], start) || !parseInt(args[3], stop)) {
        out.error(resp::valueNotInteger());
        return;
    }
    emit(store_.lrange(args[1], start, stop), out);
}

void Executor::cmdLindex(const std::vector<std::string>& args, resp::Response& out) {
    long long index = 0;
    if (!parseInt(args[2], index)) {
        out.error(resp::valueNotInteger());
        return;
    }
    emit(store_.lindex(args[1], index), out);
}

void Executor::cmdLset(const std::vector<std::string>& args, resp::Response& out) {
    long long index = 0;
    if (!parseInt(args[2], index)) {
        out.error(resp::valueNotInteger());
        return;
    }
    emit(store_.lset(args[1], index, args[3]), out);
}

void Executor::cmdLrem(const std::vector<std::string>& args, resp::Response& out) {
    long long count = 0;
    if (!parseInt(args[2], count)) {
        out.error(resp::valueNotInteger());
        return;
    }
    emit(store_.lrem(args[1], count, args[3]), out);
}

// ---------- 哈希 ----------

void Executor::cmdHset(const std::vector<std::string>& args, resp::Response& out) {
    if ((args.size() - 2) % 2 != 0) {
        out.error(resp::wrongArity("hset"));
        return;
    }
    std::vector<std::pair<std::string, std::string>> fv;
    for (size_t i = 2; i + 1 < args.size(); i += 2) fv.emplace_back(args[i], args[i + 1]);
    emit(store_.hset(args[1], fv), out);
}

void Executor::cmdHmset(const std::vector<std::string>& args, resp::Response& out) {
    if ((args.size() - 2) % 2 != 0) {
        out.error(resp::wrongArity("hmset"));
        return;
    }
    std::vector<std::pair<std::string, std::string>> fv;
    for (size_t i = 2; i + 1 < args.size(); i += 2) fv.emplace_back(args[i], args[i + 1]);
    emit(store_.hmset(args[1], fv), out);
}

void Executor::cmdHget(const std::vector<std::string>& args, resp::Response& out) {
    emit(store_.hget(args[1], args[2]), out);
}

void Executor::cmdHexists(const std::vector<std::string>& args, resp::Response& out) {
    emit(store_.hexists(args[1], args[2]), out);
}

void Executor::cmdHdel(const std::vector<std::string>& args, resp::Response& out) {
    std::vector<std::string> fields(args.begin() + 2, args.end());
    emit(store_.hdel(args[1], fields), out);
}

void Executor::cmdHgetall(const std::vector<std::string>& args, resp::Response& out) {
    emit(store_.hgetall(args[1]), out);
}

void Executor::cmdHkeys(const std::vector<std::string>& args, resp::Response& out) {
    emit(store_.hkeys(args[1]), out);
}

void Executor::cmdHvals(const std::vector<std::string>& args, resp::Response& out) {
    emit(store_.hvals(args[1]), out);
}

void Executor::cmdHlen(const std::vector<std::string>& args, resp::Response& out) {
    emit(store_.hlen(args[1]), out);
}

}  // namespace redis
