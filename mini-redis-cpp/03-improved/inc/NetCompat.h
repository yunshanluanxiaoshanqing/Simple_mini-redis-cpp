#pragma once

// NetCompat.h — 跨平台网络与进程封装 (POSIX / Windows)
// 统一 socket 类型、关闭、初始化接口，屏蔽 winsock 与 BSD socket 差异。

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif

#else  // POSIX

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <csignal>

#endif

#include <cstddef>
#include <string>

namespace net {

#ifdef _WIN32

using socket_t = SOCKET;
inline constexpr socket_t INVALID_SOCK = INVALID_SOCKET;

inline void closeSocket(socket_t s) {
    if (s != INVALID_SOCKET) closesocket(s);
}

inline bool init() {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

inline void cleanup() { WSACleanup(); }

#else

using socket_t = int;
inline constexpr socket_t INVALID_SOCK = -1;

inline void closeSocket(socket_t s) {
    if (s != INVALID_SOCK) ::close(s);
}

inline bool init() { return true; }
inline void cleanup() {}

#endif

// send 可能短写，循环直到全部发出；失败时放弃剩余部分
inline void sendAll(socket_t fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        size_t chunk = data.size() - sent;
        if (chunk > 1024 * 1024) chunk = 1024 * 1024;
        int n = ::send(fd, data.data() + sent, static_cast<int>(chunk), 0);
        if (n <= 0) return;
        sent += static_cast<size_t>(n);
    }
}

}  // namespace net
