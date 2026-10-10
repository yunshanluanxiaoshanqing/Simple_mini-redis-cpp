// Storage.cpp — v3 数据操作与过期时间轮
//
// 本文件只负责内存数据与过期语义；持久化见 Snapshot.cpp。

#include "Storage.h"

#include "Glob.h"

#include <algorithm>
#include <cctype>
#include <limits>

namespace redis {

namespace {

/// 解析十进制整数：整段必须为数字，拒绝 "12abc"、拒绝溢出。
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
        if (value > (std::numeric_limits<long long>::max() - digit) / 10) return false;
        value = value * 10 + digit;
    }
    out = negative ? -value : value;
    return true;
}

Reply makeInteger(long long v) {
    Reply r;
    r.code = Code::Integer;
    r.number = v;
    return r;
}

Reply makeArray(std::vector<std::string> items) {
    Reply r;
    r.code = Code::Array;
    r.items = std::move(items);
    return r;
}

Reply nullArray() {
    Reply r;
    r.code = Code::Nil;
    return r;
}

Reply wrongType() {
    Reply r;
    r.code = Code::WrongType;
    return r;
}

Reply bulk(std::string text) {
    Reply r;
    r.code = Code::Bulk;
    r.text = std::move(text);
    return r;
}

Reply outOfMemory() {
    Reply r;
    r.code = Code::OutOfMemory;
    return r;
}

}  // namespace

const char* typeName(const Val& value) {
    if (std::holds_alternative<std::string>(value)) return "string";
    if (std::holds_alternative<List>(value)) return "list";
    return "hash";
}

long long unixSeconds() { return static_cast<long long>(std::time(nullptr)); }

bool parseInt(const std::string& text, long long& out) { return parseDecimal(text, out); }

bool parseDurationMs(const std::string& text, long long& out_ms) {
    if (text.empty()) return false;
    std::string number = text;
    long long multiplier = 1000;  // 默认单位：秒
    if (text.size() > 2 && text.compare(text.size() - 2, 2, "ms") == 0) {
        multiplier = 1;
        number = text.substr(0, text.size() - 2);
    } else if (text.size() > 1 && (text.back() == 's' || text.back() == 'S')) {
        multiplier = 1000;
        number = text.substr(0, text.size() - 1);
    }
    long long value = 0;
    if (!parseDecimal(number, value)) return false;
    if (value > std::numeric_limits<long long>::max() / multiplier) return false;
    out_ms = value * multiplier;
    return true;
}

std::string humanBytes(size_t bytes) {
    static const char* units[] = {"B", "K", "M", "G", "T"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < sizeof(units) / sizeof(units[0])) {
        value /= 1024.0;
        ++unit;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f%s", value, units[unit]);
    return buf;
}

// ---------- 时间轮 ----------

long long Store::wheelSecond(ExpireAt at) {
    return std::chrono::duration_cast<std::chrono::seconds>(at.time_since_epoch()).count();
}

void Store::setExpireLocked(const std::string& key, Entry& entry, ExpireAt at) {
    clearExpireLocked(key, entry);
    entry.has_expire = true;
    entry.expire_at = at;
    const long long bucket = wheelSecond(at);
    expiry_buckets_[bucket].push_back(key);
    key_bucket_[key] = bucket;
}

void Store::clearExpireLocked(const std::string& key, Entry& entry) {
    entry.has_expire = false;
    auto it = key_bucket_.find(key);
    if (it != key_bucket_.end()) {
        auto bucketIt = expiry_buckets_.find(it->second);
        if (bucketIt != expiry_buckets_.end()) {
            auto& keys = bucketIt->second;
            keys.erase(std::remove(keys.begin(), keys.end(), key), keys.end());
            if (keys.empty()) expiry_buckets_.erase(bucketIt);
        }
        key_bucket_.erase(it);
    }
}

void Store::advanceExpiryWheelLocked() {
    const long long nowSecond = wheelSecond(now());
    if (nowSecond <= wheel_cursor_) return;
    // 只处理 cursor 之后、当前秒之前的桶；每个桶整体删除，均摊 O(1)。
    for (long long second = wheel_cursor_; second <= nowSecond; ++second) {
        auto it = expiry_buckets_.find(second);
        if (it == expiry_buckets_.end()) continue;
        for (const auto& key : it->second) {
            key_bucket_.erase(key);
            auto dataIt = data_.find(key);
            if (dataIt == data_.end()) continue;
            // 桶是按秒分的，同一桶内可能有的键尚未到期（毫秒级边界）
            if (dataIt->second.has_expire && now() >= dataIt->second.expire_at) {
                data_.erase(dataIt);
                expired_keys_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        expiry_buckets_.erase(it);
    }
    wheel_cursor_ = nowSecond;
}

// ---------- 内部查询 ----------

Entry* Store::lookup(const std::string& key) {
    advanceExpiryWheelLocked();
    auto it = data_.find(key);
    if (it == data_.end()) return nullptr;
    if (it->second.has_expire && now() >= it->second.expire_at) {
        data_.erase(it);
        expired_keys_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    return &it->second;
}

void Store::removeIfEmpty(const std::string& key) {
    auto it = data_.find(key);
    if (it == data_.end()) return;
    if (std::holds_alternative<List>(it->second.value) && std::get<List>(it->second.value).empty()) {
        data_.erase(it);
        key_bucket_.erase(key);
    } else if (std::holds_alternative<Hash>(it->second.value) &&
               std::get<Hash>(it->second.value).empty()) {
        data_.erase(it);
        key_bucket_.erase(key);
    }
}

size_t Store::memoryUsageLocked() const {
    size_t total = 0;
    for (const auto& kv : data_) {
        total += kv.first.size() + sizeof(Entry) + 32;
        if (std::holds_alternative<std::string>(kv.second.value)) {
            total += std::get<std::string>(kv.second.value).size();
        } else if (std::holds_alternative<List>(kv.second.value)) {
            for (const auto& item : std::get<List>(kv.second.value)) total += item.size() + 24;
        } else {
            for (const auto& fv : std::get<Hash>(kv.second.value))
                total += fv.first.size() + fv.second.size() + 48;
        }
    }
    return total;
}

bool Store::wouldExceedLimit(size_t incoming) const {
    const size_t limit = config_.maxmemory;
    if (limit == 0) return false;
    return memoryUsageLocked() + incoming > limit;
}

// ---------- 通用 ----------

void Store::flushAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    data_.clear();
    expiry_buckets_.clear();
    key_bucket_.clear();
}

size_t Store::size() {
    std::lock_guard<std::mutex> lock(mutex_);
    advanceExpiryWheelLocked();
    return data_.size();
}

std::vector<std::string> Store::keys(const std::string& pattern) {
    std::lock_guard<std::mutex> lock(mutex_);
    advanceExpiryWheelLocked();
    std::vector<std::string> result;
    for (auto it = data_.begin(); it != data_.end();) {
        if (it->second.has_expire && now() >= it->second.expire_at) {
            key_bucket_.erase(it->first);
            it = data_.erase(it);
            expired_keys_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (globMatch(pattern, it->first)) result.push_back(it->first);
        ++it;
    }
    std::sort(result.begin(), result.end());
    return result;
}

long long Store::exists(const std::vector<std::string>& keys_in) {
    std::lock_guard<std::mutex> lock(mutex_);
    long long count = 0;
    for (const auto& key : keys_in)
        if (lookup(key) != nullptr) ++count;
    return count;
}

long long Store::del(const std::vector<std::string>& keys_in) {
    std::lock_guard<std::mutex> lock(mutex_);
    long long count = 0;
    for (const auto& key : keys_in) {
        if (lookup(key) != nullptr) {
            data_.erase(key);
            key_bucket_.erase(key);
            ++count;
        }
    }
    return count;
}

std::string Store::type(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    const Entry* entry = lookup(key);
    return entry ? typeName(entry->value) : "none";
}

// ---------- 字符串 ----------

Reply Store::set(const std::string& key, const std::string& value, bool nx, bool xx, bool keep_ttl,
                 long long expire_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    const bool exists = (entry != nullptr);

    if (nx && exists) return nullArray();
    if (xx && !exists) return nullArray();

    if (wouldExceedLimit(key.size() + value.size() + 48)) return outOfMemory();

    if (!exists) {
        data_[key] = Entry{value, false, ExpireAt{}};
        entry = &data_[key];
    } else {
        entry->value = value;
    }

    if (expire_ms > 0) {
        setExpireLocked(key, *entry, now() + std::chrono::milliseconds(expire_ms));
    } else if (!keep_ttl) {
        clearExpireLocked(key, *entry);
    }
    return Reply{};
}

Reply Store::get(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry == nullptr) {
        misses_.fetch_add(1, std::memory_order_relaxed);
        return nullArray();
    }
    if (!std::holds_alternative<std::string>(entry->value)) return wrongType();
    hits_.fetch_add(1, std::memory_order_relaxed);
    return bulk(std::get<std::string>(entry->value));
}

Reply Store::append(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry != nullptr && !std::holds_alternative<std::string>(entry->value)) return wrongType();
    if (wouldExceedLimit(value.size())) return outOfMemory();
    if (entry == nullptr) {
        data_[key] = Entry{value, false, ExpireAt{}};
        return makeInteger(static_cast<long long>(value.size()));
    }
    std::string& target = std::get<std::string>(entry->value);
    target += value;  // TTL 保持不变（与 Redis 一致）
    return makeInteger(static_cast<long long>(target.size()));
}

Reply Store::incrBy(const std::string& key, long long delta) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry != nullptr && !std::holds_alternative<std::string>(entry->value)) return wrongType();
    long long current = 0;
    if (entry != nullptr) {
        const std::string& raw = std::get<std::string>(entry->value);
        if (!parseDecimal(raw, current)) {
            Reply r;
            r.code = Code::NotInteger;
            return r;
        }
    }
    // 溢出检查：C++ 有符号溢出是 UB，必须显式判断
    if ((delta > 0 && current > std::numeric_limits<long long>::max() - delta) ||
        (delta < 0 && current < std::numeric_limits<long long>::min() - delta)) {
        return Reply{Code::DbError, 0, "", {}, "increment or decrement would overflow"};
    }
    const long long next = current + delta;
    if (entry == nullptr) {
        data_[key] = Entry{std::to_string(next), false, ExpireAt{}};
    } else {
        std::get<std::string>(entry->value) = std::to_string(next);
    }
    return makeInteger(next);
}

// ---------- 过期 ----------

Reply Store::expire(const std::string& key, long long seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry == nullptr) return makeInteger(0);
    if (seconds <= 0) {
        data_.erase(key);
        key_bucket_.erase(key);
        return makeInteger(1);
    }
    setExpireLocked(key, *entry, now() + std::chrono::seconds(seconds));
    return makeInteger(1);
}

Reply Store::expireAt(const std::string& key, long long unix_seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry == nullptr) return makeInteger(0);
    const long long remain = unix_seconds - unixSeconds();
    if (remain <= 0) {
        data_.erase(key);
        key_bucket_.erase(key);
        return makeInteger(1);
    }
    setExpireLocked(key, *entry, now() + std::chrono::seconds(remain));
    return makeInteger(1);
}

Reply Store::ttl(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry == nullptr) return makeInteger(-2);
    if (!entry->has_expire) return makeInteger(-1);
    const auto remain =
        std::chrono::duration_cast<std::chrono::milliseconds>(entry->expire_at - now()).count();
    return makeInteger((remain + 999) / 1000);
}

Reply Store::pttl(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry == nullptr) return makeInteger(-2);
    if (!entry->has_expire) return makeInteger(-1);
    return makeInteger(
        std::chrono::duration_cast<std::chrono::milliseconds>(entry->expire_at - now()).count());
}

Reply Store::persist(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry == nullptr || !entry->has_expire) return makeInteger(0);
    clearExpireLocked(key, *entry);
    return makeInteger(1);
}

Reply Store::rename(const std::string& old_key, const std::string& new_key) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* src = lookup(old_key);
    if (src == nullptr) {
        Reply r;
        r.code = Code::NoSuchKey;
        return r;
    }
    if (old_key == new_key) return Reply{};
    Entry moved = *src;
    data_.erase(old_key);
    key_bucket_.erase(old_key);
    data_[new_key] = std::move(moved);
    // 时间轮记录要跟着改名
    if (data_[new_key].has_expire) {
        Entry& entry = data_[new_key];
        const ExpireAt at = entry.expire_at;
        entry.has_expire = false;
        setExpireLocked(new_key, entry, at);
    }
    return Reply{};
}

// ---------- 列表 ----------

Reply Store::lpush(const std::string& key, const std::vector<std::string>& values) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry != nullptr && !std::holds_alternative<List>(entry->value)) return wrongType();
    size_t incoming = 0;
    for (const auto& v : values) incoming += v.size() + 24;
    if (wouldExceedLimit(incoming)) return outOfMemory();
    if (entry == nullptr) {
        data_[key] = Entry{List{}, false, ExpireAt{}};
        entry = &data_[key];
    }
    List& list = std::get<List>(entry->value);
    for (const auto& v : values) list.insert(list.begin(), v);
    return makeInteger(static_cast<long long>(list.size()));
}

Reply Store::rpush(const std::string& key, const std::vector<std::string>& values) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry != nullptr && !std::holds_alternative<List>(entry->value)) return wrongType();
    size_t incoming = 0;
    for (const auto& v : values) incoming += v.size() + 24;
    if (wouldExceedLimit(incoming)) return outOfMemory();
    if (entry == nullptr) {
        data_[key] = Entry{List{}, false, ExpireAt{}};
        entry = &data_[key];
    }
    List& list = std::get<List>(entry->value);
    for (const auto& v : values) list.push_back(v);
    return makeInteger(static_cast<long long>(list.size()));
}

Reply Store::lpop(const std::string& key, long long count) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry == nullptr) return nullArray();
    if (!std::holds_alternative<List>(entry->value)) return wrongType();
    List& list = std::get<List>(entry->value);
    if (list.empty()) return nullArray();
    const long long n = std::min<long long>(count, static_cast<long long>(list.size()));
    std::vector<std::string> popped(list.begin(), list.begin() + static_cast<size_t>(n));
    list.erase(list.begin(), list.begin() + static_cast<size_t>(n));
    removeIfEmpty(key);
    if (count == 1 && popped.size() == 1) return bulk(std::move(popped[0]));
    return makeArray(std::move(popped));
}

Reply Store::rpop(const std::string& key, long long count) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry == nullptr) return nullArray();
    if (!std::holds_alternative<List>(entry->value)) return wrongType();
    List& list = std::get<List>(entry->value);
    if (list.empty()) return nullArray();
    const long long n = std::min<long long>(count, static_cast<long long>(list.size()));
    std::vector<std::string> popped;
    popped.reserve(static_cast<size_t>(n));
    for (long long i = 0; i < n; ++i) {
        popped.push_back(std::move(list.back()));
        list.pop_back();
    }
    removeIfEmpty(key);
    if (count == 1 && popped.size() == 1) return bulk(std::move(popped[0]));
    return makeArray(std::move(popped));
}

Reply Store::llen(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry == nullptr) return makeInteger(0);
    if (!std::holds_alternative<List>(entry->value)) return wrongType();
    return makeInteger(static_cast<long long>(std::get<List>(entry->value).size()));
}

Reply Store::lget(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry == nullptr) return makeArray({});
    if (!std::holds_alternative<List>(entry->value)) return wrongType();
    return makeArray(std::get<List>(entry->value));
}

Reply Store::lrange(const std::string& key, long long start, long long stop) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry == nullptr) return makeArray({});
    if (!std::holds_alternative<List>(entry->value)) return wrongType();
    const List& list = std::get<List>(entry->value);
    const long long len = static_cast<long long>(list.size());
    if (start < 0) start += len;
    if (stop < 0) stop += len;
    if (start < 0) start = 0;
    if (stop >= len) stop = len - 1;
    if (start > stop || start >= len) return makeArray({});
    return makeArray(List(list.begin() + start, list.begin() + stop + 1));
}

Reply Store::lindex(const std::string& key, long long index) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry == nullptr) return nullArray();
    if (!std::holds_alternative<List>(entry->value)) return wrongType();
    const List& list = std::get<List>(entry->value);
    const long long len = static_cast<long long>(list.size());
    if (index < 0) index += len;
    if (index < 0 || index >= len) return nullArray();
    return bulk(list[static_cast<size_t>(index)]);
}

Reply Store::lset(const std::string& key, long long index, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry == nullptr) {
        Reply r;
        r.code = Code::NoSuchKey;
        return r;
    }
    if (!std::holds_alternative<List>(entry->value)) return wrongType();
    List& list = std::get<List>(entry->value);
    const long long len = static_cast<long long>(list.size());
    if (index < 0) index += len;
    if (index < 0 || index >= len) {
        Reply r;
        r.code = Code::IndexOutOfRange;
        return r;
    }
    list[static_cast<size_t>(index)] = value;
    return Reply{};
}

Reply Store::lrem(const std::string& key, long long count, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry == nullptr) return makeInteger(0);
    if (!std::holds_alternative<List>(entry->value)) return wrongType();
    List& list = std::get<List>(entry->value);
    long long removed = 0;

    if (count == 0) {
        auto new_end = std::remove(list.begin(), list.end(), value);
        removed = static_cast<long long>(std::distance(new_end, list.end()));
        list.erase(new_end, list.end());
    } else if (count > 0) {
        for (auto it = list.begin(); it != list.end() && removed < count;) {
            if (*it == value) {
                it = list.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
    } else {
        const long long limit = -count;
        for (auto rit = list.rbegin(); rit != list.rend() && removed < limit;) {
            if (*rit == value) {
                auto fwd = std::next(rit).base();
                fwd = list.erase(fwd);
                ++removed;
                rit = std::reverse_iterator<List::iterator>(fwd);
            } else {
                ++rit;
            }
        }
    }
    removeIfEmpty(key);
    return makeInteger(removed);
}

// ---------- 哈希 ----------

Reply Store::hset(const std::string& key,
                  const std::vector<std::pair<std::string, std::string>>& fv) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry != nullptr && !std::holds_alternative<Hash>(entry->value)) return wrongType();
    size_t incoming = 0;
    for (const auto& kv : fv) incoming += kv.first.size() + kv.second.size() + 48;
    if (wouldExceedLimit(incoming)) return outOfMemory();
    if (entry == nullptr) {
        data_[key] = Entry{Hash{}, false, ExpireAt{}};
        entry = &data_[key];
    }
    Hash& hash = std::get<Hash>(entry->value);
    long long created = 0;
    for (const auto& kv : fv) {
        if (hash.find(kv.first) == hash.end()) ++created;
        hash[kv.first] = kv.second;
    }
    return makeInteger(created);
}

Reply Store::hmset(const std::string& key,
                   const std::vector<std::pair<std::string, std::string>>& fv) {
    Reply r = hset(key, fv);
    if (r.code == Code::Integer) return Reply{};  // HMSET 恒返回 +OK
    return r;
}

Reply Store::hget(const std::string& key, const std::string& field) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry == nullptr) {
        misses_.fetch_add(1, std::memory_order_relaxed);
        return nullArray();
    }
    if (!std::holds_alternative<Hash>(entry->value)) return wrongType();
    const Hash& hash = std::get<Hash>(entry->value);
    auto it = hash.find(field);
    if (it == hash.end()) {
        misses_.fetch_add(1, std::memory_order_relaxed);
        return nullArray();
    }
    hits_.fetch_add(1, std::memory_order_relaxed);
    return bulk(it->second);
}

Reply Store::hexists(const std::string& key, const std::string& field) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry == nullptr) return makeInteger(0);
    if (!std::holds_alternative<Hash>(entry->value)) return wrongType();
    return makeInteger(std::get<Hash>(entry->value).count(field) > 0 ? 1 : 0);
}

Reply Store::hdel(const std::string& key, const std::vector<std::string>& fields) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry == nullptr) return makeInteger(0);
    if (!std::holds_alternative<Hash>(entry->value)) return wrongType();
    Hash& hash = std::get<Hash>(entry->value);
    long long removed = 0;
    for (const auto& f : fields) removed += static_cast<long long>(hash.erase(f));
    removeIfEmpty(key);
    return makeInteger(removed);
}

Reply Store::hgetall(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry == nullptr) return makeArray({});
    if (!std::holds_alternative<Hash>(entry->value)) return wrongType();
    std::vector<std::pair<std::string, std::string>> sorted(std::get<Hash>(entry->value).begin(),
                                                           std::get<Hash>(entry->value).end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<std::string> flat;
    flat.reserve(sorted.size() * 2);
    for (const auto& kv : sorted) {
        flat.push_back(kv.first);
        flat.push_back(kv.second);
    }
    return makeArray(std::move(flat));
}

Reply Store::hkeys(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry == nullptr) return makeArray({});
    if (!std::holds_alternative<Hash>(entry->value)) return wrongType();
    std::vector<std::string> fields;
    for (const auto& kv : std::get<Hash>(entry->value)) fields.push_back(kv.first);
    std::sort(fields.begin(), fields.end());
    return makeArray(std::move(fields));
}

Reply Store::hvals(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry == nullptr) return makeArray({});
    if (!std::holds_alternative<Hash>(entry->value)) return wrongType();
    std::vector<std::string> values;
    for (const auto& kv : std::get<Hash>(entry->value)) values.push_back(kv.second);
    std::sort(values.begin(), values.end());
    return makeArray(std::move(values));
}

Reply Store::hlen(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry == nullptr) return makeInteger(0);
    if (!std::holds_alternative<Hash>(entry->value)) return wrongType();
    return makeInteger(static_cast<long long>(std::get<Hash>(entry->value).size()));
}

// ---------- 统计与配置 ----------

Stats Store::stats() const {
    Stats s;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        s.keys = data_.size();
        s.used_memory = memoryUsageLocked();
    }
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        s.maxmemory = config_.maxmemory;
    }
    s.hits = hits_.load();
    s.misses = misses_.load();
    s.expired_keys = expired_keys_.load();
    s.commands = commands_.load();
    s.connections = connections_.load();
    s.rejected_connections = rejected_.load();
    s.snapshots = snapshots_.load();
    s.snapshot_errors = snapshot_errors_.load();
    return s;
}

size_t Store::approximateMemory() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return memoryUsageLocked();
}

bool Store::updateConfig(const RuntimeConfig& next) {
    if (next.max_clients < 1 || next.max_clients > 100000) return false;
    if (next.idle_timeout_ms < 0 || next.idle_timeout_ms > 24 * 3600 * 1000) return false;
    if (next.dump_interval_ms < 0 || next.dump_interval_ms > 24 * 3600 * 1000) return false;
    std::lock_guard<std::mutex> lock(config_mutex_);
    config_ = next;
    return true;
}

}  // namespace redis
