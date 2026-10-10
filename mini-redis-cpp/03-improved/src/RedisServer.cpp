#include "RedisServer.h"

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

namespace {

RedisServer* g_server = nullptr;

void signalHandler(int /*signal*/) {
    if (g_server) g_server->shutdown();
    // signal handler 中不做复杂清理：dump 已在 shutdown() 完成，
    // _Exit 跳过静态析构，避免与其他线程竞争。
    std::_Exit(0);
}

// 单个客户端连接的读写循环（在独立线程中运行）
void clientLoop(net::socket_t client) {
    RedisCommandHandler handler;
    std::string buffer;
    char chunk[4096];

    for (;;) {
        int n = ::recv(client, chunk, sizeof(chunk), 0);
        if (n <= 0) break;
        buffer.append(chunk, static_cast<size_t>(n));

        std::string command;
        int r;
        while ((r = RedisCommandHandler::extractCommand(buffer, command)) != 0) {
            std::string response = (r > 0) ? handler.processCommand(command)
                                           : "-ERR Protocol error\r\n";
            net::sendAll(client, response);
        }
    }
    net::closeSocket(client);
}

}  // namespace

RedisServer::RedisServer(int port) : port(port), server_socket(net::INVALID_SOCK), running(true) {
    g_server = this;
    setupSignalHandler();
}

void RedisServer::setupSignalHandler() {
    std::signal(SIGINT, signalHandler);
#ifndef _WIN32
    std::signal(SIGTERM, signalHandler);
#endif
}

void RedisServer::shutdown() {
    running = false;
    if (RedisDatabase::getInstance().dump("dump.my_rdb"))
        std::cout << "Database dumped" << std::endl;
    else
        std::cerr << "Failed to dump database" << std::endl;
    if (server_socket != net::INVALID_SOCK) {
        net::closeSocket(server_socket);
        server_socket = net::INVALID_SOCK;
    }
    std::cout << "Server shutdown" << std::endl;
}

bool RedisServer::run() {
    server_socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == net::INVALID_SOCK) {
        std::cerr << "Failed to create socket" << std::endl;
        return false;
    }

    int opt = 1;
#ifdef _WIN32
    // Windows 上 SO_REUSEADDR 允许双绑定，需用 EXCLUSIVEADDRUSE 保证独占
    ::setsockopt(server_socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                 reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
    ::setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt),
                 sizeof(opt));
#endif

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(static_cast<uint16_t>(port));
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(server_socket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) != 0) {
        std::cerr << "Failed to bind socket (port " << port << " in use?)" << std::endl;
        net::closeSocket(server_socket);
        server_socket = net::INVALID_SOCK;
        return false;
    }

    if (::listen(server_socket, 10) != 0) {
        std::cerr << "Failed to listen on socket" << std::endl;
        net::closeSocket(server_socket);
        server_socket = net::INVALID_SOCK;
        return false;
    }

    std::cout << "Server listening on port " << port << std::endl;

    while (running) {
        net::socket_t client = ::accept(server_socket, nullptr, nullptr);
        if (client == net::INVALID_SOCK) {
            if (running) std::cerr << "Failed to accept client connection" << std::endl;
            break;
        }
        std::thread(clientLoop, client).detach();
    }

    // 走到这里说明 accept 失败（非信号路径），兜底 dump
    if (server_socket != net::INVALID_SOCK) {
        net::closeSocket(server_socket);
        server_socket = net::INVALID_SOCK;
    }
    if (RedisDatabase::getInstance().dump("dump.my_rdb"))
        std::cout << "Database dumped" << std::endl;
    else
        std::cerr << "Failed to dump database" << std::endl;
    return true;
}
