#pragma once

// Server.h — TCP 服务器（v1：每连接一线程 + 自管道唤醒的优雅退出）
//
// 针对 01-original 的缺陷：
//   1. 信号处理里直接 exit()：dump 过程可能被中断，且 handler 中调用
//      stdout/加锁等非异步信号安全函数属于未定义行为
//      → 本版本 signal handler 只写一个字节到自管道，真正的清理在主循环完成。
//   2. 线程 join 依赖客户端主动断开（原实现 accept 循环退出后 join，
//      只要有一个客户端连着就永久卡住）
//      → 本版本记录所有连接，关闭时主动 close 掉每个连接再 join。
//   3. bind 失败仍返回 0（脚本无法判断启动是否成功）
//      → run() 返回 false，main 以非零码退出。

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
    Server(int port, std::string dump_path);
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
    net::Socket listener_;
    std::atomic<bool> running_{true};
    std::mutex workers_mutex_;
    std::unordered_map<std::thread::id, Worker> workers_;
    std::atomic<int> active_connections_{0};
    std::atomic<long long> total_connections_{0};
    std::atomic<long long> total_commands_{0};
};

}  // namespace redis
