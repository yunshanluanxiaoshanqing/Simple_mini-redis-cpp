#pragma once

// ThreadPool.h — 有界连接队列 + 固定工作线程池（v3 的核心结构改进）
//
// 为什么需要它：
//   01/02/03（含本仓库 v1/v2）都是"每个连接新建一个线程"。线程数随连接数
//   线性增长：10k 连接 = 10k 线程，每个线程默认 1~8MB 栈，光是栈就要几十 GB；
//   线程切换与调度开销也会把吞吐拖垮。
//   v3 用固定数量的工作线程 + 有界任务队列：
//     * 线程数不随连接数增长（--threads 指定，默认 = CPU 核数，上限 64）；
//     * 队列有上界，队列满时明确拒绝连接并回复 -ERR max number of clients，
//       而不是耗尽内存后被系统 OOM 杀掉；
//     * 停止时先清空队列再唤醒工作线程，避免"关不掉的 accept 循环"。

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "NetCompat.h"

namespace redis {

class ThreadPool {
public:
    /// 连接处理函数：拿到一个已建立的连接，负责处理到关闭。
    using Task = std::function<void(net::Socket&)>;

    ThreadPool() = default;
    ~ThreadPool() { stop(); }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /// 设置连接处理函数（必须在 start 之前调用）。
    void setHandler(Task handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        handler_ = std::move(handler);
    }

    /// 启动 threads 个线程；queue_capacity 为待处理连接队列上界。
    void start(size_t threads, size_t queue_capacity);

    /// 投递一个连接。队列已满或已停止时返回 false（调用方应回复错误并关闭）。
    bool submit(net::Socket socket);

    /// 停止：清空队列、唤醒并 join 所有工作线程；最多等待 timeout_ms。
    void stop(int timeout_ms = 5000);

    bool running() const { return running_.load(); }
    size_t threadCount() const { return workers_.size(); }
    size_t queueSize() const;
    size_t queueCapacity() const { return capacity_; }

private:
    void workerLoop();

    std::vector<std::thread> workers_;
    std::deque<net::Socket> queue_;
    mutable std::mutex mutex_;
    std::condition_variable has_work_;
    std::condition_variable finished_;
    Task handler_;
    size_t capacity_ = 1024;
    int active_ = 0;
    bool stopping_ = false;
    std::atomic<bool> running_{false};
};

}  // namespace redis
