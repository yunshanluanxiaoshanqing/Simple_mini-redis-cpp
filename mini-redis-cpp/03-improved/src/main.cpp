#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "NetCompat.h"
#include "RedisDatabase.h"
#include "RedisServer.h"

int main(int argc, char* argv[]) {
    int port = 6379;
    if (argc >= 2) {
        try {
            port = std::stoi(argv[1]);
        } catch (...) {
            std::cerr << "Usage: " << argv[0] << " [port]" << std::endl;
            return 1;
        }
    }

    if (!net::init()) {
        std::cerr << "Network init failed" << std::endl;
        return 1;
    }

    if (RedisDatabase::getInstance().load("dump.my_rdb"))
        std::cout << "Loaded dump.my_rdb" << std::endl;
    else
        std::cout << "No dump file found; starting with an empty database" << std::endl;

    RedisServer redis(port);

    // 周期持久化；可用 REDIS_PERSIST_INTERVAL（秒）覆盖间隔，便于测试
    int interval = 300;
    if (const char* env = std::getenv("REDIS_PERSIST_INTERVAL")) {
        int v = std::atoi(env);
        if (v > 0) interval = v;
    }
    std::thread persistence([interval]() {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(interval));
            if (RedisDatabase::getInstance().dump("dump.my_rdb"))
                std::cout << "dump success" << std::endl;
            else
                std::cerr << "dump failed" << std::endl;
        }
    });
    persistence.detach();

    bool ok = redis.run();
    net::cleanup();
    return ok ? 0 : 1;
}
