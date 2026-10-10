#include "Client.h"

namespace redis {

CommandEffect Client::handle(const std::vector<std::string>& args) {
    return executor_.execute(args, pending_);
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
        // 协议错误：回复错误并关闭连接（与 Redis 一致，不做重同步）
        pending_.error("ERR Protocol error: " + parser_.errorMessage());
        want_close_ = true;
    }
    return state;
}

bool Client::flush() {
    if (pending_.size() == 0) return true;
    const bool ok = socket_.sendAll(pending_.data());
    pending_.clear();
    return ok;
}

}  // namespace redis
