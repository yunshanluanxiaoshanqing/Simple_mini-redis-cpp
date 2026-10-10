#pragma once

// Storage.h — 线程安全内存数据库（v3-improved-scalable）
//
// 相对 v1/v2 的结构性改进：
//
//   1. **快照式持久化**：save() 只在锁内拷贝数据，落盘与校验都在锁外完成。
//      v1/v2 的 save() 全程持锁 → 大库落盘时所有客户端一起卡住。
//   2. **过期时间轮**：按秒分桶记录过期键，不再每次操作都线性扫描整个
//      expiry_map（01/02/03 的 purgeExpired 都是全表扫描，带 TTL 的键一多，
//      每条命令的代价就随 TTL 键数增长）。
//   3. **运行时配置**：maxmemory / 空闲超时 / 最大连接数可热改，便于压测运维。
//   4. **统计计数**：命中/未命中/过期/拒绝连接等，供 INFO 输出。
//   5. 保留 v2 的 `魔数 + 版本 + CRC32` 持久化格式（与本仓库 v2 互通）。

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
    OutOfMemory,
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

enum class LoadResult { Ok, NotFound, Corrupt, IoError, LegacyFormat };

/// 落盘结果：区分“成功”与“已有一次落盘在进行中”。
enum class SaveResult { Ok, Busy, IoError };

/// 数据库统计（供 INFO 使用）。
struct Stats {
    size_t keys = 0;
    size_t used_memory = 0;
    size_t maxmemory = 0;
    long long hits = 0;
    long long misses = 0;
    long long expired_keys = 0;
    long long commands = 0;
    long long connections = 0;
    long long rejected_connections = 0;
    long long snapshots = 0;
    long long snapshot_errors = 0;
};

/// 可在运行期调整的参数。
struct RuntimeConfig {
    size_t maxmemory = 0;          ///< 0 = 不限制
    int idle_timeout_ms = 300000;  ///< 连接空闲超时
    int max_clients = 1024;        ///< 最大同时连接数
    int dump_interval_ms = 300000; ///< 后台自动落盘间隔
};

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
    Reply append(const std::string& key, const std::string& value);
    Reply incrBy(const std::string& key, long long delta);

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
    SaveResult save(const std::string& path);
    LoadResult load(const std::string& path);
    /// 在锁内构造完整的文件字节流（魔数+版本+记录区+CRC32），供后台线程慢慢写盘。
    std::string exportSnapshot() const;
    /// 从字节流装载（内部写临时文件后调用 load；供测试验证 CRC 行为）。
    LoadResult importSnapshot(const std::string& bytes);
    bool saving() const { return saving_.load(); }
    bool lastSaveOk() const { return last_save_ok_.load(); }

    // ---------- 统计与配置 ----------
    Stats stats() const;
    void noteCommand() { commands_.fetch_add(1, std::memory_order_relaxed); }
    void noteConnection() { connections_.fetch_add(1, std::memory_order_relaxed); }
    void noteRejectedConnection() { rejected_.fetch_add(1, std::memory_order_relaxed); }
    void noteSnapshot(bool ok) {
        snapshots_.fetch_add(1, std::memory_order_relaxed);
        if (!ok) snapshot_errors_.fetch_add(1, std::memory_order_relaxed);
        last_save_ok_.store(ok, std::memory_order_relaxed);
    }

    void setMaxmemory(size_t bytes) {
        std::lock_guard<std::mutex> lock(config_mutex_);
        config_.maxmemory = bytes;
    }
    size_t maxmemory() const {
        std::lock_guard<std::mutex> lock(config_mutex_);
        return config_.maxmemory;
    }
    RuntimeConfig config() const {
        std::lock_guard<std::mutex> lock(config_mutex_);
        return config_;
    }
    /// 更新热改参数；返回 false 表示参数非法（未做任何修改）。
    bool updateConfig(const RuntimeConfig& next);

    /// 估算当前键值占用的字节数。
    size_t approximateMemory() const;

    static ExpireAt now() { return Clock::now(); }

private:
    /// 查表并顺带删除过期条目；调用方必须已持有 mutex_。
    Entry* lookup(const std::string& key);
    void removeIfEmpty(const std::string& key);
    size_t memoryUsageLocked() const;
    bool wouldExceedLimit(size_t incoming) const;
    /// 设置 key 的过期时间（同时维护时间轮分桶）。
    void setExpireLocked(const std::string& key, Entry& entry, ExpireAt at);
    /// 清除 key 的过期记录（含时间轮分桶）。
    void clearExpireLocked(const std::string& key, Entry& entry);
    /// 处理已到期的桶（调用方须持锁）。
    void advanceExpiryWheelLocked();
    /// 当前 steady_clock 的"秒"游标。
    static long long wheelSecond(ExpireAt at);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> data_;

    // 过期时间轮：bucket = 绝对秒
    std::unordered_map<long long, std::vector<std::string>> expiry_buckets_;
    long long wheel_cursor_ = 0;
    std::unordered_map<std::string, long long> key_bucket_;

    mutable std::mutex config_mutex_;
    RuntimeConfig config_;

    std::atomic<bool> saving_{false};
    std::atomic<bool> last_save_ok_{true};
    std::atomic<long long> hits_{0};
    std::atomic<long long> misses_{0};
    std::atomic<long long> expired_keys_{0};
    std::atomic<long long> commands_{0};
    std::atomic<long long> connections_{0};
    std::atomic<long long> rejected_{0};
    std::atomic<long long> snapshots_{0};
    std::atomic<long long> snapshot_errors_{0};
};

// ---------- 工具 ----------
long long unixSeconds();
bool parseInt(const std::string& text, long long& out);
bool parseDurationMs(const std::string& text, long long& out_ms);
std::string humanBytes(size_t bytes);

// ---------- 持久化格式（与本仓库 v2 互通） ----------
inline constexpr char kDumpMagic[8] = {'M', 'I', 'N', 'I', 'R', 'D', 'B', '2'};
inline constexpr uint8_t kDumpVersion = 2;

uint32_t crc32(const void* data, size_t len, uint32_t seed = 0);
inline uint32_t crc32(const std::string& s, uint32_t seed = 0) {
    return crc32(s.data(), s.size(), seed);
}

}  // namespace redis
