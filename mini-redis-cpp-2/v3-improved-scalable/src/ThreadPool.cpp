#include "ThreadPool.h"

#include <algorithm>
#include <chrono>
#include <iostream>

namespace redis {

void ThreadPool::start(size_t threads, size_t queue_capacity) {
    if (running_.load()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        capacity_ = std::max<size_t>(1, queue_capacity);
        stopping_ = false;
    }
    running_.store(true);
    const size_t count = std::max<size_t>(1, threads);
    workers_.reserve(count);
    for (size_t i = 0; i < count; ++i) workers_.emplace_back([this] { workerLoop(); });
    std::cout << "[pool] " << count << " worker threads, queue capacity " << capacity_
              << std::endl;
}

bool ThreadPool::submit(net::Socket socket) {
    if (!running_.load()) return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || queue_.size() >= capacity_) return false;
        queue_.push_back(std::move(socket));
    }
    has_work_.notify_one();
    return true;
}

void ThreadPool::workerLoop() {
    for (;;) {
        net::Socket socket;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            has_work_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty()) break;
            socket = std::move(queue_.front());
            queue_.pop_front();
            ++active_;
        }

        // 在线程内处理一个连接，直到对端关闭 / 出错 / 要求退出。
        if (handler_) handler_(socket);
        socket.reset();  // 确保关闭

        {
            std::lock_guard<std::mutex> lock(mutex_);
            --active_;
            if (stopping_ && queue_.empty() && active_ == 0) finished_.notify_all();
        }
    }
}

size_t ThreadPool::queueSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

void ThreadPool::stop(int timeout_ms) {
    if (!running_.exchange(false)) {
        for (auto& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
        workers_.clear();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        // 丢弃尚未处理的连接：直接关闭它们的 socket
        for (auto& socket : queue_) socket.reset();
        queue_.clear();
    }
    has_work_.notify_all();

    // 最多等待 timeout_ms：工作线程可能正阻塞在客户端读超时上
    {
        std::unique_lock<std::mutex> lock(mutex_);
        finished_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                           [this] { return queue_.empty() && active_ == 0; });
    }
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
    workers_.clear();
}

}  // namespace redis
