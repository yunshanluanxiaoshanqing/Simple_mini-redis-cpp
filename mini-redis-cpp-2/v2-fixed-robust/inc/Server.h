#pragma once

// Server.h — TCP 服务器（v2：每连接一线程 + 空闲超时 + 可被唤醒的 accept）
//
// 在 v1 的基础上补充健壮性：
//   1. 连接空闲超时：原实现（含 v1）对"连上却不发数据"的连接无限等待，
//      每个连接占一个线程 + 一个 fd，少量客户端即可耗尽资源；
//      v2 用带超时的可读等待，超时即断开。
//   2. 带超时的 accept：阻塞 accept() 无法被信号唤醒（POSIX 属未定义行为，
//      Windows closesocket 也不唤醒），这是 01/02 只能在信号里 exit() 的根因。
//   3. 每个连接的内存预算由 Resp 的 Limits 统一约束，超限即协议错误。

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "Client.h"
#include "NetCompat.h"

namespace redis {

/// 由 main 安装的信号处理：只做异步信号安全的 write()。
void installSignalHandlers();
/// 通知服务器退出（可由信号处理器或测试代码调用）。
void requestShutdown();

class Server {
public:
    /// idle_timeout_ms：连接空闲多久后主动断开（0 表示不限制）。
    explicit Server(int port, std::string dump_path, int idle_timeout_ms = 300000);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// 进入 accept 循环，直到收到退出信号或致命错误。
    /// 返回 false 表示监听套接字创建/绑定/监听失败（启动失败）。
    bool run();

    /// 被动退出：关闭所有连接、join 线程、落盘。
    void stop();

private:
    /// 单个客户端连接的工作对象（线程安全地登记/注销）。
    struct Worker {
        std::shared_ptr<net::Socket> socket;
        std::thread thread;
    };

    static void workerMain(Server& server, const std::shared_ptr<net::Socket>& socket);

    bool saveDatabase();

    int port_;
    std::string dump_path_;
    int idle_timeout_ms_ = 300000;
    net::Socket listener_;
    std::atomic<bool> running_{true};
    std::mutex workers_mutex_;
    std::unordered_map<std::thread::id, Worker> workers_;
    std::atomic<int> active_connections_{0};
    std::atomic<long long> total_connections_{0};
    std::atomic<long long> total_commands_{0};
};

}  // namespace redis
