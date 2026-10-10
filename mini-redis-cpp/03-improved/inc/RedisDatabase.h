#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <chrono>
#include <algorithm>
#include <iterator>

// 线程安全的内存数据库（string / list / hash + TTL）。
// 注意：purgeExpired() 要求调用方已持有 db_mutex，仅供内部使用。
class RedisDatabase {
public:
    static RedisDatabase& getInstance();

    bool flushAll();
    void set(const std::string& key, const std::string& value);
    bool get(const std::string& key, std::string& value);
    std::vector<std::string> keys();
    std::string type(const std::string& key);
    bool del(const std::string& key);
    bool expire(const std::string& key, int seconds);
    bool rename(const std::string& oldkey, const std::string& newkey);

    std::vector<std::string> lget(const std::string& key);
    int64_t llen(const std::string& key);
    void lpush(const std::string& key, const std::string& value);
    void rpush(const std::string& key, const std::string& value);
    bool lpop(const std::string& key, std::string& value);
    bool rpop(const std::string& key, std::string& value);
    int lrem(const std::string& key, int count, const std::string& value);
    bool lindex(const std::string& key, int index, std::string& value);
    bool lset(const std::string& key, int index, const std::string& value);

    // 返回 true 表示 field 为新建（Redis HSET 语义：新 field 返回 1，更新返回 0）
    bool hset(const std::string& key, const std::string& field, const std::string& value);
    bool hget(const std::string& key, const std::string& field, std::string& value);
    bool hexists(const std::string& key, const std::string& field);
    bool hdel(const std::string& key, const std::string& field);
    std::unordered_map<std::string, std::string> hgetAll(const std::string& key);
    std::vector<std::string> hkeys(const std::string& key);
    std::vector<std::string> hvals(const std::string& key);
    int64_t hlen(const std::string& key);
    bool hmset(const std::string& key, const std::vector<std::pair<std::string, std::string>>& fields_values);

    // 持久化：dump 写出 / load 读回（自定义长度前缀二进制安全格式，
    // 键值可包含空格换行，并保存剩余 TTL）
    bool dump(const std::string& filename);
    bool load(const std::string& filename);

private:
    RedisDatabase() = default;
    ~RedisDatabase() = default;
    RedisDatabase(const RedisDatabase&) = delete;
    RedisDatabase& operator=(const RedisDatabase&) = delete;

    // 清理已过期 key。调用方必须已持有 db_mutex。
    void purgeExpired();

    // 列表为空时删除 key（与 Redis 行为一致：空 key 不存在）
    void removeIfEmpty(const std::string& key);

    std::mutex db_mutex;
    // kv: key-value; l: list; h: hash
    std::unordered_map<std::string, std::string> kv_store;
    std::unordered_map<std::string, std::vector<std::string>> l_store;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> h_store;

    std::unordered_map<std::string, std::chrono::steady_clock::time_point> expiry_map;
};
