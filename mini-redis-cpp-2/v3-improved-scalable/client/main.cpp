// client/main.cpp — v3-cli
//
// 用法:
//   v3-cli [-h host] [-p port] [--raw] <command> [args...]
//   v3-cli [-h host] [-p port] [--raw] --stdin     # 从 stdin 读命令，流水线发送
//   v3-cli [-h host] [-p port]                       # 交互式 REPL
//
// 与服务器共用 Resp/NetCompat/Storage 工具代码，避免"客户端自己实现一套
// 解析"带来的行为不一致（02-fixed 的客户端就是这样分叉的）。
// 退出码：0 成功、1 用法错误、2 连接失败、3 协议/连接中断。

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "NetCompat.h"
#include "Resp.h"
#include "Storage.h"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitConnect = 2;
constexpr int kExitProtocol = 3;

void usage(const char* prog) {
    std::cout << "Usage: " << prog << " [-h host] [-p port] [--raw] [command args...]\n"
              << "       " << prog << " [-h host] [-p port] [--raw] --stdin\n"
              << "       " << prog << " [-h host] [-p port] [--raw]\n";
}

std::string encode(const std::vector<std::string>& args) {
    std::string out = "*" + std::to_string(args.size()) + "\r\n";
    for (const auto& a : args) out += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
    return out;
}

/// 增量解析一条完整响应：1=完成、0=需要更多数据、-1=协议错误。
int parseReply(const std::string& buf, size_t& pos, std::string& out, int depth) {
    if (pos >= buf.size()) return 0;
    switch (buf[pos]) {
        case '+':
        case '-':
        case ':': {
            const size_t crlf = buf.find("\r\n", pos);
            if (crlf == std::string::npos) return 0;
            const std::string line = buf.substr(pos + 1, crlf - pos - 1);
            out = (buf[pos] == '-') ? "(error) " + line : line;
            pos = crlf + 2;
            return 1;
        }
        case '$': {
            const size_t crlf = buf.find("\r\n", pos);
            if (crlf == std::string::npos) return 0;
            long long len = 0;
            if (!redis::parseInt(buf.substr(pos + 1, crlf - pos - 1), len)) return -1;
            if (len == -1) {
                out = "(nil)";
                pos = crlf + 2;
                return 1;
            }
            if (len < 0) return -1;
            const size_t start = crlf + 2;
            if (start + static_cast<size_t>(len) + 2 > buf.size()) return 0;
            out = buf.substr(start, static_cast<size_t>(len));
            pos = start + static_cast<size_t>(len) + 2;
            if (depth > 0) out = "\"" + out + "\"";
            return 1;
        }
        case '*': {
            const size_t crlf = buf.find("\r\n", pos);
            if (crlf == std::string::npos) return 0;
            long long n = 0;
            if (!redis::parseInt(buf.substr(pos + 1, crlf - pos - 1), n)) return -1;
            pos = crlf + 2;
            if (n == -1) {
                out = "(nil)";
                return 1;
            }
            if (n < 0) return -1;
            out.clear();
            for (long long i = 0; i < n; ++i) {
                std::string elem;
                const int rc = parseReply(buf, pos, elem, depth + 1);
                if (rc <= 0) return rc;
                if (i > 0) out += "\n";
                out += std::to_string(i + 1) + ") " + elem;
            }
            return 1;
        }
        default:
            return -1;
    }
}

struct Connection {
    net::Socket socket;
    std::string buffer;

    bool readReply(std::string& pretty) {
        for (;;) {
            size_t pos = 0;
            const int rc = parseReply(buffer, pos, pretty, 0);
            if (rc == 1) {
                buffer.erase(0, pos);
                return true;
            }
            if (rc == -1) return false;
            char tmp[8192];
            const int n = socket.recvSome(tmp, sizeof(tmp));
            if (n <= 0) return false;
            buffer.append(tmp, static_cast<size_t>(n));
        }
    }
};

}  // namespace

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    int port = 6379;
    bool raw = false;
    bool stdin_mode = false;
    bool single_command = false;  // -c：把后面的参数当作一条命令执行（脚本用）
    int index = 1;

    for (; index < argc; ++index) {
        const std::string arg = argv[index];
        if (arg == "-h" && index + 1 < argc) {
            host = argv[++index];
        } else if (arg == "-p" && index + 1 < argc) {
            long long value = 0;
            if (!redis::parseInt(argv[++index], value) || value < 1 || value > 65535) {
                std::cerr << "Invalid port" << std::endl;
                return kExitUsage;
            }
            port = static_cast<int>(value);
        } else if (arg == "-c" || arg == "--command") {
            single_command = true;  // 允许 "cli -p PORT -c PING" 这类无参数命令
        } else if (arg == "--raw") {
            raw = true;
        } else if (arg == "--stdin") {
            stdin_mode = true;
        } else if (arg == "--help" || arg == "-help") {
            usage(argv[0]);
            return kExitOk;
        } else {
            break;
        }
    }

    if (!net::init()) {
        std::cerr << "Network init failed: " << net::errorText(net::lastError()) << std::endl;
        return kExitConnect;
    }

    Connection conn;
    if (!conn.socket.connectTo(host, static_cast<uint16_t>(port))) {
        std::cerr << "Could not connect to " << host << ":" << port << ": "
                  << net::errorText(net::lastError()) << std::endl;
        net::cleanup();
        return kExitConnect;
    }

    int rc = kExitOk;

    auto sendAndPrint = [&](const std::vector<std::string>& args) -> bool {
        if (!conn.socket.sendAll(encode(args))) return false;
        if (raw) {
            char tmp[8192];
            const int n = conn.socket.recvSome(tmp, sizeof(tmp));
            if (n <= 0) return false;
            std::cout.write(tmp, n);
            std::cout << std::flush;
            return true;
        }
        std::string pretty;
        if (!conn.readReply(pretty)) return false;
        std::cout << pretty << std::endl;
        return true;
    };

    if (stdin_mode) {
        std::vector<std::vector<std::string>> commands;
        std::string line;
        while (std::getline(std::cin, line)) {
            auto args = resp::splitInline(line);
            if (!args.empty()) commands.push_back(std::move(args));
        }
        if (commands.empty()) {
            std::cerr << "No commands on stdin" << std::endl;
            rc = kExitUsage;
        } else {
            std::string payload;
            for (const auto& args : commands) payload += encode(args);
            if (!conn.socket.sendAll(payload)) {
                rc = kExitConnect;
            } else {
                for (size_t i = 0; i < commands.size(); ++i) {
                    std::string pretty;
                    if (!conn.readReply(pretty)) {
                        std::cerr << "Connection closed after " << i << " replies" << std::endl;
                        rc = kExitProtocol;
                        break;
                    }
                    std::cout << pretty << std::endl;
                }
            }
        }
    } else if (single_command || index < argc) {
        std::vector<std::string> args;
        for (; index < argc; ++index) args.emplace_back(argv[index]);
        if (!sendAndPrint(args)) rc = kExitProtocol;
    } else {
        std::string line;
        while (true) {
            std::cout << host << ":" << port << "> " << std::flush;
            if (!std::getline(std::cin, line)) break;
            auto args = resp::splitInline(line);
            if (args.empty()) continue;
            if (!sendAndPrint(args)) {
                std::cerr << "Connection closed" << std::endl;
                rc = kExitProtocol;
                break;
            }
        }
    }

    net::cleanup();  // socket 由 net::Socket 的 RAII 析构关闭
    return rc;
}
