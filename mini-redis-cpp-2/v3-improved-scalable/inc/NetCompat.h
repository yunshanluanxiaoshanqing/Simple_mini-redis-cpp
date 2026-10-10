#pragma once

// NetCompat.h — 跨平台 socket 封装（POSIX / Windows）
//
// 设计要点：
//   * net::init() / net::cleanup() 必须成对调用（Windows 需要 WSAStartup）。
//   * Socket 是 RAII 句柄：析构自动关闭，禁止拷贝、允许移动。
//     → 01-original 里“手工 close、异常路径漏 close”的问题从类型层面消除。
//   * net::Socket::accept() 通过引用参数返回新连接，所有权明确归属于调用者。
//   * 所有可能失败的调用都通过 net::lastError() 暴露最后一次错误码。

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif

#else

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <csignal>

#endif

namespace net {

#ifdef _WIN32
using native_socket = SOCKET;
#else
using native_socket = int;
#endif

/// 进程级初始化；失败返回 false（Windows 上是 WSAStartup 失败）。
bool init();

/// 进程级清理；与 init() 配对。
void cleanup();

/// 最后一次 socket 调用的错误码（Windows 为 WSAGetLastError，POSIX 为 errno）。
int lastError();

/// 错误码的可读描述。
std::string errorText(int code);

/// 已建立的连接或监听套接字。RAII：析构自动关闭。
class Socket {
public:
    Socket() = default;
    explicit Socket(native_socket fd) : fd_(fd) {}
    ~Socket() { reset(); }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) noexcept : fd_(other.fd_) { other.fd_ = kInvalid; }
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.fd_;
            other.fd_ = kInvalid;
        }
        return *this;
    }

    static constexpr native_socket kInvalid =
#ifdef _WIN32
        INVALID_SOCKET;
#else
        -1;
#endif

    bool valid() const { return fd_ != kInvalid; }
    native_socket get() const { return fd_; }

    /// 关闭并置为无效；返回是否确实关闭了一个有效句柄。
    bool reset();

    /// 关闭写方向（用于优雅收尾），失败返回 false。
    bool shutdownWrite();

    /// 绑定到通配地址的指定端口。
    bool bindAny(uint16_t port);

    /// 开始监听；backlog 为等待队列长度。
    bool listen(int backlog);

    /// 阻塞接受一个连接。成功时 client 持有新连接并返回 true。
    bool accept(Socket& client);

    /// 在 timeoutMs 内等待连接到达。
    /// 返回 1 表示有连接待接受，0 表示超时（无连接），-1 表示出错。
    ///
    /// 为什么需要它：阻塞式 accept() 收到退出信号时不会返回，而关闭监听套接字
    /// 也**不能**可靠唤醒正在 accept 的线程（POSIX 上属未定义行为，Windows 的
    /// closesocket 同样不会唤醒阻塞的 accept）。01/02 版本因此只能在信号处理器
    /// 里直接 exit()，dump 随时可能被截断。用带超时的等待把"可被唤醒"做正确。
    int waitReadable(int timeoutMs);

    /// 连接到 host:port（host 可为域名或点分十进制）。
    bool connectTo(const std::string& host, uint16_t port);

    /// 发送全部字节；被信号打断自动重试。失败返回 false。
    bool sendAll(const std::string& data);

    /// 接收一次；返回 >0 为字节数，0 为对端关闭，-1 为错误或中断。
    int recvSome(char* buf, size_t len);

    /// 关闭 Nagle，降低小响应延迟（流水线场景友好）。
    bool setNoDelay(bool enable);

    /// 开启/关闭地址复用。Windows 上使用 SO_EXCLUSIVEADDRUSE 保证独占监听。
    bool setReuseAddr(bool enable);

    /// 发送/接收超时（毫秒，0 表示不限制）。
    bool setTimeouts(int sendMs, int recvMs);

private:
    native_socket fd_ = kInvalid;
};

// ---------- 轮询（v1 用于监听 + 已建立连接的可读检测） ----------

struct PollItem {
    native_socket fd = Socket::kInvalid;
    bool readable = false;
    bool writable = false;
    bool error = false;
};

/// 等待若干 fd 就绪。timeoutMs < 0 表示永久阻塞。
/// 返回就绪数量；0 表示超时；-1 表示错误或被打断。
int poll(std::vector<PollItem>& items, int timeoutMs);

}  // namespace net
