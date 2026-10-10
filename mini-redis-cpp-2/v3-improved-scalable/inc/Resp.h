#pragma once

// Resp.h — RESP 协议：增量请求解析器 + 响应构造器
//
// 设计要点（针对 01-original 的三处结构性问题）：
//   1. 命令边界由“协议自身”决定，而不是由某一次 recv() 返回的字节数决定
//      → 粘包 / 半包 / 流水线 / 超过 1KB 的大命令全部正确处理。
//   2. 缓冲区用读游标 + 压缩，不做 `erase(0, n)` 的 O(n²) 搬移。
//   3. 解析器带总量上限，超限返回协议错误，攻击者无法用无限输入耗尽内存。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace resp {

/// 一次解析结果。
enum class ParseState {
    kNeedMore,  ///< 数据不完整，等待更多字节
    kRequest,   ///< 成功解析出一条请求（request 非空）
    kError,     ///< 协议错误（连接应回复 -ERR 后关闭）
};

/// 协议上限（超出即视为协议错误）。
struct Limits {
    size_t max_inline_line = 64 * 1024;          ///< 内联命令最大长度
    size_t max_buffer = 64 * 1024 * 1024;        ///< 单连接缓存上限
    long long max_args = 1024 * 1024;            ///< 单条命令参数个数上限
    size_t max_arg_len = 512 * 1024 * 1024;      ///< 单个参数长度上限
};

/// 增量请求解析器：feed() 追加字节，next() 按序取出完整请求。
class RequestParser {
public:
    explicit RequestParser(Limits limits = Limits{}) : limits_(limits) {}

    /// 追加已接收的字节。
    void feed(const char* data, size_t len) { buffer_.append(data, len); }

    /// 取出下一条完整请求。返回 kNeedMore 表示需要继续收数据。
    ParseState next(std::vector<std::string>& request);

    /// 解析器是否已进入错误状态（错误不可恢复，连接必须关闭）。
    bool failed() const { return failed_; }

    /// 协议错误的可读描述。
    const std::string& errorMessage() const { return error_; }

    /// 当前缓存中尚未消费的字节数（用于内存上限判断与统计）。
    size_t pending() const { return buffer_.size() - read_pos_; }

    /// 丢弃当前缓存（用于连接关闭前的清理）。
    void clear() {
        buffer_.clear();
        read_pos_ = 0;
    }

private:
    ParseState parseArray(std::vector<std::string>& request);
    ParseState parseInline(std::vector<std::string>& request);
    void fail(const std::string& message);
    /// 压缩已消费前缀，避免长连接下 buffer_ 无限增长。
    void compactIfNeeded();
    /// 从 pos 开始找 CRLF；找不到返回 npos。
    size_t findCrlf(size_t pos) const;

    Limits limits_;
    std::string buffer_;
    size_t read_pos_ = 0;
    bool failed_ = false;
    std::string error_;
};

/// 响应构造器：直接拼装 RESP 字节流。
class Response {
public:
    /// 简单字符串（+OK）。
    void status(const std::string& text);
    /// 错误（-ERR ...），第一个词按惯例为错误码。
    void error(const std::string& message);
    /// 整数（:1）。
    void integer(long long value);
    /// 批量字符串（$len\r\n...\r\n）。
    void bulk(const std::string& value);
    /// 空批量（$-1）。
    void nil();
    /// 数组头（*n），随后应依次写入 n 个元素。
    void arrayHeader(size_t n);
    /// 清空已累积的内容。
    void clear() { out_.clear(); }

    const std::string& data() const { return out_; }
    size_t size() const { return out_.size(); }

private:
    std::string out_;
};

/// 常用错误响应。
std::string wrongArity(const char* command);
std::string unknownCommand(const std::string& command);
std::string wrongType();
std::string valueNotInteger();

/// 内联命令切分：按空白拆分，支持双引号与反斜杠转义（为 KEYS 等命令服务）。
std::vector<std::string> splitInline(const std::string& line);

}  // namespace resp
