// redis-cli —— 配套的轻量 Redis 协议客户端
// 本客户端由 MiMo（AI 助手）编写，属 03-improved 版本成果。
//
// 用法:
//   redis-cli [-h host] [-p port] <command> [args...]   单条命令，打印响应
//   redis-cli [-h host] [-p port] --stdin               从 stdin 逐行读命令，
//                                                       全部流水线(pipeline)发送后统一读响应
//   redis-cli [-h host] [-p port]                       交互式 REPL
//
// 响应输出格式对齐 redis-cli：
//   +OK      -> OK
//   $3/foo   -> foo        $-1 -> (nil)
//   :42      -> 42         -ERR ... -> (error) ERR ...
//   *N 数组  -> 1) xxx / 2) yyy（元素带引号）

#include <cctype>
#include <iostream>
#include <string>
#include <vector>

#include "NetCompat.h"

namespace {

void usage(const char* prog) {
    std::cout << "Usage: " << prog << " [-h host] [-p port] [command args...]\n"
              << "       " << prog << " [-h host] [-p port] --stdin  (pipeline from stdin)\n"
              << "       " << prog << " [-h host] [-p port]            (interactive)\n";
}

// 支持双引号与反斜杠转义的命令行切分（交互/stdin 模式用）
std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::string cur;
    bool active = false;  // 当前 token 是否已开始（引号可产生空 token）
    bool inQuote = false;

    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (inQuote) {
            if (c == '\\' && i + 1 < line.size()) {
                cur += line[++i];
            } else if (c == '"') {
                inQuote = false;
            } else {
                cur += c;
            }
        } else if (c == '"') {
            inQuote = true;
            active = true;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (active) {
                tokens.push_back(cur);
                cur.clear();
                active = false;
            }
        } else {
            cur += c;
            active = true;
        }
    }
    if (active) tokens.push_back(cur);
    return tokens;
}

std::string encodeCommand(const std::vector<std::string>& tokens) {
    std::string out = "*" + std::to_string(tokens.size()) + "\r\n";
    for (const auto& t : tokens)
        out += "$" + std::to_string(t.size()) + "\r\n" + t + "\r\n";
    return out;
}

// ---------- RESP 响应解析（增量） ----------

// 返回 1=解析完成, 0=数据不完整, -1=协议错误
int parseReplyAt(const std::string& s, size_t& pos, std::string& out, int depth) {
    if (pos >= s.size()) return 0;
    char t = s[pos];

    auto readLine = [&](std::string& line) -> bool {
        size_t crlf = s.find("\r\n", pos + 1);
        if (crlf == std::string::npos) return false;
        line = s.substr(pos + 1, crlf - pos - 1);
        pos = crlf + 2;
        return true;
    };

    switch (t) {
        case '+': {
            std::string line;
            if (!readLine(line)) return 0;
            out = (depth > 0) ? "\"" + line + "\"" : line;
            return 1;
        }
        case '-': {
            std::string line;
            if (!readLine(line)) return 0;
            out = "(error) " + line;
            return 1;
        }
        case ':': {
            std::string line;
            if (!readLine(line)) return 0;
            out = line;
            return 1;
        }
        case '$': {
            size_t crlf = s.find("\r\n", pos + 1);
            if (crlf == std::string::npos) return 0;
            long long len = -2;
            try {
                len = std::stoll(s.substr(pos + 1, crlf - pos - 1));
            } catch (...) {
                return -1;
            }
            if (len == -1) {
                pos = crlf + 2;
                out = "(nil)";
                return 1;
            }
            if (len < 0) return -1;
            size_t dataStart = crlf + 2;
            if (dataStart + static_cast<size_t>(len) + 2 > s.size()) return 0;
            std::string value = s.substr(dataStart, static_cast<size_t>(len));
            pos = dataStart + static_cast<size_t>(len) + 2;
            out = (depth > 0) ? "\"" + value + "\"" : value;
            return 1;
        }
        case '*': {
            size_t crlf = s.find("\r\n", pos + 1);
            if (crlf == std::string::npos) return 0;
            long long n = -2;
            try {
                n = std::stoll(s.substr(pos + 1, crlf - pos - 1));
            } catch (...) {
                return -1;
            }
            pos = crlf + 2;
            if (n == -1) {
                out = "(nil)";
                return 1;
            }
            if (n < 0) return -1;
            out.clear();
            for (long long i = 0; i < n; ++i) {
                std::string elem;
                int r = parseReplyAt(s, pos, elem, depth + 1);
                if (r <= 0) return r;
                if (i > 0) out += "\n";
                out += std::to_string(i + 1) + ") " + elem;
            }
            return 1;
        }
        default:
            return -1;
    }
}

struct Conn {
    net::socket_t fd;
    std::string buf;
};

bool recvMore(Conn& c) {
    char tmp[8192];
    int n = ::recv(c.fd, tmp, sizeof(tmp), 0);
    if (n <= 0) return false;
    c.buf.append(tmp, static_cast<size_t>(n));
    return true;
}

// 读取一个完整响应并格式化；连接断开返回 false
bool readOneReply(Conn& c, std::string& pretty) {
    for (;;) {
        size_t pos = 0;
        int r = parseReplyAt(c.buf, pos, pretty, 0);
        if (r == 1) {
            c.buf.erase(0, pos);
            return true;
        }
        if (r == -1) {
            pretty = "(error) protocol error";
            c.buf.clear();
            return true;
        }
        if (!recvMore(c)) return false;
    }
}

bool connectTo(const std::string& host, int port, net::socket_t& fd) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || res == nullptr) return false;

    fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd == net::INVALID_SOCK) {
        freeaddrinfo(res);
        return false;
    }
    bool ok = ::connect(fd, res->ai_addr, static_cast<int>(res->ai_addrlen)) == 0;
    freeaddrinfo(res);
    if (!ok) {
        net::closeSocket(fd);
        fd = net::INVALID_SOCK;
    }
    return ok;
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    int port = 6379;
    bool stdinMode = false;
    int i = 1;

    for (; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" && i + 1 < argc) {
            host = argv[++i];
        } else if (a == "-p" && i + 1 < argc) {
            try {
                port = std::stoi(argv[++i]);
            } catch (...) {
                std::cerr << "Invalid port" << std::endl;
                return 1;
            }
        } else if (a == "--stdin") {
            stdinMode = true;
        } else if (a == "--help" || a == "-help") {
            usage(argv[0]);
            return 0;
        } else {
            break;
        }
    }

    if (!net::init()) {
        std::cerr << "Network init failed" << std::endl;
        return 1;
    }

    net::socket_t fd = net::INVALID_SOCK;
    if (!connectTo(host, port, fd)) {
        std::cerr << "Could not connect to " << host << ":" << port << std::endl;
        net::cleanup();
        return 2;
    }
    Conn conn{fd, ""};
    int rc = 0;

    if (stdinMode) {
        // 收集全部命令后一次性发送（流水线），再按序读取响应
        std::vector<std::vector<std::string>> commands;
        std::string line;
        while (std::getline(std::cin, line)) {
            auto tokens = tokenize(line);
            if (!tokens.empty()) commands.push_back(std::move(tokens));
        }
        if (commands.empty()) {
            std::cerr << "No commands on stdin" << std::endl;
            rc = 1;
        } else {
            std::string payload;
            for (const auto& cmd : commands) payload += encodeCommand(cmd);
            net::sendAll(fd, payload);

            bool ok = true;
            for (size_t k = 0; k < commands.size(); ++k) {
                std::string pretty;
                if (!readOneReply(conn, pretty)) {
                    std::cerr << "Connection closed after " << k << " replies" << std::endl;
                    ok = false;
                    break;
                }
                if (k > 0) std::cout << "\n";
                std::cout << pretty;
            }
            std::cout << std::endl;
            if (!ok) rc = 2;
        }
    } else if (i < argc) {
        // 单条命令模式：每个 argv 元素即一个参数
        std::vector<std::string> tokens;
        for (; i < argc; ++i) tokens.emplace_back(argv[i]);

        net::sendAll(fd, encodeCommand(tokens));
        std::string pretty;
        if (!readOneReply(conn, pretty)) {
            std::cerr << "Connection closed" << std::endl;
            rc = 2;
        } else {
            std::cout << pretty << std::endl;
        }
    } else {
        // 交互式 REPL
        std::string line;
        while (true) {
            std::cout << host << ":" << port << "> " << std::flush;
            if (!std::getline(std::cin, line)) break;
            auto tokens = tokenize(line);
            if (tokens.empty()) continue;

            net::sendAll(fd, encodeCommand(tokens));
            std::string pretty;
            if (!readOneReply(conn, pretty)) {
                std::cerr << "Connection closed" << std::endl;
                rc = 2;
                break;
            }
            std::cout << pretty << std::endl;
        }
    }

    net::closeSocket(fd);
    net::cleanup();
    return rc;
}
