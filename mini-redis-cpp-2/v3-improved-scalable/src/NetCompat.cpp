#include "NetCompat.h"

#include <cstring>

namespace net {

bool init() {
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    // 忽略 SIGPIPE：对端已关闭时 send 返回 EPIPE 而不是杀死整个进程。
    // 这是 01/02 版本最严重的健壮性缺陷之一（默认动作是终止进程）。
    ::signal(SIGPIPE, SIG_IGN);
    return true;
#endif
}

void cleanup() {
#ifdef _WIN32
    WSACleanup();
#endif
}

int lastError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

std::string errorText(int code) {
#ifdef _WIN32
    switch (code) {
        case WSAEINTR: return "interrupted system call";
        case WSAEACCES: return "permission denied";
        case WSAEADDRINUSE: return "address already in use";
        case WSAEADDRNOTAVAIL: return "address not available";
        case WSAECONNABORTED: return "software caused connection abort";
        case WSAECONNREFUSED: return "connection refused";
        case WSAECONNRESET: return "connection reset by peer";
        case WSAENETUNREACH: return "network is unreachable";
        case WSAETIMEDOUT: return "connection timed out";
        case WSAEWOULDBLOCK: return "resource temporarily unavailable";
        default: return "winsock error " + std::to_string(code);
    }
#else
    return std::strerror(code);
#endif
}

namespace {

/// 这些错误属于“被信号打断/暂无数据”，应当重试而不是当作致命错误。
bool isRetryable(int code) {
#ifdef _WIN32
    return code == WSAEINTR || code == WSAEWOULDBLOCK;
#else
    return code == EINTR || code == EAGAIN || code == EWOULDBLOCK;
#endif
}

/// 把毫秒转成 timeval（POSIX select 用）。
#ifndef _WIN32
timeval makeTimeval(int timeoutMs) {
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    return tv;
}
#endif

}  // namespace

bool Socket::reset() {
    if (fd_ == kInvalid) return false;
#ifdef _WIN32
    ::closesocket(fd_);
#else
    ::close(fd_);
#endif
    fd_ = kInvalid;
    return true;
}

int Socket::waitReadable(int timeoutMs) {
    if (fd_ == kInvalid) return -1;
    for (;;) {
#ifdef _WIN32
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(fd_, &readSet);
        timeval tv{};
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        const int rc = ::select(0, &readSet, nullptr, nullptr, &tv);
#else
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(fd_, &readSet);
        timeval tv = makeTimeval(timeoutMs);
        const int rc = ::select(fd_ + 1, &readSet, nullptr, nullptr, &tv);
#endif
        if (rc > 0) return 1;
        if (rc == 0) return 0;
        if (!isRetryable(lastError())) return -1;
        // 被信号打断：立即返回 0，让调用方有机会检查退出标志
        return 0;
    }
}

bool Socket::shutdownWrite() {
    if (fd_ == kInvalid) return false;
#ifdef _WIN32
    return ::shutdown(fd_, SD_SEND) == 0;
#else
    return ::shutdown(fd_, SHUT_WR) == 0;
#endif
}

bool Socket::bindAny(uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    return ::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
}

bool Socket::listen(int backlog) { return ::listen(fd_, backlog) == 0; }

bool Socket::accept(Socket& client) {
    for (;;) {
        native_socket fd = ::accept(fd_, nullptr, nullptr);
        if (fd != kInvalid) {
            client.reset();
            client = Socket(fd);
            return true;
        }
        const int err = lastError();
        if (isRetryable(err)) {
#ifndef _WIN32
            // 信号打断：若监听套接字已被关闭（关闭路径），视为正常结束。
            if (err == EINTR) continue;
#endif
            continue;
        }
        return false;
    }
}

bool Socket::connectTo(const std::string& host, uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_INET;  // 与服务器保持一致：IPv4
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || res == nullptr)
        return false;

    bool ok = false;
    for (addrinfo* it = res; it != nullptr && !ok; it = it->ai_next) {
        reset();
        fd_ = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd_ == kInvalid) continue;
        if (::connect(fd_, it->ai_addr, static_cast<int>(it->ai_addrlen)) == 0) ok = true;
    }
    ::freeaddrinfo(res);
    if (!ok) reset();
    return ok;
}

bool Socket::sendAll(const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        size_t chunk = data.size() - sent;
        if (chunk > 1u << 20) chunk = 1u << 20;  // 单次调用不超过 1 MiB
        int n = ::send(fd_, data.data() + sent, static_cast<int>(chunk), 0);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        const int err = lastError();
        if (isRetryable(err)) continue;
        return false;
    }
    return true;
}

int Socket::recvSome(char* buf, size_t len) {
    for (;;) {
        int n = ::recv(fd_, buf, static_cast<int>(len), 0);
        if (n < 0 && isRetryable(lastError())) continue;
        return n;
    }
}

bool Socket::setNoDelay(bool enable) {
    int v = enable ? 1 : 0;
    return ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&v),
                        sizeof(v)) == 0;
}

bool Socket::setReuseAddr(bool enable) {
    int v = enable ? 1 : 0;
#ifdef _WIN32
    // Windows 上 SO_REUSEADDR 允许两个进程绑定同一端口，反而危险；
    // 用 SO_EXCLUSIVEADDRUSE 表达“独占绑定”的本意。
    return ::setsockopt(fd_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                        reinterpret_cast<const char*>(&v), sizeof(v)) == 0;
#else
    return ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&v),
                        sizeof(v)) == 0;
#endif
}

bool Socket::setTimeouts(int sendMs, int recvMs) {
    bool ok = true;
#ifdef _WIN32
    DWORD tv = static_cast<DWORD>(sendMs);
    if (sendMs > 0 && ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO,
                                   reinterpret_cast<const char*>(&tv), sizeof(tv)) != 0)
        ok = false;
    tv = static_cast<DWORD>(recvMs);
    if (recvMs > 0 && ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO,
                                   reinterpret_cast<const char*>(&tv), sizeof(tv)) != 0)
        ok = false;
#else
    timeval tv{};
    tv.tv_sec = sendMs / 1000;
    tv.tv_usec = (sendMs % 1000) * 1000;
    if (sendMs > 0 && ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) ok = false;
    tv.tv_sec = recvMs / 1000;
    tv.tv_usec = (recvMs % 1000) * 1000;
    if (recvMs > 0 && ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) ok = false;
#endif
    return ok;
}

int poll(std::vector<PollItem>& items, int timeoutMs) {
    if (items.empty()) return 0;
    std::vector<pollfd> fds(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        fds[i].fd = items[i].fd;
        fds[i].events = POLLIN;
#ifdef _WIN32
        fds[i].events = static_cast<SHORT>(POLLIN);
#else
        fds[i].events = POLLIN;
#endif
        fds[i].revents = 0;
    }

    int rc;
    do {
#ifdef _WIN32
        rc = ::WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), timeoutMs);
#else
        rc = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeoutMs);
#endif
    } while (rc < 0 && isRetryable(lastError()));

    if (rc <= 0) return rc;

    int ready = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        items[i].readable = (fds[i].revents & POLLIN) != 0;
        items[i].writable = (fds[i].revents & POLLOUT) != 0;
        items[i].error = (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
        if (fds[i].revents != 0) ++ready;
    }
    return ready;
}

}  // namespace net
