#include "Client.h"

namespace redis {

namespace {
constexpr size_t kReadChunk = 16 * 1024;
}

Client::Client(net::Socket& socket) : socket_(socket) {}

CommandEffect Client::handle(const std::vector<std::string>& args) {
    executor_.execute(args, pending_);
    if (args.empty()) return CommandEffect::kNone;
    if (commandClosesConnection(args[0])) {
        want_close_ = true;
        return CommandEffect::kCloseConn;
    }
    if (commandShutsDownServer(args[0])) return CommandEffect::kStopServer;
    return CommandEffect::kNone;
}

int Client::receive() {
    char buf[kReadChunk];
    const int n = socket_.recvSome(buf, sizeof(buf));
    if (n > 0) parser_.feed(buf, static_cast<size_t>(n));
    return n;
}

resp::ParseState Client::nextRequest(std::vector<std::string>& args) {
    const resp::ParseState state = parser_.next(args);
    if (state == resp::ParseState::kError) {
        // 协议错误：按 Redis 行为回复错误并关闭连接（不做无意义的重同步）。
        pending_.error("ERR Protocol error: " + parser_.errorMessage());
        want_close_ = true;
    }    return state;
}

bool Client::flush() {
    if (pending_.size() == 0) return true;
    const bool ok = socket_.sendAll(pending_.data());
    pending_.clear();
    return ok;
}

}  // namespace redis
