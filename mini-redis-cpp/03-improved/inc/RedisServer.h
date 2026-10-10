#pragma once

#include <atomic>

#include "NetCompat.h"
#include "RedisCommandHandler.h"

// 多线程 TCP Redis 服务器。
// 每个客户端连接一个线程；RESP/内联协议；SIGINT 触发优雅退出并 dump。
class RedisServer {
public:
    explicit RedisServer(int port);

    // 进入 accept 循环；setup 失败返回 false。
    // 收到 SIGINT 时进程直接退出（见 signalHandler），不会返回。
    bool run();

    // 停止服务：置位 running、dump 数据库、关闭监听 socket。
    void shutdown();

private:
    void setupSignalHandler();

    int port;
    net::socket_t server_socket;
    std::atomic<bool> running;
};
