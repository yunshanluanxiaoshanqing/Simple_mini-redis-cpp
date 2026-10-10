#pragma once

// Client.h — 单连接状态与协议循环（v3）
//
// 与 v1/v2 相同的增量解析模型，但由线程池的工作线程驱动，并支持
// "批量回写"：一次 recv 里可能包含多条流水线命令，它们的响应都在同一个
// 发送缓冲里累积，等输入耗尽（或达到回写阈值）再一次性写出，
// 用更少的系统调用完成同样的工作。

#include <string>
#include <vector>

#include "Commands.h"
#include "NetCompat.h"
#include "Resp.h"

namespace redis {

class Client {
public:
    /// socket 的所有权仍在连接登记处，本类只借用。
    explicit Client(net::Socket& socket, Executor& executor) : socket_(socket), executor_(executor) {}

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    /// 执行一条命令并把响应累积到发送缓冲；返回对连接/进程的影响。
    CommandEffect handle(const std::vector<std::string>& args);

    /// 读取一次 socket 数据；>0 有新数据，0 表示对端关闭，-1 表示错误。
    int receive();

    /// 把发送缓冲全部写出。
    bool flush();

    /// 取出下一条完整请求（内部处理协议错误）。
    resp::ParseState nextRequest(std::vector<std::string>& args);

    /// 是否已有响应待发送。
    bool hasPendingOutput() const { return pending_.size() > 0; }
    /// 发送缓冲是否达到批量回写阈值。
    bool outputFull() const { return pending_.size() >= kFlushThreshold; }

    bool wantClose() const { return want_close_ || parser_.failed(); }
    bool closed() const { return !socket_.valid(); }
    net::native_socket fd() const { return socket_.get(); }

private:
    static constexpr size_t kFlushThreshold = 256 * 1024;
    static constexpr size_t kReadChunk = 16 * 1024;

    net::Socket& socket_;
    Executor& executor_;
    resp::RequestParser parser_;
    resp::Response pending_;
    bool want_close_ = false;
};

}  // namespace redis
