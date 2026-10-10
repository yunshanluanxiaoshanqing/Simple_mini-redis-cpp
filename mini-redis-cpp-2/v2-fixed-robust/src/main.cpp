// main.cpp — mini-redis-server (v2-fixed-robust)
//
// 用法:
//   v2-server [--port N] [--dump FILE] [--timeout MS] [--selftest]
//
// v2 在 v1 之上新增：
//   * --timeout：空闲连接超时（毫秒，0=不限制），默认 300000
//   * MINI_REDIS_MAXMEMORY 环境变量：单库内存上限，超限返回 -OOM
//   * 打印持久化文件格式与旧格式迁移提示

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "Commands.h"
#include "NetCompat.h"
#include "Selftest.h"
#include "Server.h"

namespace {

void printUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "  --port N       监听端口（默认 6379，范围 1..65535）\n"
              << "  --dump FILE    持久化文件路径（默认 dump.my_rdb）\n"
              << "  --timeout MS   空闲连接超时毫秒数（默认 300000，0=不限制）\n"
              << "  --selftest     运行内置语义自检后退出\n"
              << "  -h, --help     显示帮助\n"
              << "\n环境变量:\n"
              << "  MINI_REDIS_MAXMEMORY  单库内存上限（字节），超过后写命令返回 -OOM\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    int port = 6379;
    std::string dump_path = "dump.my_rdb";
    int idle_timeout_ms = 300000;
    bool selftest = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--port" || arg == "-p") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << arg << std::endl;
                printUsage(argv[0]);
                return 1;
            }
            long long value = 0;
            if (!redis::parseInt(argv[++i], value) || value < 1 || value > 65535) {
                std::cerr << "Invalid port: " << argv[i] << " (expected 1..65535)" << std::endl;
                return 1;
            }
            port = static_cast<int>(value);
        } else if (arg == "--dump" || arg == "-d") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << arg << std::endl;
                return 1;
            }
            dump_path = argv[++i];
            if (dump_path.empty()) {
                std::cerr << "Empty dump path" << std::endl;
                return 1;
            }
        } else if (arg == "--timeout") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << arg << std::endl;
                return 1;
            }
            long long value = 0;
            if (!redis::parseInt(argv[++i], value) || value < 0 ||
                value > 24LL * 3600 * 1000) {
                std::cerr << "Invalid timeout (expected 0..86400000 ms)" << std::endl;
                return 1;
            }
            idle_timeout_ms = static_cast<int>(value);
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

    switch (redis::database().load(dump_path)) {
        case redis::LoadResult::Ok:
            std::cout << "Loaded " << dump_path << " (format " << redis::kDumpFormatName << ")"
                      << std::endl;
            break;
        case redis::LoadResult::LegacyFormat:
            std::cout << "Loaded " << dump_path
                      << " in legacy v1 format; it will be rewritten in the current format on exit"
                      << std::endl;
            break;
        case redis::LoadResult::NotFound:
            std::cout << "No dump file (" << dump_path << "); starting empty" << std::endl;
            break;
        case redis::LoadResult::Corrupt:
            std::cerr << "Dump file " << dump_path
                      << " failed integrity check (magic/version/CRC32); starting empty. "
                         "The old file is left untouched until the next successful dump."
                      << std::endl;
            break;
        case redis::LoadResult::IoError:
            std::cerr << "Failed to read " << dump_path << "; starting empty" << std::endl;
            break;
    }

    // 内存上限：环境变量 MINI_REDIS_MAXMEMORY（字节），默认不限制
    if (const char* env = std::getenv("MINI_REDIS_MAXMEMORY")) {
        long long bytes = 0;
        if (redis::parseInt(env, bytes) && bytes > 0)
            redis::database().setMemoryLimit(static_cast<size_t>(bytes));
    }

    redis::Server server(port, dump_path, idle_timeout_ms);
    const bool ok = server.run();  // 静态绑定到局部对象：不存在悬垂引用

    net::cleanup();
    if (!ok) {
        std::cerr << "Server exited with an error" << std::endl;
        return 1;
    }
    return 0;
}
