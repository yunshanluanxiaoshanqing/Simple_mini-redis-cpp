#pragma once

// Storage.h — 线程安全内存数据库（v2-fixed-robust）
//
// 在 v1 的基础上强化"数据不被悄悄弄坏"这一维度：
//   * v1 的持久化文件没有魔数、没有版本、没有校验：字节被截断或篡改后，
//     load() 只能靠格式巧合去猜。v2 加上
//     `8 字节魔数 + 版本号 + 记录区 + 4 字节 CRC32`，CRC 不匹配即判定损坏，
//     绝不把垃圾数据装进内存。
//   * 同时仍能识别并读取 v1 的旧格式（向后兼容），日志中说明。
//   * 落盘采用「临时文件 + 回读校验 + 原子替换」，替换失败不影响旧文件。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace redis {

using Clock = std::chrono::steady_clock;
using List = std::vector<std::string>;
using Hash = std::unordered_map<std::string, std::string>;
using Val = std::variant<std::string, List, Hash>;

const char* typeName(const Val& value);

// 命令结果语义码：handler 只做“码 → RESP”翻译，不再重复判断状态。
enum class Code {
    Ok,
    Pong,
    Integer,
    Bulk,
    Nil,
    Array,
    WrongType,
    NotInteger,
    NoSuchKey,
    SyntaxError,
    IndexOutOfRange,
    InvalidExpire,
    UnknownCommand,
    EmptyCommand,
    DbError,
    OutOfMemory,  ///< 达到单库内存上限（v2 新增）
};

struct Reply {
    Code code = Code::Ok;
    long long number = 0;
    std::string text;
    std::vector<std::string> items;
    std::string detail;
};

using ExpireAt = Clock::time_point;

struct Entry {
    Val value;
    bool has_expire = false;
    ExpireAt expire_at{};
};

/// load() 的结果。v2 增加 LegacyFormat：成功读取了 v1 的旧格式文件。
enum class LoadResult { Ok, NotFound, Corrupt, IoError, LegacyFormat };

class Store {
public:
    Store() = default;
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    // ---------- 通用 ----------
    void flushAll();
    size_t size();
    std::vector<std::string> keys(const std::string& pattern);
    long long exists(const std::vector<std::string>& keys);
    long long del(const std::vector<std::string>& keys);
    std::string type(const std::string& key);

    // ---------- 字符串 ----------
    Reply set(const std::string& key, const std::string& value, bool nx, bool xx, bool keep_ttl,
              long long expire_ms);
    Reply get(const std::string& key);

    // ---------- 过期 ----------
    Reply expire(const std::string& key, long long seconds);
    Reply expireAt(const std::string& key, long long unix_seconds);
    Reply ttl(const std::string& key);
    Reply pttl(const std::string& key);
    Reply persist(const std::string& key);
    Reply rename(const std::string& old_key, const std::string& new_key);

    // ---------- 列表 ----------
    Reply lpush(const std::string& key, const std::vector<std::string>& values);
    Reply rpush(const std::string& key, const std::vector<std::string>& values);
    Reply lpop(const std::string& key, long long count);
    Reply rpop(const std::string& key, long long count);
    Reply llen(const std::string& key);
    Reply lget(const std::string& key);
    Reply lrange(const std::string& key, long long start, long long stop);
    Reply lindex(const std::string& key, long long index);
    Reply lset(const std::string& key, long long index, const std::string& value);
    Reply lrem(const std::string& key, long long count, const std::string& value);

    // ---------- 哈希 ----------
    Reply hset(const std::string& key, const std::vector<std::pair<std::string, std::string>>& fv);
    Reply hmset(const std::string& key, const std::vector<std::pair<std::string, std::string>>& fv);
    Reply hget(const std::string& key, const std::string& field);
    Reply hexists(const std::string& key, const std::string& field);
    Reply hdel(const std::string& key, const std::vector<std::string>& fields);
    Reply hgetall(const std::string& key);
    Reply hkeys(const std::string& key);
    Reply hvals(const std::string& key);
    Reply hlen(const std::string& key);

    // ---------- 持久化 ----------
    bool save(const std::string& path);
    LoadResult load(const std::string& path);

    /// 估算当前键值占用的字节数（用于内存上限与 INFO）。
    size_t approximateMemory() const;

    /// 单库内存上限（0 表示不限制）。v2 用它挡住"一个大 SET 打爆内存"。
    void setMemoryLimit(size_t bytes) { memory_limit_.store(bytes); }
    size_t memoryLimit() const { return memory_limit_.load(); }

    static ExpireAt now() { return Clock::now(); }

private:
    /// 查表并顺带删除过期条目；调用方必须已持有 mutex_。
    Entry* lookup(const std::string& key);
    void removeIfEmpty(const std::string& key);
    /// 当前所有键值占用的粗略字节数（不加锁，调用方需持锁）。
    size_t memoryUsageLocked() const;
    /// 待写入数据是否会让数据库超过内存上限（调用方需持锁）。
    bool wouldExceedLimit(size_t incoming) const;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> data_;
    std::atomic<size_t> memory_limit_{0};
};

// ---------- 工具 ----------
long long unixSeconds();
bool parseInt(const std::string& text, long long& out);
bool parseDurationMs(const std::string& text, long long& out_ms);

// ---------- 持久化文件格式常量 ----------
inline constexpr char kDumpMagic[8] = {'M', 'I', 'N', 'I', 'R', 'D', 'B', '2'};
inline constexpr uint8_t kDumpVersion = 2;

/// CRC-32（IEEE 802.3 反射多项式 0xEDB88320），用于持久化文件完整性校验。
uint32_t crc32(const void* data, size_t len, uint32_t seed = 0);
inline uint32_t crc32(const std::string& s, uint32_t seed = 0) {
    return crc32(s.data(), s.size(), seed);
}

}  // namespace redis
