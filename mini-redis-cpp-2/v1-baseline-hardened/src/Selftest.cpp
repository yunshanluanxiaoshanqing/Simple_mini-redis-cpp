// Selftest.cpp — 内置语义自检（v1）
//
// 为什么需要它：01/02/03 三个版本的验证完全依赖 shell 脚本 + nc，
// 一旦环境没有 nc 或端口被占用就无法验证任何语义。
// 本自检在进程内直接驱动 Store/Executor/Parser，几毫秒即可跑完，
// 覆盖：命令语义、类型不变量、过期、持久化、协议粘包/半包、并发原子性。

#include "Selftest.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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
        const char c = in[i];
        if (c == '\r' && i + 1 < in.size() && in[i + 1] == '\n') {
            out.push_back('|');
            ++i;
            continue;
        }
        if (c == '\n') {
            out.push_back('|');
            continue;
        }
        out.push_back(c);
    }
    return out;
}

std::string run(Executor& executor, const std::vector<std::string>& args) {
    resp::Response response;
    executor.execute(args, response);
    return flatten(response.data());
}

/// 变参便捷包装：run(executor, "SET", "k", "v") 等价于 run(executor, {"SET","k","v"})。
template <typename... Args>
std::string run(Executor& executor, Args&&... args) {
    const std::vector<std::string> argv{std::string(std::forward<Args>(args))...};
    return run(executor, argv);
}

/// 把参数列表拼成 "a,b,c," 形式，便于断言解析结果。
std::string joinArgs(const std::vector<std::string>& args) {
    std::string s;
    for (const auto& t : args) s += t + ",";
    return s;
}

/// 把 RESP 数组展开成 "N:e0,e1,..."（元素为 bulk 字符串内容，空数组为 "0:"）。
/// 用于断言“数组长度 + 内容”，例如 HKEYS/HGETALL/KEYS 这类数组响应。
std::string arrayShape(const std::string& resp_bytes) {
    if (resp_bytes.empty() || resp_bytes[0] != '*') return "not-array";
    size_t crlf = resp_bytes.find("\r\n", 0);
    if (crlf == std::string::npos) return "truncated";
    const long long n = std::atoll(resp_bytes.substr(1, crlf - 1).c_str());
    size_t pos = crlf + 2;
    std::string out = std::to_string(n) + ":";
    for (long long i = 0; i < n; ++i) {
        if (pos >= resp_bytes.size()) {
            out += "<missing>";
            out += ",";
            continue;
        }
        if (resp_bytes[pos] == '$') {
            const size_t lcrlf = resp_bytes.find("\r\n", pos);
            const long long len = std::atoll(resp_bytes.substr(pos + 1, lcrlf - pos - 1).c_str());
            if (len < 0) {
                out += "<nil>";
                pos = lcrlf + 2;
            } else {
                out += resp_bytes.substr(lcrlf + 2, static_cast<size_t>(len));
                pos = lcrlf + 2 + static_cast<size_t>(len) + 2;
            }
        } else if (resp_bytes[pos] == '+') {
            const size_t lcrlf = resp_bytes.find("\r\n", pos);
            out += resp_bytes.substr(pos + 1, lcrlf - pos - 1);
            pos = lcrlf + 2;
        } else {
            out += "?";
            break;
        }
        out += ",";
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

    std::cout << "== v1 self-test ==" << std::endl;

    // ---------- 协议：粘包 / 半包 / 内联 / 协议错误 ----------
    {
        resp::RequestParser parser;
        const std::string a = "*2\r\n$4\r\nECHO\r\n$1\r\na\r\n";
        const std::string b = "*1\r\n$4\r\nPING\r\n";
        parser.feed((a + b).c_str(), a.size() + b.size());  // 粘包：两条一次性到达
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
        for (size_t i = 0; i < full.size(); ++i) {  // 半包：逐字节喂入
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
        // 声明的 bulk 长度本身就超过缓存上限：不可能收满，必须立即判协议错误
        // （否则连接会永久等待并占住内存）。
        const std::string bad = "*1\r\n$70000000\r\n";
        parser.feed(bad.c_str(), bad.size());
        std::vector<std::string> args;
        checkTrue("协议错误-bulk 长度超过上限",
                  parser.next(args) == resp::ParseState::kError);
    }
    {
        resp::RequestParser parser;
        // 声明 2 字节却塞了 "abc"，于是数据后紧跟的不是 CRLF → 协议错误
        const std::string bad = "*1\r\n$2\r\nabc\r\n";
        parser.feed(bad.c_str(), bad.size());
        std::vector<std::string> args;
        checkTrue("协议错误-终止符与长度不匹配",
                  parser.next(args) == resp::ParseState::kError);
    }

    // ---------- 命令语义 ----------
    {
        Store store;
        Executor executor(store);
        check("PING", "+PONG|", run(executor, "PING"));
        check("PING msg", "$3|hey|", run(executor, "PING", "hey"));
        check("ECHO", "$5|hello|", run(executor, "ECHO", "hello"));
        check("SET", "+OK|", run(executor, "SET", "k", "v"));
        check("GET", "$1|v|", run(executor, "GET", "k"));
        check("GET 不存在", "$-1|", run(executor, "GET", "nope"));
        check("TYPE string", "+string|", run(executor, "TYPE", "k"));
        check("EXISTS 命中", ":1|", run(executor, "EXISTS", "k"));
        check("EXISTS 未命中", ":0|", run(executor, "EXISTS", "nope"));
        check("DEL", ":1|", run(executor, "DEL", "k"));
        check("DEL 二次", ":0|", run(executor, "DEL", "k"));
        check("未知命令-带命令名", "-ERR unknown command 'nosuchcmd'|",
              run(executor, "NoSuchCmd"));
        check("参数个数错误", "-ERR wrong number of arguments for 'get' command|",
              run(executor, "GET"));
        check("整数非法（拒绝前缀数字）", "-ERR value is not an integer or out of range|",
              run(executor, "EXPIRE", "k", "12abc"));
    }
    {
        // 关键改进：SET/LPUSH 混用同一个 key 不再产生“幽灵数据”
        Store store;
        Executor executor(store);
        run(executor, "LPUSH", "mix", "a");
        check("SET 覆盖 list 不报错", "+OK|", run(executor, "SET", "mix", "str"));
        check("覆盖后 TYPE=string", "+string|", run(executor, "TYPE", "mix"));
        check("覆盖后 GET 正确", "$3|str|", run(executor, "GET", "mix"));
        check("覆盖后 LLEN 报 WRONGTYPE（旧 list 已清除）",
              "-WRONGTYPE Operation against a key holding the wrong kind of value|",
              run(executor, "LLEN", "mix"));
        // 反向：list 命令打到 string 上必须报 WRONGTYPE，而不是把 key 悄悄变成列表
        run(executor, "DEL", "mix");
        run(executor, "SET", "mix", "str");
        check("LPUSH 打到 string 报 WRONGTYPE",
              "-WRONGTYPE Operation against a key holding the wrong kind of value|",
              run(executor, "LPUSH", "mix", "a"));
        check("LPUSH 失败后仍是 string", "+string|", run(executor, "TYPE", "mix"));
        check("LPUSH 失败后值未变", "$3|str|", run(executor, "GET", "mix"));
        run(executor, "DEL", "mix");
        run(executor, "RPUSH", "mix", "a");
        check("LRANGE", "*1|$1|a|", run(executor, "LRANGE", "mix", "0", "-1"));
    }
    {
        Store store;
        Executor executor(store);
        check("WRONGTYPE-GET list", "-WRONGTYPE Operation against a key holding the wrong kind of value|",
              [&] {
                  run(executor, "RPUSH", "l", "a");
                  return run(executor, "GET", "l");
              }());
        check("WRONGTYPE-LPUSH string", "-WRONGTYPE Operation against a key holding the wrong kind of value|",
              [&] {
                  run(executor, "SET", "s", "v");
                  return run(executor, "LPUSH", "s", "a");
              }());
        check("WRONGTYPE-HGET list", "-WRONGTYPE Operation against a key holding the wrong kind of value|",
              run(executor, "HGET", "l", "f"));
    }
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
        check("LPOP 单元素", "$1|a|", run(executor, "LPOP", "l"));
        check("RPOP count=2", "*2|$1|c|$1|b|", run(executor, "RPOP", "l", "2"));
        check("列表清空后 key 消失", ":0|", run(executor, "EXISTS", "l"));
        check("空列表 TYPE=none", "+none|", run(executor, "TYPE", "l"));
    }
    {
        Store store;
        Executor executor(store);
        run(executor, "RPUSH", "l", "a");
        run(executor, "LREM", "l", "0", "a");
        check("LREM 清空后 key 消失", ":0|", run(executor, "EXISTS", "l"));
        run(executor, "LPUSH", "l", "a");
        run(executor, "LSET", "l", "0", "b");
        check("LSET 生效", "*1|$1|b|", run(executor, "LRANGE", "l", "0", "-1"));
        check("LSET 越界", "-ERR index out of range|", run(executor, "LSET", "l", "5", "x"));
        check("LSET 不存在的 key", "-ERR no such key|", run(executor, "LSET", "nokey", "0", "x"));
        check("LGET 兼容命令", "*1|$1|b|", run(executor, "LGET", "l"));
    }
    {
        Store store;
        Executor executor(store);
        check("HSET 新建", ":1|", run(executor, "HSET", "h", "f1", "v1"));
        check("HSET 更新", ":0|", run(executor, "HSET", "h", "f1", "v2"));
        check("HGET", "$2|v2|", run(executor, "HGET", "h", "f1"));
        check("HEXISTS 命中", ":1|", run(executor, "HEXISTS", "h", "f1"));
        check("HMSET", "+OK|", run(executor, "HMSET", "h", "f2", "x", "f3", "y"));
        check("HLEN", ":3|", run(executor, "HLEN", "h"));
        check("HGETALL 以 field 排序输出", "6:f1,v2,f2,x,f3,y,",
              arrayShape([&] {
                  resp::Response r;
                  executor.execute({"HGETALL", "h"}, r);
                  return r.data();
              }()));
        check("HKEYS 排序", "3:f1,f2,f3,",
              arrayShape([&] {
                  resp::Response r;
                  executor.execute({"HKEYS", "h"}, r);
                  return r.data();
              }()));
        check("HDEL", ":1|", run(executor, "HDEL", "h", "f1"));
        check("HDEL 多字段", ":2|", run(executor, "HDEL", "h", "f2", "f3"));
        check("哈希清空后 key 消失", ":0|", run(executor, "EXISTS", "h"));
        check("HGETALL 空哈希", "*0|", run(executor, "HGETALL", "h"));
        check("HMSET 奇数参数", "-ERR wrong number of arguments for 'hmset' command|",
              run(executor, "HMSET", "h", "f1"));
    }
    {
        Store store;
        Executor executor(store);
        run(executor, "SET", "k", "v");
        check("TTL 无过期", ":-1|", run(executor, "TTL", "k"));
        check("TTL 不存在的 key", ":-2|", run(executor, "TTL", "nope"));
        check("EXPIRE", ":1|", run(executor, "EXPIRE", "k", "100"));
        const std::string ttl = run(executor, "TTL", "k");
        checkTrue("TTL 在 1..100 之间", ttl.size() > 2 && std::atoll(ttl.c_str() + 1) > 0 &&
                                            std::atoll(ttl.c_str() + 1) <= 100);
        check("PERSIST", ":1|", run(executor, "PERSIST", "k"));
        check("PERSIST 后 TTL=-1", ":-1|", run(executor, "TTL", "k"));
        check("PERSIST 无 TTL 的 key", ":0|", run(executor, "PERSIST", "k"));
        check("EXPIRE 非正数立即删除", ":1|", run(executor, "EXPIRE", "k", "0"));
        check("被立即删除后不存在", ":0|", run(executor, "EXISTS", "k"));
        run(executor, "SET", "k2", "v");
        check("EXPIREAT 过去时间删除", ":1|", run(executor, "EXPIREAT", "k2", "1"));
        check("EXPIREAT 之后不存在", ":0|", run(executor, "EXISTS", "k2"));
        run(executor, "SET", "k3", "v");
        check("SET 带 EX 生效", "+OK|", run(executor, "SET", "k3", "v2", "EX", "50"));
        checkTrue("SET EX 后 TTL>0", std::atoll(run(executor, "TTL", "k3").c_str() + 1) > 0);
        check("SET 覆盖默认清除 TTL", "+OK|", run(executor, "SET", "k3", "v3"));
        check("覆盖后 TTL=-1", ":-1|", run(executor, "TTL", "k3"));
        run(executor, "SET", "k4", "v", "EX", "50");
        run(executor, "SET", "k4", "v2", "KEEPTTL");
        checkTrue("SET KEEPTTL 保留 TTL", std::atoll(run(executor, "TTL", "k4").c_str() + 1) > 0);
        check("SET NX 已存在", "$-1|", run(executor, "SET", "k4", "v3", "NX"));
        check("SET XX 不存在", "$-1|", run(executor, "SET", "k5", "v", "XX"));
        check("SET 非法选项", "-ERR syntax error|", run(executor, "SET", "k5", "v", "BOGUS"));
        check("SET 非法过期时间", "-ERR invalid expire time in 'set' command|",
              run(executor, "SET", "k5", "v", "EX", "0"));
    }
    {
        Store store;
        Executor executor(store);
        run(executor, "SET", "kk", "v");
        check("RENAME 保留 TTL", ":1|", run(executor, "EXPIRE", "kk", "50"));
        check("RENAME", "+OK|", run(executor, "RENAME", "kk", "kk2"));
        checkTrue("RENAME 后 TTL 跟随", std::atoll(run(executor, "TTL", "kk2").c_str() + 1) > 0);
        check("RENAME 不存在的 key", "-ERR no such key|", run(executor, "RENAME", "nope", "x"));
        run(executor, "LPUSH", "dst", "old");
        run(executor, "SET", "src", "new");
        run(executor, "RENAME", "src", "dst");
        check("RENAME 覆盖目标类型", "+string|", run(executor, "TYPE", "dst"));
        // 覆盖后旧 list 必须彻底消失：LLEN 对 string 报 WRONGTYPE（而非残留长度）
        check("RENAME 覆盖后无 list 残留",
              "-WRONGTYPE Operation against a key holding the wrong kind of value|",
              run(executor, "LLEN", "dst"));
        check("RENAME 覆盖后按 string 读取", "$3|new|", run(executor, "GET", "dst"));
        check("KEYS 模式匹配", "2:a1,a2,", [&] {
            run(executor, "FLUSHALL");
            run(executor, "SET", "a1", "1");
            run(executor, "SET", "a2", "2");
            run(executor, "SET", "b1", "3");
            resp::Response r;
            executor.execute({"KEYS", "a*"}, r);
            return arrayShape(r.data());
        }());
        // "a?" 匹配任意两字符且首字符为 a：a1/a2 都该命中，b1 不命中
        check("KEYS ? 匹配单字符", "2:a1,a2,", arrayShape([&] {
                  resp::Response r;
                  executor.execute({"KEYS", "a?"}, r);
                  return r.data();
              }()));
        check("KEYS ? 不多吃字符", "0:", arrayShape([&] {
                  resp::Response r;
                  executor.execute({"KEYS", "a??"}, r);  // 需要三字符，只有两个 key
                  return r.data();
              }()));
        check("DBSIZE", ":3|", run(executor, "DBSIZE"));
    }

    // ---------- 持久化 ----------
    {
        const std::string path = "selftest-dump.rdb";
        std::remove(path.c_str());
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
            checkTrue("持久化写入成功", store.save(path));
        }
        {
            Store store;
            const LoadResult result = store.load(path);
            checkTrue("持久化读取成功", result == LoadResult::Ok);
            Executor executor(store);
            check("二进制安全的值",
                  flatten("$" + std::to_string(std::string("a b\tc\nline2 ").size() + 1 + 4) + "|" +
                          std::string("a b\tc\nline2 ") + '\0' + "tail|"),
                  run(executor, "GET", "bin key"));
            check("含空格的值", "$17|value with spaces|", run(executor, "GET", "plain"));
            // 元素含空格、含换行都能原样读回（换行在展示时被转成 '|'）
            check("列表元素含空格/换行", "*2|$3|x y|$3|z|w|", run(executor, "LGET", "list"));            check("哈希 field/value 含空格与冒号", "2:field one,value:one,", arrayShape([&] {
                      resp::Response r;
                      executor.execute({"HGETALL", "hash"}, r);
                      return r.data();
                  }()));
            const std::string ttl = run(executor, "TTL", "withttl");
            checkTrue("TTL 持久化后仍有效", std::atoll(ttl.c_str() + 1) > 900);
        }
        {
            // 损坏文件必须被识别，而不是把垃圾数据装进内存
            std::string data = "K 5 3 0\nkeyxxx";  // 声明 3 字节值但给了 3 字节后缺 CRLF 无所谓
            data += "G garbage\n";
            FILE* fp = std::fopen(path.c_str(), "wb");
            if (fp) {
                std::fwrite(data.data(), 1, data.size(), fp);
                std::fclose(fp);
            }
            Store store;
            checkTrue("损坏文件被识别", store.load(path) == LoadResult::Corrupt);
        }
        std::remove(path.c_str());
        {
            Store store;
            checkTrue("不存在的文件返回 NotFound",
                      store.load("definitely-not-here.rdb") == LoadResult::NotFound);
        }
    }

    // ---------- 并发原子性 ----------
    {
        // 四个线程各操作自己的 key：验证 Store 的锁没有丢（计数必须精确）。
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

    std::cout << "== v1 self-test: " << pass << " passed, " << fail << " failed ==" << std::endl;
    return fail > 127 ? 127 : fail;
}

}  // namespace redis
