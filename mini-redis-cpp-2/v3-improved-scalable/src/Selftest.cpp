// Selftest.cpp — v3 内置语义自检
//
// 覆盖：命令语义、协议粘包/半包、过期时间轮、快照与 CRC 校验、
//       运行期配置、内存上限、并发原子性。

#include "Selftest.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "Commands.h"
#include "Resp.h"
#include "Storage.h"

namespace redis {

namespace {

std::string flatten(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\r' && i + 1 < in.size() && in[i + 1] == '\n') {
            out.push_back('|');
            ++i;
            continue;
        }
        out.push_back(in[i]);
    }
    return out;
}

std::string run(Executor& executor, const std::vector<std::string>& args) {
    resp::Response response;
    executor.execute(args, response);
    return flatten(response.data());
}

template <typename... Args>
std::string run(Executor& executor, Args&&... args) {
    const std::vector<std::string> argv{std::string(std::forward<Args>(args))...};
    return run(executor, argv);
}

/// 把参数列表拼成 "a,b,c,"，便于断言解析结果。
std::string joinArgs(const std::vector<std::string>& args) {
    std::string s;
    for (const auto& t : args) s += t + ",";
    return s;
}

/// 把 RESP 数组展开成 "N:e0,e1,..."（空数组为 "0:"）。
std::string arrayShape(const std::string& resp_bytes) {
    if (resp_bytes.empty() || resp_bytes[0] != '*') return "not-array";
    const size_t crlf = resp_bytes.find("\r\n", 0);
    if (crlf == std::string::npos) return "truncated";
    const long long n = std::atoll(resp_bytes.substr(1, crlf - 1).c_str());
    size_t pos = crlf + 2;
    std::string out = std::to_string(n) + ":";
    for (long long i = 0; i < n; ++i) {
        if (pos >= resp_bytes.size()) {
            out += "<missing>,";
            continue;
        }
        if (resp_bytes[pos] == '$') {
            const size_t lcrlf = resp_bytes.find("\r\n", pos);
            if (lcrlf == std::string::npos) {
                out += "<bad>,";
                break;
            }
            const long long len = std::atoll(resp_bytes.substr(pos + 1, lcrlf - pos - 1).c_str());
            if (len < 0) {
                out += "<nil>,";
                pos = lcrlf + 2;
            } else {
                out += resp_bytes.substr(lcrlf + 2, static_cast<size_t>(len)) + ",";
                pos = lcrlf + 2 + static_cast<size_t>(len) + 2;
            }
        } else if (resp_bytes[pos] == '+') {
            const size_t lcrlf = resp_bytes.find("\r\n", pos);
            out += resp_bytes.substr(pos + 1, lcrlf - pos - 1) + ",";
            pos = lcrlf + 2;
        } else {
            out += "?,";
            break;
        }
    }
    return out;
}

}  // namespace

int runSelfTest() {
    int pass = 0;
    int fail = 0;
    auto check = [&](const std::string& name, const std::string& expected,
                     const std::string& actual) {
        if (expected == actual) {
            ++pass;
            return;
        }
        ++fail;
        std::cout << "FAIL " << name << "\n     expected: [" << expected << "]\n     actual:   ["
                  << actual << "]" << std::endl;
    };
    auto checkTrue = [&](const std::string& name, bool value) {
        check(name, "true", value ? "true" : "false");
    };
    auto contains = [&](const std::string& name, const std::string& needle,
                        const std::string& haystack) {
        checkTrue(name, haystack.find(needle) != std::string::npos);
    };

    std::cout << "== v3 self-test ==" << std::endl;

    // ---------- 协议解析：粘包 / 半包 / 内联 / 协议错误 ----------
    {
        resp::RequestParser parser;
        const std::string a = "*2\r\n$4\r\nECHO\r\n$1\r\na\r\n";
        const std::string b = "*1\r\n$4\r\nPING\r\n";
        parser.feed((a + b).c_str(), a.size() + b.size());
        std::vector<std::string> args;
        check("粘包-第1条", "ECHO,a,", [&] {
            parser.next(args);
            return joinArgs(args);
        }());
        check("粘包-第2条", "PING,", [&] {
            parser.next(args);
            return joinArgs(args);
        }());
        checkTrue("粘包-无剩余", parser.next(args) == resp::ParseState::kNeedMore);
    }
    {
        resp::RequestParser parser;
        const std::string full = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$5\r\nhello\r\n";
        resp::ParseState state = resp::ParseState::kNeedMore;
        std::vector<std::string> args;
        for (size_t i = 0; i < full.size(); ++i) {
            parser.feed(full.data() + i, 1);
            state = parser.next(args);
            if (state == resp::ParseState::kRequest) break;
        }
        checkTrue("半包-逐字节最终成功", state == resp::ParseState::kRequest);
        check("半包-参数", "SET,k,hello,", joinArgs(args));
    }
    {
        resp::RequestParser parser;
        const std::string inline_cmd = "eChO \"hello world\"\n";
        parser.feed(inline_cmd.c_str(), inline_cmd.size());
        std::vector<std::string> args;
        checkTrue("内联-解析成功", parser.next(args) == resp::ParseState::kRequest);
        check("内联-带引号参数", "eChO,hello world,", joinArgs(args));
    }
    {
        resp::RequestParser parser;
        const std::string bad = "*abc\r\n";
        parser.feed(bad.c_str(), bad.size());
        std::vector<std::string> args;
        checkTrue("协议错误-非法 multibulk", parser.next(args) == resp::ParseState::kError);
        checkTrue("协议错误-粘性失败", parser.failed());
    }
    {
        resp::RequestParser parser;
        const std::string bad = "*1\r\n$70000000\r\n";
        parser.feed(bad.c_str(), bad.size());
        std::vector<std::string> args;
        checkTrue("协议错误-bulk 长度超过上限", parser.next(args) == resp::ParseState::kError);
    }
    {
        resp::RequestParser parser;
        const std::string bad = "*1\r\n$2\r\nabc\r\n";
        parser.feed(bad.c_str(), bad.size());
        std::vector<std::string> args;
        checkTrue("协议错误-终止符与长度不匹配", parser.next(args) == resp::ParseState::kError);
    }

    // ---------- 基础命令 ----------
    {
        Store store;
        Executor executor(store);
        check("PING", "+PONG|", run(executor, "PING"));
        check("PING msg", "$3|hey|", run(executor, "PING", "hey"));
        check("ECHO", "$5|hello|", run(executor, "ECHO", "hello"));
        check("SET", "+OK|", run(executor, "SET", "k", "v"));
        check("GET", "$1|v|", run(executor, "GET", "k"));
        check("GET 不存在", "$-1|", run(executor, "GET", "nope"));
        check("TYPE", "+string|", run(executor, "TYPE", "k"));
        check("EXISTS", ":1|", run(executor, "EXISTS", "k"));
        check("EXISTS 多 key", ":2|", run(executor, "EXISTS", "k", "nope", "k"));
        check("DEL", ":1|", run(executor, "DEL", "k"));
        check("DEL 二次", ":0|", run(executor, "DEL", "k"));
        check("未知命令", "-ERR unknown command 'nosuch'|", run(executor, "NoSuch"));
        check("参数个数错误", "-ERR wrong number of arguments for 'get' command|",
              run(executor, "GET"));
        check("整数非法", "-ERR value is not an integer or out of range|",
              run(executor, "EXPIRE", "k", "12abc"));
        check("QUIT 返回 OK", "+OK|", run(executor, "QUIT"));
        check("COMMAND 列表含 GET", "true", [&] {
            resp::Response r;
            executor.execute({"COMMAND"}, r);
            return r.data().find("GET") != std::string::npos ? "true" : "false";
        }());
    }

    // ---------- v3 新增字符串命令 ----------
    {
        Store store;
        Executor executor(store);
        check("SETNX 新 key", ":1|", run(executor, "SETNX", "a", "1"));
        check("SETNX 已存在", ":0|", run(executor, "SETNX", "a", "2"));
        check("SETNX 未覆盖值", "$1|1|", run(executor, "GET", "a"));
        check("APPEND 到新 key", ":3|", run(executor, "APPEND", "b", "abc"));
        check("APPEND 追加", ":6|", run(executor, "APPEND", "b", "def"));
        check("APPEND 结果", "$6|abcdef|", run(executor, "GET", "b"));
        check("INCR 新 key", ":1|", run(executor, "INCR", "n"));
        check("INCR 第二次", ":2|", run(executor, "INCR", "n"));
        check("INCRBY", ":12|", run(executor, "INCRBY", "n", "10"));
        check("DECR", ":11|", run(executor, "DECR", "n"));
        check("DECRBY", ":1|", run(executor, "DECRBY", "n", "10"));
        check("INCR 非整数值", "-ERR value is not an integer or out of range|", [&] {
            run(executor, "SET", "s", "abc");
            return run(executor, "INCR", "s");
        }());
        check("INCR 溢出保护", "-ERR increment or decrement would overflow|", [&] {
            run(executor, "SET", "big", "9223372036854775807");
            return run(executor, "INCR", "big");
        }());
        run(executor, "SET", "old", "oldval");
        check("GETSET 返回旧值", "$6|oldval|", run(executor, "GETSET", "old", "newval"));
        check("GETSET 写入新值", "$6|newval|", run(executor, "GET", "old"));
        check("GETSET 新 key 返回 nil", "$-1|", run(executor, "GETSET", "fresh", "x"));
        check("MSET", "+OK|", run(executor, "MSET", "m1", "1", "m2", "2"));
        check("MGET", "*2|$1|1|$1|2|", run(executor, "MGET", "m1", "m2"));
        check("MGET 缺 key 返回 nil", "*2|$1|1|$-1|", run(executor, "MGET", "m1", "nosuch"));
        check("MSET 奇数参数", "-ERR wrong number of arguments for 'mset' command|",
              run(executor, "MSET", "m1"));
        check("APPEND 到 list 报 WRONGTYPE",
              "-WRONGTYPE Operation against a key holding the wrong kind of value|", [&] {
                  run(executor, "RPUSH", "L", "x");
                  return run(executor, "APPEND", "L", "y");
              }());
    }

    // ---------- 类型不变量 ----------
    {
        Store store;
        Executor executor(store);
        run(executor, "LPUSH", "mix", "a");
        check("SET 覆盖 list", "+OK|", run(executor, "SET", "mix", "str"));
        check("覆盖后 TYPE=string", "+string|", run(executor, "TYPE", "mix"));
        check("覆盖后无幽灵列表",
              "-WRONGTYPE Operation against a key holding the wrong kind of value|",
              run(executor, "LLEN", "mix"));
        run(executor, "DEL", "mix");
        run(executor, "SET", "mix", "str");
        check("LPUSH 打到 string 报 WRONGTYPE",
              "-WRONGTYPE Operation against a key holding the wrong kind of value|",
              run(executor, "LPUSH", "mix", "a"));
        check("失败后值未变", "$3|str|", run(executor, "GET", "mix"));
        run(executor, "FLUSHALL");
        check("FLUSHALL 后 DBSIZE=0", ":0|", run(executor, "DBSIZE"));
    }

    // ---------- 列表 ----------
    {
        Store store;
        Executor executor(store);
        run(executor, "RPUSH", "l", "a");
        run(executor, "RPUSH", "l", "b");
        run(executor, "RPUSH", "l", "c");
        check("LLEN", ":3|", run(executor, "LLEN", "l"));
        check("LINDEX 0", "$1|a|", run(executor, "LINDEX", "l", "0"));
        check("LINDEX -1", "$1|c|", run(executor, "LINDEX", "l", "-1"));
        check("LINDEX 越界", "$-1|", run(executor, "LINDEX", "l", "9"));
        check("LRANGE", "*3|$1|a|$1|b|$1|c|", run(executor, "LRANGE", "l", "0", "-1"));
        check("LRANGE 越界段", "*0|", run(executor, "LRANGE", "l", "5", "9"));
        check("LPOP", "$1|a|", run(executor, "LPOP", "l"));
        check("RPOP count=2", "*2|$1|c|$1|b|", run(executor, "RPOP", "l", "2"));
        check("列表清空后 key 消失", ":0|", run(executor, "EXISTS", "l"));
        check("空列表 TYPE=none", "+none|", run(executor, "TYPE", "l"));
        run(executor, "RPUSH", "l2", "a");
        run(executor, "LSET", "l2", "0", "b");
        check("LSET 生效", "*1|$1|b|", run(executor, "LRANGE", "l2", "0", "-1"));
        check("LSET 越界", "-ERR index out of range|", run(executor, "LSET", "l2", "5", "x"));
        check("LSET 不存在 key", "-ERR no such key|", run(executor, "LSET", "nokey", "0", "x"));
        run(executor, "RPUSH", "l3", "x", "y", "x");
        check("LREM count=0 删全部", ":2|", run(executor, "LREM", "l3", "0", "x"));
        check("LREM 后剩余", "*1|$1|y|", run(executor, "LGET", "l3"));
    }

    // ---------- 哈希 ----------
    {
        Store store;
        Executor executor(store);
        check("HSET 新建", ":1|", run(executor, "HSET", "h", "f1", "v1"));
        check("HSET 更新", ":0|", run(executor, "HSET", "h", "f1", "v2"));
        check("HGET", "$2|v2|", run(executor, "HGET", "h", "f1"));
        check("HEXISTS", ":1|", run(executor, "HEXISTS", "h", "f1"));
        check("HMSET", "+OK|", run(executor, "HMSET", "h", "f2", "x", "f3", "y"));
        check("HLEN", ":3|", run(executor, "HLEN", "h"));
        check("HGETALL 排序输出", "6:f1,v2,f2,x,f3,y,", arrayShape([&] {
                  resp::Response r;
                  executor.execute({"HGETALL", "h"}, r);
                  return r.data();
              }()));
        check("HKEYS 排序", "3:f1,f2,f3,", arrayShape([&] {
                  resp::Response r;
                  executor.execute({"HKEYS", "h"}, r);
                  return r.data();
              }()));
        check("HVALS 排序", "3:v2,x,y,", arrayShape([&] {
                  resp::Response r;
                  executor.execute({"HVALS", "h"}, r);
                  return r.data();
              }()));
        check("HDEL 多字段", ":2|", run(executor, "HDEL", "h", "f2", "f3"));
        check("HDEL 单字段", ":1|", run(executor, "HDEL", "h", "f1"));
        check("哈希清空后 key 消失", ":0|", run(executor, "EXISTS", "h"));
        check("HGETALL 空哈希", "*0|", run(executor, "HGETALL", "h"));
        check("HMSET 奇数参数", "-ERR wrong number of arguments for 'hmset' command|",
              run(executor, "HMSET", "h", "f1"));
    }

    // ---------- 过期与时间轮 ----------
    {
        Store store;
        Executor executor(store);
        run(executor, "SET", "k", "v");
        check("TTL 无过期", ":-1|", run(executor, "TTL", "k"));
        check("TTL 不存在", ":-2|", run(executor, "TTL", "nope"));
        check("EXPIRE", ":1|", run(executor, "EXPIRE", "k", "100"));
        checkTrue("TTL 在 1..100", std::atoll(run(executor, "TTL", "k").c_str() + 1) > 0);
        check("PERSIST", ":1|", run(executor, "PERSIST", "k"));
        check("PERSIST 后 TTL=-1", ":-1|", run(executor, "TTL", "k"));
        check("PERSIST 无 TTL", ":0|", run(executor, "PERSIST", "k"));
        // 时间轮：1 秒后必须自动不可见（不需要任何全表扫描）
        run(executor, "SET", "short", "v");
        run(executor, "EXPIRE", "short", "1");
        check("刚设置时存在", ":1|", run(executor, "EXISTS", "short"));
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        check("1 秒后时间轮已清除", ":0|", run(executor, "EXISTS", "short"));
        check("被清除后 TYPE=none", "+none|", run(executor, "TYPE", "short"));
        check("过期键不在 KEYS 中", "0:", arrayShape([&] {
                  resp::Response r;
                  executor.execute({"KEYS", "short*"}, r);
                  return r.data();
              }()));
        const Stats after_expire = store.stats();
        checkTrue("过期计数已增加", after_expire.expired_keys >= 1);
        check("EXPIRE 非正数立即删除", ":1|", [&] {
            run(executor, "SET", "gone", "v");
            return run(executor, "EXPIRE", "gone", "0");
        }());
        check("立即删除后不存在", ":0|", run(executor, "EXISTS", "gone"));
        // SET 选项
        check("SET EX", "+OK|", run(executor, "SET", "k3", "v", "EX", "50"));
        checkTrue("SET EX 后 TTL>0", std::atoll(run(executor, "TTL", "k3").c_str() + 1) > 0);
        check("SET 默认清除 TTL", "+OK|", run(executor, "SET", "k3", "v2"));
        check("覆盖后 TTL=-1", ":-1|", run(executor, "TTL", "k3"));
        run(executor, "SET", "k4", "v", "EX", "50");
        run(executor, "SET", "k4", "v2", "KEEPTTL");
        checkTrue("SET KEEPTTL 保留 TTL", std::atoll(run(executor, "TTL", "k4").c_str() + 1) > 0);
        check("SET NX 已存在返回 nil", "$-1|", run(executor, "SET", "k4", "v3", "NX"));
        check("SET XX 不存在返回 nil", "$-1|", run(executor, "SET", "newkey", "v", "XX"));
        check("SET 非法选项", "-ERR syntax error|", run(executor, "SET", "x", "v", "BOGUS"));
        check("SET 非法过期时间", "-ERR invalid expire time in 'set' command|",
              run(executor, "SET", "x", "v", "EX", "0"));
    }

    // ---------- RENAME / KEYS / DBSIZE ----------
    {
        Store store;
        Executor executor(store);
        run(executor, "SET", "src", "v");
        run(executor, "EXPIRE", "src", "100");
        check("RENAME", "+OK|", run(executor, "RENAME", "src", "dst"));
        checkTrue("RENAME 后 TTL 跟随", std::atoll(run(executor, "TTL", "dst").c_str() + 1) > 0);
        check("RENAME 不存在", "-ERR no such key|", run(executor, "RENAME", "nope", "x"));
        run(executor, "LPUSH", "old", "a");
        run(executor, "SET", "new", "s");
        run(executor, "RENAME", "new", "old");
        check("RENAME 覆盖目标类型", "+string|", run(executor, "TYPE", "old"));
        check("RENAME 覆盖后无列表残留",
              "-WRONGTYPE Operation against a key holding the wrong kind of value|",
              run(executor, "LLEN", "old"));
        check("RENAME 自身", "+OK|", run(executor, "RENAME", "old", "old"));
        run(executor, "FLUSHALL");
        run(executor, "SET", "a1", "1");
        run(executor, "SET", "a2", "2");
        run(executor, "SET", "b1", "3");
        check("KEYS a*", "2:a1,a2,", arrayShape([&] {
                  resp::Response r;
                  executor.execute({"KEYS", "a*"}, r);
                  return r.data();
              }()));
        check("KEYS a?", "2:a1,a2,", arrayShape([&] {
                  resp::Response r;
                  executor.execute({"KEYS", "a?"}, r);
                  return r.data();
              }()));
        check("KEYS a??", "0:", arrayShape([&] {
                  resp::Response r;
                  executor.execute({"KEYS", "a??"}, r);
                  return r.data();
              }()));
        check("DBSIZE", ":3|", run(executor, "DBSIZE"));
    }

    // ---------- 快照 / CRC / 内存上限 / 配置 ----------
    {
        const std::string path = "selftest-v3.rdb";
        std::remove(path.c_str());
        std::remove((path + ".tmp").c_str());
        {
            Store store;
            Executor executor(store);
            const std::string binary = std::string("a b\tc\nline2 ") + '\0' + "tail";
            run(executor, "SET", "bin key", binary);
            run(executor, "SET", "plain", "value with spaces");
            run(executor, "RPUSH", "list", "x y", "z\nw");
            run(executor, "HSET", "hash", "field one", "value:one");
            run(executor, "SET", "withttl", "v");
            run(executor, "EXPIRE", "withttl", "1000");
            checkTrue("快照写入成功", store.save(path) == SaveResult::Ok);
        }
        {
            Store store;
            checkTrue("快照读取成功", store.load(path) == LoadResult::Ok);
            Executor executor(store);
            check("二进制安全的值",
                  "$" + std::to_string(std::string("a b\tc\nline2 ").size() + 1 + 4) + "|" +
                      std::string("a b\tc\nline2 ") + '\0' + "tail|",
                  run(executor, "GET", "bin key"));
            check("含空格的值", "$17|value with spaces|", run(executor, "GET", "plain"));
            check("列表元素含空格/换行", "*2|$3|x y|$3|z\nw|", run(executor, "LGET", "list"));
            check("哈希含空格与冒号", "2:field one,value:one,", arrayShape([&] {
                      resp::Response r;
                      executor.execute({"HGETALL", "hash"}, r);
                      return r.data();
                  }()));
            checkTrue("TTL 恢复后仍有效", std::atoll(run(executor, "TTL", "withttl").c_str() + 1) > 900);
            // 时间轮在 load 之后也必须正确：设置 1 秒 TTL 并等待
            run(executor, "SET", "short2", "v");
            run(executor, "EXPIRE", "short2", "1");
            std::this_thread::sleep_for(std::chrono::milliseconds(1200));
            check("load 后时间轮仍生效", ":0|", run(executor, "EXISTS", "short2"));
        }
        {
            // CRC 损坏检测
            std::ifstream in(path, std::ios::binary);
            std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            in.close();
            checkTrue("带魔数",
                      data.size() > 12 && std::memcmp(data.data(), kDumpMagic, sizeof(kDumpMagic)) == 0);
            checkTrue("带版本号",
                      data.size() > 9 &&
                          static_cast<uint8_t>(data[sizeof(kDumpMagic)]) == kDumpVersion);
            const std::string original = data;
            if (data.size() > 20) data[data.size() / 2] ^= 0x01;
            Store store;
            checkTrue("CRC 发现 1 bit 损坏", store.importSnapshot(data) == LoadResult::Corrupt);
            checkTrue("原始字节流可正常导入", store.importSnapshot(original) == LoadResult::Ok);
            Executor executor(store);
            check("导入后内容正确", ":1|", run(executor, "EXISTS", "plain"));
        }
        std::remove(path.c_str());
        std::remove((path + ".tmp").c_str());
        {
            Store store;
            checkTrue("不存在文件返回 NotFound",
                      store.load("definitely-not-here.rdb") == LoadResult::NotFound);
        }
    }
    {
        Store store;
        Executor executor(store);
        // 内存上限
        check("CONFIG SET maxmemory", "+OK|", run(executor, "CONFIG", "SET", "maxmemory", "4096"));
        check("CONFIG GET maxmemory", "*2|$9|maxmemory|$4|4096|",
              run(executor, "CONFIG", "GET", "maxmemory"));
        check("CONFIG GET *", "true", [&] {
            resp::Response r;
            executor.execute({"CONFIG", "GET", "*"}, r);
            return r.data().find("maxclients") != std::string::npos ? "true" : "false";
        }());
        check("小写入成功", "+OK|", run(executor, "SET", "small", "v"));
        const std::string big(8192, 'x');
        checkTrue("超限写入被拒绝",
                  run(executor, "SET", "big", big).rfind("-OOM", 0) == 0);
        checkTrue("超限列表写入被拒绝",
                  run(executor, "RPUSH", "biglist", big).rfind("-OOM", 0) == 0);
        check("超限后未写入", ":0|", run(executor, "EXISTS", "big"));
        check("CONFIG SET 非法值", "-ERR value is not an integer or out of range|",
              run(executor, "CONFIG", "SET", "maxmemory", "abc"));
        check("CONFIG GET 未知参数", "-ERR Unsupported CONFIG parameter: nosuch|",
              run(executor, "CONFIG", "GET", "nosuch"));
        check("CONFIG 未知子命令",
              "-ERR unknown subcommand 'nope'. Try CONFIG GET|SET.|",
              run(executor, "CONFIG", "nope"));
        check("解除限制", "+OK|", run(executor, "CONFIG", "SET", "maxmemory", "0"));
        check("解除后写入成功", "+OK|", run(executor, "SET", "big", big));
        // INFO 字段
        const std::string info = run(executor, "INFO");
        contains("INFO 含 server", "server:mini-redis 3.0", info);
        contains("INFO 含 keys", "keys:", info);
        contains("INFO 含 hits", "keyspace_hits:", info);
        contains("INFO 含 snapshots", "snapshots:", info);
        contains("INFO 含 expired_keys", "expired_keys:", info);
        contains("INFO 含 used_memory_human", "used_memory_human:", info);
    }

    // ---------- 并发原子性 ----------
    {
        Store store;
        constexpr int kThreads = 4;
        constexpr int kIterations = 500;
        std::atomic<int> mismatches{0};
        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&store, &mismatches, t]() {
                Executor executor(store);
                const std::string key = "counter" + std::to_string(t);
                for (int i = 0; i < kIterations; ++i) {
                    resp::Response ignored;
                    executor.execute({"RPUSH", key, std::to_string(i)}, ignored);
                    resp::Response response;
                    executor.execute({"LLEN", key}, response);
                    if (response.data() != ":" + std::to_string(i + 1) + "\r\n") ++mismatches;
                }
            });
        }
        for (auto& thread : threads) thread.join();
        check("并发-无计数错乱", "0", std::to_string(mismatches.load()));
        Executor final_executor(store);
        check("并发-最终长度一致", ":" + std::to_string(kIterations) + "|",
              run(final_executor, "LLEN", "counter0"));
    }
    {
        // 同一个 key 上的并发 INCR：验证读改写是原子的（不能丢更新）
        Store store;
        constexpr int kThreads = 4;
        constexpr int kIterations = 300;
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&store]() {
                Executor executor(store);
                for (int i = 0; i < kIterations; ++i) {
                    resp::Response ignored;
                    executor.execute({"INCR", "shared"}, ignored);
                }
            });
        }
        for (auto& thread : threads) thread.join();
        Executor executor(store);
        check("并发 INCR 不丢更新", "$4|1200|", run(executor, "GET", "shared"));
    }

    std::cout << "== v3 self-test: " << pass << " passed, " << fail << " failed ==" << std::endl;
    return fail > 127 ? 127 : fail;
}

}  // namespace redis
