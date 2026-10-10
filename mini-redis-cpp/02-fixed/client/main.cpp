// redis-cli — 02-fixed 配套简易客户端（POSIX 版）
// 本客户端由 MiMo（AI 助手）编写，属 02-fixed 版本成果。
//
// 用法:
//   redis-cli [-p port] <command> [args...]   单命令
//   redis-cli [-p port]                       交互式 REPL
//
// 响应输出: +OK -> OK | $3/foo -> foo | $-1 -> (nil)
//           :42 -> 42 | -ERR ... -> (error) ERR ...
//           数组 -> 1) "a" / 2) "b"；无法识别的响应原样输出

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::string cur;
    bool active = false;
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

// 解析一个 RESP 响应；1=完成 0=不完整 -1=协议错误(原样输出整行)
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
            out = s.substr(dataStart, static_cast<size_t>(len));
            pos = dataStart + static_cast<size_t>(len) + 2;
            out = (depth > 0) ? "\"" + out + "\"" : out;
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
            if (n < 0) {
                out = "(nil)";
                return 1;
            }
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
        default: {
            // 服务器返回的非 RESP 文本（如 "Err: Empty command"）：原样输出
            std::string line;
            if (!readLine(line)) return 0;
            out = line;
            return 1;
        }
    }
}

bool sendAll(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

struct Conn {
    int fd;
    std::string buf;
};

bool recvMore(Conn& c) {
    char tmp[8192];
    ssize_t n = ::recv(c.fd, tmp, sizeof(tmp), 0);
    if (n <= 0) return false;
    c.buf.append(tmp, static_cast<size_t>(n));
    return true;
}

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

int connectTo(const std::string& host, int port, int& fd) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) return -1;
    fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }
    if (::connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        ::close(fd);
        fd = -1;
        return -1;
    }
    freeaddrinfo(res);
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    int port = 6379;
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
        } else {
            break;
        }
    }

    int fd = -1;
    if (connectTo(host, port, fd) != 0) {
        std::cerr << "Could not connect to " << host << ":" << port << std::endl;
        return 2;
    }
    Conn conn{fd, ""};
    int rc = 0;

    if (i < argc) {
        std::vector<std::string> tokens;
        for (; i < argc; ++i) tokens.emplace_back(argv[i]);
        if (!sendAll(fd, encodeCommand(tokens))) {
            std::cerr << "Send failed" << std::endl;
            rc = 2;
        } else {
            std::string pretty;
            if (!readOneReply(conn, pretty)) {
                std::cerr << "Connection closed" << std::endl;
                rc = 2;
            } else {
                std::cout << pretty << std::endl;
            }
        }
    } else {
        std::string line;
        while (true) {
            std::cout << host << ":" << port << "> " << std::flush;
            if (!std::getline(std::cin, line)) break;
            auto tokens = tokenize(line);
            if (tokens.empty()) continue;
            if (!sendAll(fd, encodeCommand(tokens))) {
                std::cerr << "Send failed" << std::endl;
                rc = 2;
                break;
            }
            std::string pretty;
            if (!readOneReply(conn, pretty)) {
                std::cerr << "Connection closed" << std::endl;
                rc = 2;
                break;
            }
            std::cout << pretty << std::endl;
        }
    }

    ::close(fd);
    return rc;
}
