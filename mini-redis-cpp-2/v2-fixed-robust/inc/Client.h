#pragma once

// Client.h — 单连接状态与协议循环
//
// 一个 Client 对象 = 一个 TCP 连接 + 收发缓冲 + 解析器。
// 01-original 把"一次 recv 当一条命令"写在 accept 循环的 lambda 里，
// 本版本把连接状态显式建模，使粘包/半包/流水线都能正确处理。

#include <string>
#include <vector>

#include "Commands.h"
#include "NetCompat.h"
#include "Resp.h"

namespace redis {

/// 单条命令执行后对连接/进程的影响。
enum class CommandEffect {
    kNone,        ///< 继续服务
    kCloseConn,   ///< 关闭当前连接（QUIT）
    kStopServer,  ///< 整个进程优雅退出（SHUTDOWN）
};

class Client {
public:
    /// socket 的所有权仍归调用方（Server 通过 shared_ptr 持有），本类只借用。
    explicit Client(net::Socket& socket);

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    /// 处理一条已解析的命令：执行后把响应写入发送缓冲。
    /// 返回该命令对连接/进程的影响。
    CommandEffect handle(const std::vector<std::string>& args);

    /// 读取一次 socket 数据；返回 >0 有新数据，0 表示对端关闭，-1 表示错误。
    int receive();

    /// 把发送缓冲全部写出；失败返回 false。
    bool flush();

    /// 是否已收到完整请求（用于 v1 的单线程轮询模型判断可处理）。
    resp::ParseState nextRequest(std::vector<std::string>& args);

    /// 是否请求关闭连接（协议错误或客户端发送 QUIT）。
    bool wantClose() const { return want_close_ || parser_.failed(); }

    /// 主动关闭连接。
    void close() { socket_.reset(); }

    bool closed() const { return !socket_.valid(); }

    net::native_socket fd() const { return socket_.get(); }

    net::Socket& socket() { return socket_; }

private:
    net::Socket& socket_;
    resp::RequestParser parser_;
    resp::Response pending_;
    Executor executor_{database()};
    bool want_close_ = false;
};

}  // namespace redis
