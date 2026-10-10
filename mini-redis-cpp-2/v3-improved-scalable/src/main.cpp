// main.cpp — v3-server (v3-improved-scalable)
//
// 用法:
//   v3-server [--port N] [--dump FILE] [--threads N] [--maxclients N]
//             [--timeout MS] [--dump-interval MS] [--selftest]
//
// 与 v1/v2 的差别：
//   * --threads：固定工作线程数（默认 CPU 核数，上限 64）——连接数不再决定线程数
//   * --maxclients：最大连接数 + 队列上界，超限明确回复 -ERR 而不是耗尽内存
//   * --dump-interval：后台自动快照间隔（落盘不长时间持锁）

#include <cstdlib>
#include <iostream>
#include <string>

#include "Commands.h"
#include "NetCompat.h"
#include "Selftest.h"
#include "Server.h"

namespace {

void printUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "  --port N            监听端口（默认 6379，范围 1..65535）\n"
              << "  --dump FILE         持久化文件路径（默认 dump.my_rdb）\n"
              << "  --threads N         工作线程数（默认 CPU 核数，上限 64）\n"
              << "  --maxclients N      最大连接数（默认 1024）\n"
              << "  --timeout MS        空闲连接超时毫秒（默认 300000，0=不限制）\n"
              << "  --dump-interval MS  自动快照间隔毫秒（默认 300000，0=关闭）\n"
              << "  --selftest          运行内置语义自检后退出\n"
              << "  -h, --help          显示帮助\n"
              << "\n环境变量:\n"
              << "  MINI_REDIS_MAXMEMORY  单库内存上限（字节），超过后写命令返回 -OOM\n";
}

/// 解析一个非负整数选项；失败返回 false 且不修改 out。
bool parseOptionValue(const char* text, long long min, long long max, long long& out) {
    long long value = 0;
    if (!redis::parseInt(text, value)) return false;
    if (value < min || value > max) return false;
    out = value;
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    redis::Server::Options options;
    options.threads = redis::defaultThreadCount();
    bool selftest = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* name, long long min, long long max, long long& out) {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << std::endl;
                return false;
            }
            if (!parseOptionValue(argv[++i], min, max, out)) {
                std::cerr << "Invalid value for " << name << ": " << argv[i] << " (expected "
                          << min << ".." << max << ")" << std::endl;
                return false;
            }
            return true;
        };

        long long value = 0;
        if (arg == "--port" || arg == "-p") {
            if (!next("--port", 1, 65535, value)) return 1;
            options.port = static_cast<int>(value);
        } else if (arg == "--dump" || arg == "-d") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --dump" << std::endl;
                return 1;
            }
            options.dump_path = argv[++i];
            if (options.dump_path.empty()) {
                std::cerr << "Empty dump path" << std::endl;
                return 1;
            }
        } else if (arg == "--threads") {
            if (!next("--threads", 1, 64, value)) return 1;
            options.threads = static_cast<size_t>(value);
        } else if (arg == "--maxclients") {
            if (!next("--maxclients", 1, 100000, value)) return 1;
            options.max_clients = static_cast<int>(value);
        } else if (arg == "--timeout") {
            if (!next("--timeout", 0, 24LL * 3600 * 1000, value)) return 1;
            options.idle_timeout_ms = static_cast<int>(value);
        } else if (arg == "--dump-interval") {
            if (!next("--dump-interval", 0, 24LL * 3600 * 1000, value)) return 1;
            options.dump_interval_ms = static_cast<int>(value);
        } else if (arg == "--selftest") {
            selftest = true;
        } else if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << arg << std::endl;
            printUsage(argv[0]);
            return 1;
        }
    }

    if (!net::init()) {
        std::cerr << "Network initialization failed: " << net::errorText(net::lastError())
                  << std::endl;
        return 1;
    }

    if (selftest) {
        const int rc = redis::runSelfTest();
        net::cleanup();
        return rc;
    }

    switch (redis::database().load(options.dump_path)) {
        case redis::LoadResult::Ok:
            std::cout << "Loaded " << options.dump_path << " (format " << redis::kDumpFormatName
                      << ")" << std::endl;
            break;
        case redis::LoadResult::LegacyFormat:
            std::cout << "Loaded " << options.dump_path
                      << " in legacy format; it will be rewritten on exit" << std::endl;
            break;
        case redis::LoadResult::NotFound:
            std::cout << "No dump file (" << options.dump_path << "); starting empty" << std::endl;
            break;
        case redis::LoadResult::Corrupt:
            std::cerr << "Dump file " << options.dump_path
                      << " failed integrity check (magic/version/CRC32); starting empty" << std::endl;
            break;
        case redis::LoadResult::IoError:
            std::cerr << "Failed to read " << options.dump_path << "; starting empty" << std::endl;
            break;
    }

    if (const char* env = std::getenv("MINI_REDIS_MAXMEMORY")) {
        long long bytes = 0;
        if (redis::parseInt(env, bytes) && bytes > 0)
            redis::database().setMaxmemory(static_cast<size_t>(bytes));
    }

    redis::Server server(options);
    const bool ok = server.run();

    net::cleanup();
    if (!ok) {
        std::cerr << "Server exited with an error" << std::endl;
        return 1;
    }
    return 0;
}
