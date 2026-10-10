// Snapshot.cpp — v3 持久化：锁内快照 + 锁外落盘 + CRC32 校验
//
// 与 v1/v2 的关键差别：
//   v1/v2 的 save() 在持有数据库锁的情况下完成"序列化 + 写盘 + 校验"，
//   库越大、磁盘越慢，所有客户端被阻塞得越久。
//   v3 把它拆成两段：
//     ① 持锁：把数据序列化成内存里的 payload（纯拷贝，无 I/O）；
//     ② 放锁：写临时文件 → 回读校验 → 原子替换。
//   代价是快照期间的内存峰值约等于数据量的两倍，换来的是"落盘不阻塞读"。

#include "Storage.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace redis {

uint32_t crc32(const void* data, size_t len, uint32_t seed) {
    static uint32_t table[256];
    static std::once_flag init_flag;
    std::call_once(init_flag, [] {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
    });
    uint32_t crc = ~seed;
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < len; ++i) crc = table[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

SaveResult Store::save(const std::string& path) {
    // 防止两次落盘重叠：后台线程与 SHUTDOWN/SAVE 可能同时触发。
    bool expected = false;
    if (!saving_.compare_exchange_strong(expected, true)) return SaveResult::Busy;

    // ① 只在锁内做序列化（exportSnapshot 内部持锁，纯内存操作，不碰磁盘）
    const std::string file = exportSnapshot();

    // ② 锁外完成 I/O
    bool ok = false;
    const std::string tmp = path + ".tmp";
    do {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) break;
        ofs.write(file.data(), static_cast<std::streamsize>(file.size()));
        ofs.flush();
        if (!ofs.good()) break;
        ofs.close();
#ifndef _WIN32
        if (FILE* fp = std::fopen(tmp.c_str(), "rb")) {
            ::fsync(::fileno(fp));
            std::fclose(fp);
        }
#endif
        // 回读校验：确认写到盘上的字节与内存一致
        {
            std::ifstream check(tmp, std::ios::binary);
            std::string roundtrip((std::istreambuf_iterator<char>(check)),
                                  std::istreambuf_iterator<char>());
            if (roundtrip != file) break;
        }
#ifdef _WIN32
        std::remove(path.c_str());
#endif
        if (std::rename(tmp.c_str(), path.c_str()) != 0) break;
        ok = true;
    } while (false);

    if (!ok) std::remove(tmp.c_str());
    saving_.store(false);
    noteSnapshot(ok);
    return ok ? SaveResult::Ok : SaveResult::IoError;
}

LoadResult Store::load(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return LoadResult::NotFound;
    const std::string content((std::istreambuf_iterator<char>(ifs)),
                              std::istreambuf_iterator<char>());
    if (!ifs.good() && !ifs.eof()) return LoadResult::IoError;

    const size_t total = content.size();
    size_t record_begin = 0;
    size_t record_end = total;
    bool legacy = true;

    if (total >= sizeof(kDumpMagic) + 1 &&
        std::memcmp(content.data(), kDumpMagic, sizeof(kDumpMagic)) == 0) {
        legacy = false;
        if (static_cast<uint8_t>(content[sizeof(kDumpMagic)]) != kDumpVersion)
            return LoadResult::Corrupt;
        if (total < sizeof(kDumpMagic) + 1 + 4) return LoadResult::Corrupt;
        record_begin = sizeof(kDumpMagic) + 1;
        record_end = total - 4;
        uint32_t stored = 0;
        for (int i = 0; i < 4; ++i)
            stored |= static_cast<uint32_t>(static_cast<unsigned char>(content[record_end + i]))
                      << (8 * i);
        if (stored != crc32(content.data() + record_begin, record_end - record_begin))
            return LoadResult::Corrupt;
    }

    const std::string body = content.substr(record_begin, record_end - record_begin);
    std::unordered_map<std::string, Entry> loaded;
    size_t pos = 0;
    const size_t body_size = body.size();

    auto readLine = [&](std::string& line) -> bool {
        const size_t nl = body.find('\n', pos);
        if (nl == std::string::npos) return false;
        line = body.substr(pos, nl - pos);
        pos = nl + 1;
        return true;
    };
    auto readExact = [&](size_t n, std::string& out) -> bool {
        if (pos + n > body_size) return false;
        out.assign(body, pos, n);
        pos += n;
        return true;
    };

    std::string header;
    while (pos < body_size) {
        if (!readLine(header)) return LoadResult::Corrupt;
        if (header.empty()) continue;
        std::istringstream iss(header);
        char type = 0;
        size_t klen = 0, count = 0;
        long long ttl_ms = 0;
        iss >> type;
        if (type == 'K') {
            if (!(iss >> klen >> count >> ttl_ms)) return LoadResult::Corrupt;
            std::string key, value;
            const size_t vlen = count;  // 该字段对 K 记录表示 value 长度
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
            if (count > body_size) return LoadResult::Corrupt;
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
            if (count > body_size) return LoadResult::Corrupt;
            Hash hash;
            for (size_t i = 0; i < count; ++i) {
                std::string lenLine;
                if (!readLine(lenLine)) return LoadResult::Corrupt;
                std::istringstream ls(lenLine);
                size_t flen = 0, vlen = 0;
                if (!(ls >> flen >> vlen)) return LoadResult::Corrupt;
                std::string field, value;
                if (!readExact(flen, field) || !readExact(vlen, value)) return LoadResult::Corrupt;
                hash[std::move(field)] = std::move(value);
            }
            Entry e{std::move(hash), false, ExpireAt{}};
            if (ttl_ms > 0) {
                e.has_expire = true;
                e.expire_at = now() + std::chrono::milliseconds(ttl_ms);
            }
            loaded[std::move(key)] = std::move(e);
        } else {
            return LoadResult::Corrupt;
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    data_ = std::move(loaded);
    // 重建时间轮：按剩余 TTL 重新分桶
    expiry_buckets_.clear();
    key_bucket_.clear();
    for (auto& kv : data_) {
        if (!kv.second.has_expire) continue;
        const long long bucket = wheelSecond(kv.second.expire_at);
        expiry_buckets_[bucket].push_back(kv.first);
        key_bucket_[kv.first] = bucket;
    }
    wheel_cursor_ = wheelSecond(now());
    return legacy ? LoadResult::LegacyFormat : LoadResult::Ok;
}

}  // namespace redis
