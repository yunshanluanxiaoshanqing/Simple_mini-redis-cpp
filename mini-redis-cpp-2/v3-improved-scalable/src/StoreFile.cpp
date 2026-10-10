// StoreFile.cpp — v3 持久化：导出/导入辅助接口
//
// 供"后台快照线程"与测试使用：
//   * exportSnapshot() 在锁内构造文件字节流，调用方拿到后可以慢慢写盘；
//   * importSnapshot() 从字节流装载（供测试在内存里验证 CRC 行为）。

#include "Storage.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace redis {

std::string Store::exportSnapshot() const {
    std::string payload;
    std::lock_guard<std::mutex> lock(mutex_);
    const Clock::time_point snapshot_at = now();
    payload.reserve(data_.size() * 64);
    for (const auto& kv : data_) {
        long long ttl = 0;
        if (kv.second.has_expire) {
            ttl = std::chrono::duration_cast<std::chrono::milliseconds>(kv.second.expire_at -
                                                                      snapshot_at)
                      .count();
            if (ttl <= 0) continue;
        }
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

    std::string file;
    file.reserve(payload.size() + 16);
    file.append(kDumpMagic, sizeof(kDumpMagic));
    file.push_back(static_cast<char>(kDumpVersion));
    file += payload;
    const uint32_t checksum = crc32(payload);
    for (int i = 0; i < 4; ++i) file.push_back(static_cast<char>((checksum >> (8 * i)) & 0xFFu));
    return file;
}

LoadResult Store::importSnapshot(const std::string& bytes) {
    std::string tmp_path = "import-snapshot.tmp";
    {
        std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
        if (!ofs) return LoadResult::IoError;
        ofs.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!ofs.good()) return LoadResult::IoError;
    }
    const LoadResult result = load(tmp_path);
    std::remove(tmp_path.c_str());
    return result;
}

}  // namespace redis
