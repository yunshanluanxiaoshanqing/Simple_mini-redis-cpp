#pragma once

// Storage.h — 线程安全内存数据库（v1）
//
// 本文件是 v1 的核心：把 01-original 的“命令层判类型 + 数据层再改数据”
// 改为「数据层单方法原子完成」，同时用 variant 建立"一个 key 一个类型"不变量。

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
};

struct Reply {
    Code code = Code::Ok;
    long long number = 0;
    std::string text;
    std::vector<std::string> items;
    std::string detail;  ///< UnknownCommand 等需要回填的具体内容
};

using ExpireAt = Clock::time_point;

struct Entry {
    Val value;
    bool has_expire = false;
    ExpireAt expire_at{};
};

enum class LoadResult { Ok, NotFound, Corrupt, IoError };

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

    /// 供持久化层读取快照（save 内部使用，需持锁）。
    static ExpireAt now() { return Clock::now(); }

private:
    /// 查表并顺带删除过期条目；调用方必须已持有 mutex_。
    Entry* lookup(const std::string& key);
    void removeIfEmpty(const std::string& key);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> data_;
};

// ---------- 工具 ----------
long long unixSeconds();
bool parseInt(const std::string& text, long long& out);
bool parseDurationMs(const std::string& text, long long& out_ms);

}  // namespace redis
