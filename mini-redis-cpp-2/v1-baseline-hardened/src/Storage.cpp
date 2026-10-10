#include "Storage.h"

#include "Glob.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <limits>
#include <sstream>

#ifndef _WIN32
#include <unistd.h>
#endif

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

}  // namespace

const char* typeName(const Val& value) {
    if (std::holds_alternative<std::string>(value)) return "string";
    if (std::holds_alternative<List>(value)) return "list";
    return "hash";
}

long long unixSeconds() {
    return static_cast<long long>(std::time(nullptr));
}

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

// ---------- 内部 ----------

Entry* Store::lookup(const std::string& key) {
    auto it = data_.find(key);
    if (it == data_.end()) return nullptr;
    if (it->second.has_expire && now() >= it->second.expire_at) {
        data_.erase(it);
        return nullptr;
    }
    return &it->second;
}

void Store::removeIfEmpty(const std::string& key) {
    auto it = data_.find(key);
    if (it == data_.end()) return;
    if (std::holds_alternative<List>(it->second.value) && std::get<List>(it->second.value).empty())
        data_.erase(it);
    else if (std::holds_alternative<Hash>(it->second.value) &&
             std::get<Hash>(it->second.value).empty())
        data_.erase(it);
}

// ---------- 通用 ----------

void Store::flushAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    data_.clear();
}

size_t Store::size() {
    std::lock_guard<std::mutex> lock(mutex_);
    // 惰性清理：只清过期项，不做全量重建
    for (auto it = data_.begin(); it != data_.end();) {
        if (it->second.has_expire && now() >= it->second.expire_at)
            it = data_.erase(it);
        else
            ++it;
    }
    return data_.size();
}

std::vector<std::string> Store::keys(const std::string& pattern) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    for (auto it = data_.begin(); it != data_.end();) {
        if (it->second.has_expire && now() >= it->second.expire_at) {
            it = data_.erase(it);
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
        if (lookup(key) != nullptr) {  // 过期 key 不算删除
            data_.erase(key);
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

    if (nx && exists) return nullArray();   // SET NX 已存在 → nil
    if (xx && !exists) return nullArray();  // SET XX 不存在 → nil

    if (!exists) {
        data_[key] = Entry{value, false, ExpireAt{}};
        entry = &data_[key];
    } else {
        entry->value = value;  // 覆盖任意旧类型：不再残留 list/hash
    }

    if (expire_ms > 0) {
        entry->has_expire = true;
        entry->expire_at = now() + std::chrono::milliseconds(expire_ms);
    } else if (!keep_ttl) {
        entry->has_expire = false;  // Redis 语义：SET 默认清除 TTL；KEEPTTL 才保留
    }
    return Reply{};
}

Reply Store::get(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry == nullptr) return nullArray();
    if (!std::holds_alternative<std::string>(entry->value)) {
        Reply r;
        r.code = Code::WrongType;
        return r;
    }
    Reply r;
    r.code = Code::Bulk;
    r.text = std::get<std::string>(entry->value);
    return r;
}

// ---------- 过期 ----------

Reply Store::expire(const std::string& key, long long seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry == nullptr) return makeInteger(0);
    if (seconds <= 0) {  // Redis：非正 TTL 立即删除
        data_.erase(key);
        return makeInteger(1);
    }
    entry->has_expire = true;
    entry->expire_at = now() + std::chrono::seconds(seconds);
    return makeInteger(1);
}

Reply Store::expireAt(const std::string& key, long long unix_seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry == nullptr) return makeInteger(0);
    const long long remain = unix_seconds - unixSeconds();
    if (remain <= 0) {
        data_.erase(key);
        return makeInteger(1);
    }
    entry->has_expire = true;
    entry->expire_at = now() + std::chrono::seconds(remain);
    return makeInteger(1);
}

Reply Store::ttl(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry == nullptr) return makeInteger(-2);
    if (!entry->has_expire) return makeInteger(-1);
    const auto remain =
        std::chrono::duration_cast<std::chrono::milliseconds>(entry->expire_at - now()).count();
    return makeInteger((remain + 999) / 1000);  // 向上取整，避免刚设置就显示 0
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
    entry->has_expire = false;
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
    Entry moved = *src;         // 值 + TTL 一起搬走
    data_.erase(old_key);
    data_[new_key] = std::move(moved);  // 覆盖目标的一切旧状态
    return Reply{};
}

// ---------- 列表 ----------

Reply Store::lpush(const std::string& key, const std::vector<std::string>& values) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry != nullptr && !std::holds_alternative<List>(entry->value)) {
        Reply r;
        r.code = Code::WrongType;
        return r;
    }
    if (entry == nullptr) {
        data_[key] = Entry{List{}, false, ExpireAt{}};
        entry = &data_[key];
    }
    List& list = std::get<List>(entry->value);
    for (const auto& v : values) list.insert(list.begin(), v);  // LPUSH 逐个头插：顺序与 Redis 一致
    return makeInteger(static_cast<long long>(list.size()));
}
Reply Store::rpush(const std::string& key, const std::vector<std::string>& values) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry != nullptr && !std::holds_alternative<List>(entry->value)) {
        Reply r;
        r.code = Code::WrongType;
        return r;
    }
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
    if (!std::holds_alternative<List>(entry->value)) {
        Reply r;
        r.code = Code::WrongType;
        return r;
    }
    List& list = std::get<List>(entry->value);
    if (list.empty()) return nullArray();
    const long long n = std::min<long long>(count, static_cast<long long>(list.size()));
    std::vector<std::string> popped(list.begin(), list.begin() + static_cast<size_t>(n));
    list.erase(list.begin(), list.begin() + static_cast<size_t>(n));
    removeIfEmpty(key);
    if (count == 1 && popped.size() == 1) {
        Reply r;
        r.code = Code::Bulk;
        r.text = std::move(popped[0]);
        return r;
    }
    return makeArray(std::move(popped));
}

Reply Store::rpop(const std::string& key, long long count) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry == nullptr) return nullArray();
    if (!std::holds_alternative<List>(entry->value)) {
        Reply r;
        r.code = Code::WrongType;
        return r;
    }
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
    if (count == 1 && popped.size() == 1) {
        Reply r;
        r.code = Code::Bulk;
        r.text = std::move(popped[0]);
        return r;
    }
    return makeArray(std::move(popped));
}

Reply Store::llen(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry == nullptr) return makeInteger(0);
    if (!std::holds_alternative<List>(entry->value)) {
        Reply r;
        r.code = Code::WrongType;
        return r;
    }
    return makeInteger(static_cast<long long>(std::get<List>(entry->value).size()));
}

Reply Store::lget(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry == nullptr) return makeArray({});
    if (!std::holds_alternative<List>(entry->value)) {
        Reply r;
        r.code = Code::WrongType;
        return r;
    }
    return makeArray(std::get<List>(entry->value));
}

Reply Store::lrange(const std::string& key, long long start, long long stop) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry == nullptr) return makeArray({});
    if (!std::holds_alternative<List>(entry->value)) {
        Reply r;
        r.code = Code::WrongType;
        return r;
    }
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
    if (!std::holds_alternative<List>(entry->value)) {
        Reply r;
        r.code = Code::WrongType;
        return r;
    }
    const List& list = std::get<List>(entry->value);
    const long long len = static_cast<long long>(list.size());
    if (index < 0) index += len;
    if (index < 0 || index >= len) return nullArray();
    Reply r;
    r.code = Code::Bulk;
    r.text = list[static_cast<size_t>(index)];
    return r;
}

Reply Store::lset(const std::string& key, long long index, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry == nullptr) {
        Reply r;
        r.code = Code::NoSuchKey;
        return r;
    }
    if (!std::holds_alternative<List>(entry->value)) {
        Reply r;
        r.code = Code::WrongType;
        return r;
    }
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
    if (!std::holds_alternative<List>(entry->value)) {
        Reply r;
        r.code = Code::WrongType;
        return r;
    }
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
                auto fwd = std::next(rit).base();  // 指向被删元素
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

Reply Store::hset(const std::string& key, const std::vector<std::pair<std::string, std::string>>& fv) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry != nullptr && !std::holds_alternative<Hash>(entry->value)) {
        Reply r;
        r.code = Code::WrongType;
        return r;
    }
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
    if (entry == nullptr) return nullArray();
    if (!std::holds_alternative<Hash>(entry->value)) {
        Reply r;
        r.code = Code::WrongType;
        return r;
    }
    const Hash& hash = std::get<Hash>(entry->value);
    auto it = hash.find(field);
    if (it == hash.end()) return nullArray();
    Reply r;
    r.code = Code::Bulk;
    r.text = it->second;
    return r;
}

Reply Store::hexists(const std::string& key, const std::string& field) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry == nullptr) return makeInteger(0);
    if (!std::holds_alternative<Hash>(entry->value)) {
        Reply r;
        r.code = Code::WrongType;
        return r;
    }
    return makeInteger(std::get<Hash>(entry->value).count(field) > 0 ? 1 : 0);
}

Reply Store::hdel(const std::string& key, const std::vector<std::string>& fields) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry == nullptr) return makeInteger(0);
    if (!std::holds_alternative<Hash>(entry->value)) {
        Reply r;
        r.code = Code::WrongType;
        return r;
    }
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
    if (!std::holds_alternative<Hash>(entry->value)) {
        Reply r;
        r.code = Code::WrongType;
        return r;
    }
    // 排序输出：原实现依赖 unordered_map 迭代顺序，HGETALL 结果不稳定
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
    if (!std::holds_alternative<Hash>(entry->value)) {
        Reply r;
        r.code = Code::WrongType;
        return r;
    }
    std::vector<std::string> fields;
    for (const auto& kv : std::get<Hash>(entry->value)) fields.push_back(kv.first);
    std::sort(fields.begin(), fields.end());
    return makeArray(std::move(fields));
}

Reply Store::hvals(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry == nullptr) return makeArray({});
    if (!std::holds_alternative<Hash>(entry->value)) {
        Reply r;
        r.code = Code::WrongType;
        return r;
    }
    std::vector<std::string> values;
    for (const auto& kv : std::get<Hash>(entry->value)) values.push_back(kv.second);
    std::sort(values.begin(), values.end());
    return makeArray(std::move(values));
}

Reply Store::hlen(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* entry = lookup(key);
    if (entry == nullptr) return makeInteger(0);
    if (!std::holds_alternative<Hash>(entry->value)) {
        Reply r;
        r.code = Code::WrongType;
        return r;
    }
    return makeInteger(static_cast<long long>(std::get<Hash>(entry->value).size()));
}

// ---------- 持久化 ----------
//
// 格式（header 为 ASCII 行，随后是精确长度的原始字节，二进制安全）：
//   K <klen> <vlen> <ttl_ms>\n<key><value>
//   L <klen> <count> <ttl_ms>\n<key>(<elen>\n<elem>)*count
//   H <klen> <count> <ttl_ms>\n<key>(<flen> <vlen>\n<field><value>)*count
//
// 改进点（相对 01-original）：
//   * 长度前缀 → 值可含空格/换行（原始格式按空白切分，值含空格即损坏）
//   * 每个 key 记录剩余 TTL → 重启后过期时间延续（原实现 TTL 完全不落盘）
//   * 写临时文件 + rename 原子替换 → 中途崩溃不会留下半个文件
//   * 读不出来时返回 Corrupt 而不是静默装了半个库

bool Store::save(const std::string& path) {
    std::string payload;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto ttlOf = [this](const Entry& e) -> long long {
            if (!e.has_expire) return 0;
            const auto remain =
                std::chrono::duration_cast<std::chrono::milliseconds>(e.expire_at - now()).count();
            return remain > 0 ? remain : 0;
        };

        for (const auto& kv : data_) {
            const long long ttl = ttlOf(kv.second);
            if (kv.second.has_expire && ttl <= 0) continue;  // 已过期：不落盘
            if (std::holds_alternative<std::string>(kv.second.value)) {
                const std::string& v = std::get<std::string>(kv.second.value);
                payload += "K " + std::to_string(kv.first.size()) + " " + std::to_string(v.size()) +
                           " " + std::to_string(ttl) + "\n";
                payload += kv.first;
                payload += v;
            } else if (std::holds_alternative<List>(kv.second.value)) {
                const List& l = std::get<List>(kv.second.value);
                payload += "L " + std::to_string(kv.first.size()) + " " + std::to_string(l.size()) +
                           " " + std::to_string(ttl) + "\n";
                payload += kv.first;
                for (const auto& item : l) {
                    payload += std::to_string(item.size()) + "\n";
                    payload += item;
                }
            } else {
                const Hash& h = std::get<Hash>(kv.second.value);
                payload += "H " + std::to_string(kv.first.size()) + " " + std::to_string(h.size()) +
                           " " + std::to_string(ttl) + "\n";
                payload += kv.first;
                for (const auto& fv : h) {
                    payload += std::to_string(fv.first.size()) + " " + std::to_string(fv.second.size()) +
                               "\n";
                    payload += fv.first;
                    payload += fv.second;
                }
            }
        }
    }

    const std::string tmp = path + ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        ofs.flush();
        if (!ofs.good()) {
            ofs.close();
            std::remove(tmp.c_str());
            return false;
        }
#ifndef _WIN32
        // 让数据真正落盘，再 rename：崩溃后不会出现"文件在但内容在缓存里"的情况
        if (FILE* fp = std::fopen(tmp.c_str(), "rb")) {
            ::fsync(::fileno(fp));
            std::fclose(fp);
        }
#endif
    }

#ifdef _WIN32
    std::remove(path.c_str());  // Windows 的 rename 不覆盖已存在文件
#endif
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

LoadResult Store::load(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return LoadResult::NotFound;
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    if (!ifs.good() && !ifs.eof()) return LoadResult::IoError;

    std::unordered_map<std::string, Entry> loaded;
    size_t pos = 0;
    const size_t total = content.size();

    auto readLine = [&](std::string& line) -> bool {
        const size_t nl = content.find('\n', pos);
        if (nl == std::string::npos) return false;
        line = content.substr(pos, nl - pos);
        pos = nl + 1;
        return true;
    };
    auto readExact = [&](size_t n, std::string& out) -> bool {
        if (pos + n > total) return false;
        out.assign(content, pos, n);
        pos += n;
        return true;
    };

    std::string header;
    while (pos < total) {
        if (!readLine(header)) return LoadResult::Corrupt;
        if (header.empty()) continue;
        std::istringstream iss(header);
        char type = 0;
        size_t klen = 0, count = 0, vlen = 0;
        long long ttl_ms = 0;
        iss >> type;
        if (type == 'K') {
            if (!(iss >> klen >> vlen >> ttl_ms)) return LoadResult::Corrupt;
            std::string key, value;
            if (!readExact(klen, key) || !readExact(vlen, value)) return LoadResult::Corrupt;
            Entry e{std::move(value), false, ExpireAt{}};
            if (ttl_ms > 0) {
                e.has_expire = true;
                e.expire_at = now() + std::chrono::milliseconds(ttl_ms);
            }
            loaded[std::move(key)] = std::move(e);
        } else if (type == 'L') {
            if (!(iss >> klen >> count >> ttl_ms)) return LoadResult::Corrupt;
            std::string key;
            if (!readExact(klen, key)) return LoadResult::Corrupt;
            List list;
            list.reserve(count);
            for (size_t i = 0; i < count; ++i) {
                std::string lenLine;
                if (!readLine(lenLine)) return LoadResult::Corrupt;
                long long elen = 0;
                if (!parseInt(lenLine, elen) || elen < 0) return LoadResult::Corrupt;
                std::string item;
                if (!readExact(static_cast<size_t>(elen), item)) return LoadResult::Corrupt;
                list.push_back(std::move(item));
            }
            Entry e{std::move(list), false, ExpireAt{}};
            if (ttl_ms > 0) {
                e.has_expire = true;
                e.expire_at = now() + std::chrono::milliseconds(ttl_ms);
            }
            loaded[std::move(key)] = std::move(e);
        } else if (type == 'H') {
            if (!(iss >> klen >> count >> ttl_ms)) return LoadResult::Corrupt;
            std::string key;
            if (!readExact(klen, key)) return LoadResult::Corrupt;
            Hash hash;
            for (size_t i = 0; i < count; ++i) {
                std::string lenLine;
                if (!readLine(lenLine)) return LoadResult::Corrupt;
                std::istringstream ls(lenLine);
                size_t flen = 0, fvlen = 0;
                if (!(ls >> flen >> fvlen)) return LoadResult::Corrupt;
                std::string field, value;
                if (!readExact(flen, field) || !readExact(fvlen, value)) return LoadResult::Corrupt;
                hash[std::move(field)] = std::move(value);
            }
            Entry e{std::move(hash), false, ExpireAt{}};
            if (ttl_ms > 0) {
                e.has_expire = true;
                e.expire_at = now() + std::chrono::milliseconds(ttl_ms);
            }
            loaded[std::move(key)] = std::move(e);
        } else {
            return LoadResult::Corrupt;  // 未知记录类型
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    data_ = std::move(loaded);
    return LoadResult::Ok;
}

}  // namespace redis
