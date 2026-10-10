#pragma once

// Server.h — TCP 服务器（v3：线程池 + 有界队列 + 后台快照 + 优雅退出）
//
// 与 v1/v2 的差别一览：
//   * 连接处理由固定大小的线程池完成（见 ThreadPool.h），线程数不随连接数增长；
//   * 最大连接数与最大队列长度可配，超限时明确回复错误再关闭；
//   * 空闲超时按 100ms 轮询检查，既能及时断开慢连接，又不会让退出卡住；
//   * 后台线程按 dump_interval_ms 自动落盘（快照拷贝在锁内、写盘在锁外）；
//   * 退出顺序：停止接入 → 排空队列 → 唤醒工作线程 → join → 落盘。

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "Client.h"
#include "NetCompat.h"
#include "ThreadPool.h"

namespace redis {

/// 默认工作线程数 = CPU 核数（下限 2、上限 64）。
size_t defaultThreadCount();

/// 由 main 安装的信号处理：只置位标志（异步信号安全）。
void installSignalHandlers();
/// 通知服务器退出（可由信号处理器、SHUTDOWN 命令或测试调用）。
void requestShutdown();
/// 供测试/嵌入使用：带配置的完整服务器。
class Server {
public:
    struct Options {
        int port = 6379;
        std::string dump_path = "dump.my_rdb";
        int idle_timeout_ms = 300000;
        int max_clients = 1024;
        size_t threads = 4;
        int dump_interval_ms = 300000;
    };

    explicit Server(Options options);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// 进入 accept 循环，直到收到退出信号或致命错误。
    /// 返回 false 表示监听套接字创建/绑定/监听失败（启动失败）。
    bool run();
    /// 主动停止（幂等）。
    void stop();

private:
    void connectionWorker(net::Socket& socket);
    void backgroundSnapshotLoop();
    bool saveDatabase();
    /// 读取当前应生效的配置（受 CONFIG SET 影响）。
    RuntimeConfig liveConfig() const;

    Options options_;
    net::Socket listener_;
    ThreadPool pool_;
    std::atomic<bool> running_{true};
    std::atomic<int> active_connections_{0};

    std::thread snapshot_thread_;
    std::mutex snapshot_mutex_;
    std::condition_variable snapshot_wakeup_;
    std::atomic<bool> snapshot_stop_{false};
};

}  // namespace redis
