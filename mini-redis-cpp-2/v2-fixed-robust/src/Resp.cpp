#include "Resp.h"

#include <cctype>
#include <cstdlib>
#include <limits>

namespace resp {

namespace {

/// 解析十进制整数：必须整段都是数字，拒绝 "12abc" 这类前缀数字。
/// 01/02 版本使用 std::stoi，会静默接受前缀数字，属于输入校验漏洞。
bool parseDecimal(const std::string& text, long long& out) {
    if (text.empty()) return false;
    size_t i = 0;
    bool negative = false;
    if (text[0] == '-' || text[0] == '+') {
        negative = (text[0] == '-');
        i = 1;
        if (text.size() == 1) return false;
    }
    long long value = 0;
    for (; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (!std::isdigit(c)) return false;
        const int digit = c - '0';
        if (value > (std::numeric_limits<long long>::max() - digit) / 10) return false;  // 溢出
        value = value * 10 + digit;
    }
    out = negative ? -value : value;
    return true;
}

}  // namespace

size_t RequestParser::findCrlf(size_t pos) const { return buffer_.find("\r\n", pos); }

void RequestParser::fail(const std::string& message) {
    failed_ = true;
    error_ = message;
    buffer_.clear();
    read_pos_ = 0;
}

void RequestParser::compactIfNeeded() {
    // 消费过半且前缀足够长时压缩，均摊代价 O(1)。
    if (read_pos_ > 8192 && read_pos_ * 2 >= buffer_.size()) {
        buffer_.erase(0, read_pos_);
        read_pos_ = 0;
    }
}

ParseState RequestParser::next(std::vector<std::string>& request) {
    request.clear();
    if (failed_) return ParseState::kError;

    for (;;) {
        // 丢弃命令之间的空行（内联模式下客户端可能多发 \r\n）。
        while (read_pos_ < buffer_.size() &&
               (buffer_[read_pos_] == '\r' || buffer_[read_pos_] == '\n'))
            ++read_pos_;

        if (read_pos_ >= buffer_.size()) {
            compactIfNeeded();
            return ParseState::kNeedMore;
        }

        if (pending() > limits_.max_buffer) {
            fail("request buffer limit exceeded");
            return ParseState::kError;
        }

        const ParseState state =
            (buffer_[read_pos_] == '*') ? parseArray(request) : parseInline(request);
        if (state == ParseState::kRequest) {
            compactIfNeeded();
            return state;
        }
        if (state == ParseState::kError) return state;
        // kNeedMore：等待更多数据；若已消费大量前缀则先压缩。
        compactIfNeeded();
        return state;
    }
}

ParseState RequestParser::parseArray(std::vector<std::string>& request) {
    const size_t headerCrlf = findCrlf(read_pos_);
    if (headerCrlf == std::string::npos) {
        // 头部行本身超限 → 协议错误
        if (pending() > limits_.max_buffer) {
            fail("invalid multibulk length");
            return ParseState::kError;
        }
        return ParseState::kNeedMore;
    }

    long long argc = 0;
    if (!parseDecimal(buffer_.substr(read_pos_ + 1, headerCrlf - read_pos_ - 1), argc)) {
        fail("invalid multibulk length");
        return ParseState::kError;
    }
    if (argc <= 0) {
        fail("invalid multibulk length");
        return ParseState::kError;
    }
    if (argc > limits_.max_args) {
        fail("invalid multibulk length");
        return ParseState::kError;
    }

    size_t pos = headerCrlf + 2;
    request.clear();
    request.reserve(static_cast<size_t>(argc));

    for (long long i = 0; i < argc; ++i) {
        if (pos >= buffer_.size()) return ParseState::kNeedMore;
        if (buffer_[pos] != '$') {
            fail("expected '$', got something else");
            return ParseState::kError;
        }
        const size_t lenCrlf = findCrlf(pos + 1);
        if (lenCrlf == std::string::npos) return ParseState::kNeedMore;

        long long len = 0;
        if (!parseDecimal(buffer_.substr(pos + 1, lenCrlf - pos - 1), len)) {
            fail("invalid bulk length");
            return ParseState::kError;
        }
        if (len < 0 || static_cast<size_t>(len) > limits_.max_arg_len) {
            fail("invalid bulk length");
            return ParseState::kError;
        }

        const size_t dataStart = lenCrlf + 2;
        const size_t dataEnd = dataStart + static_cast<size_t>(len);
        // 声明长度本身就超过缓存上限：即使收满也无意义，直接判定协议错误，
        // 避免攻击者用一个巨大的 $长度 让连接永久等待并占用内存。
        if (static_cast<size_t>(len) > limits_.max_buffer) {
            fail("invalid bulk length");
            return ParseState::kError;
        }
        if (dataEnd + 2 > buffer_.size()) return ParseState::kNeedMore;
        if (buffer_[dataEnd] != '\r' || buffer_[dataEnd + 1] != '\n') {
            fail("bulk string is not terminated by CRLF");
            return ParseState::kError;
        }
        request.emplace_back(buffer_, dataStart, static_cast<size_t>(len));
        pos = dataEnd + 2;
    }

    read_pos_ = pos;
    return ParseState::kRequest;
}

ParseState RequestParser::parseInline(std::vector<std::string>& request) {
    const size_t nl = buffer_.find('\n', read_pos_);
    if (nl == std::string::npos) {
        if (pending() > limits_.max_inline_line) {
            fail("too big inline request");
            return ParseState::kError;
        }
        return ParseState::kNeedMore;
    }
    if (nl - read_pos_ > limits_.max_inline_line) {
        fail("too big inline request");
        return ParseState::kError;
    }

    std::string line = buffer_.substr(read_pos_, nl - read_pos_);
    read_pos_ = nl + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (line.empty()) return ParseState::kNeedMore;  // 空行：继续找下一条

    request = splitInline(line);
    if (request.empty()) return ParseState::kNeedMore;
    return ParseState::kRequest;
}

std::vector<std::string> splitInline(const std::string& line) {
    std::vector<std::string> tokens;
    std::string current;
    bool active = false;
    bool inQuote = false;

    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (inQuote) {
            if (c == '\\' && i + 1 < line.size()) {
                current += line[++i];
            } else if (c == '"') {
                inQuote = false;
            } else {
                current += c;
            }
        } else if (c == '"') {
            inQuote = true;
            active = true;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (active) {
                tokens.push_back(current);
                current.clear();
                active = false;
            }
        } else {
            current += c;
            active = true;
        }
    }
    if (active) tokens.push_back(current);
    return tokens;
}

void Response::status(const std::string& text) {
    out_ += '+';
    out_ += text;
    out_ += "\r\n";
}

void Response::error(const std::string& message) {
    out_ += '-';
    out_ += message;
    out_ += "\r\n";
}

void Response::integer(long long value) {
    out_ += ':';
    out_ += std::to_string(value);
    out_ += "\r\n";
}

void Response::bulk(const std::string& value) {
    out_ += '$';
    out_ += std::to_string(value.size());
    out_ += "\r\n";
    out_ += value;
    out_ += "\r\n";
}

void Response::nil() { out_ += "$-1\r\n"; }

void Response::arrayHeader(size_t n) {
    out_ += '*';
    out_ += std::to_string(n);
    out_ += "\r\n";
}

std::string wrongArity(const char* command) {
    // Redis 的错误信息里命令名是小写的；原实现直接回显了大写后的命令名。
    std::string lowered = command ? command : "";
    for (char& c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return "ERR wrong number of arguments for '" + lowered + "' command";
}

std::string unknownCommand(const std::string& command) {
    std::string lowered = command;
    for (char& c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return "ERR unknown command '" + lowered + "'";  // 03-improved 缺少命令名，这里补上
}

std::string wrongType() {
    return "WRONGTYPE Operation against a key holding the wrong kind of value";
}

std::string valueNotInteger() { return "ERR value is not an integer or out of range"; }

}  // namespace resp
