// main.cpp — mini-redis-server (v1-baseline-hardened)
//
// 用法:
//   v1-server [--port N] [--dump FILE] [--selftest]
//
// 相对 01-original 的改进：
//   * 端口参数完整校验（1..65535），越界或非数字都不再进入 UB 的 htons 截断
//   * bind 失败返回非零退出码（原实现打印错误后仍返回 0，脚本无法判断）
//   * 数据库加载失败区分“文件不存在”和“文件损坏”
//   * --selftest：不依赖网络即可验证核心语义（含并发一致性）

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
              << "  --selftest     运行内置语义自检后退出\n"
              << "  -h, --help     显示帮助\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    int port = 6379;
    std::string dump_path = "dump.my_rdb";
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
            std::cout << "Loaded " << dump_path << std::endl;
            break;
        case redis::LoadResult::NotFound:
            std::cout << "No dump file (" << dump_path << "); starting empty" << std::endl;
            break;
        case redis::LoadResult::Corrupt:
            std::cerr << "Dump file " << dump_path
                      << " is corrupt; starting empty (the file will be overwritten on exit)"
                      << std::endl;
            break;
        case redis::LoadResult::IoError:
            std::cerr << "Failed to read " << dump_path << "; starting empty" << std::endl;
            break;
    }

    redis::Server server(port, dump_path);
    const bool ok = server.run();  // 静态绑定到局部对象：不存在悬垂引用

    net::cleanup();
    if (!ok) {
        std::cerr << "Server exited with an error" << std::endl;
        return 1;
    }
    return 0;
}
