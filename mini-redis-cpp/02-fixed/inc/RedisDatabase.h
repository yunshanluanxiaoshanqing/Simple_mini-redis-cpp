#pragma once

#include<string>
#include<vector>
#include<mutex>
#include<fstream>
#include<sstream>
#include<unordered_map>
#include<chrono>
#include<algorithm>
#include<iterator>

#include<unistd.h>

class RedisDatabase
{
public:
    static RedisDatabase& getInstance();

    bool flushAll();
    void set(const std::string& key, const std::string& value);
    bool get(const std::string& key, std::string& value);
    std::vector<std::string> keys();
    std::string type(const std::string& key);
    bool del(const std::string& key);
    bool expire(const std::string& key, int seconds);
    void purgeExpired();
    bool rename(const std::string& oldkey, const std::string& newkey);

    std::vector<std::string> lget(const std::string& key);
    ssize_t llen(const std::string& key);
    void lpush(const std::string& key, const std::string& value);
    void rpush(const std::string& key, const std::string& value);
    bool lpop(const std::string& key, std::string& value);
    bool rpop(const std::string& key, std::string& value);
    int lrem(const std::string& key, int count,const std::string& value);
    bool lindex(const std::string& key, int index, std::string& value);
    bool lset(const std::string& key, int index,const std::string& value);

    bool hset(const std::string& key, const std::string& field, const std::string& value);
    bool hget(const std::string& key, const std::string& field, std::string& value);
    bool hexists(const std::string& key, const std::string& field);
    bool hdel(const std::string& key, const std::string& field);
    std::unordered_map<std::string, std::string> hgetAll(const std::string& key);
    std::vector<std::string> hkeys(const std::string& key);
    std::vector<std::string> hvals(const std::string& key);
    ssize_t hlen(const std::string& key);
    bool hmset(const std::string& key, const std::vector<std::pair<std::string, std::string>>& fields_values);

    bool dump(const std::string& filename);
    bool load(const std::string& filename);

private:
    RedisDatabase()=default;
    ~RedisDatabase()=default;
    RedisDatabase(const RedisDatabase&)=delete;
    RedisDatabase& operator=(const RedisDatabase&)=delete;

    std::mutex db_mutex;
    //kv: key-value
    //l: list
    //h: hash
    std::unordered_map<std::string, std::string> kv_store;
    std::unordered_map<std::string, std::vector<std::string>> l_store;
    std::unordered_map<std::string, std::unordered_map<std::string,std::string>> h_store;

    std::unordered_map<std::string, std::chrono::steady_clock::time_point> expiry_map;
};
