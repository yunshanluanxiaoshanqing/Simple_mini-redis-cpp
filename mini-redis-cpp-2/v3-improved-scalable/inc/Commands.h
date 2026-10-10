#pragma once

// Commands.h — 命令表与分发（v3-improved-scalable）
//
// v3 在 v2 的命令集之上新增：
//   APPEND / INCR / DECR / INCRBY / DECRBY / SETNX / GETSET / MGET / MSET
//   INFO（含线程池与统计）/ CONFIG GET|SET（运行期热改参数）
//
// 分发仍是哈希表 + 元数据（arity/写标志），但把"哪些命令需要关闭连接/停止
// 进程"这件事统一放在 CommandEffect 里，由连接层解释，handler 不关心 I/O。

#include <string>
#include <unordered_map>
#include <vector>

#include "Resp.h"
#include "Storage.h"

namespace redis {

inline constexpr const char* kServerVersion = "mini-redis 3.0 (v3-improved-scalable)";
inline constexpr const char* kDumpFormatName = "MINIRDB2+crc32";

/// 单条命令执行后对连接/进程的影响。
enum class CommandEffect { kNone, kCloseConn, kStopServer };

class Executor {
public:
    explicit Executor(Store& store) : store_(store) {}

    /// 执行一条已解析的命令；返回该命令对连接/进程的影响。
    CommandEffect execute(const std::vector<std::string>& args, resp::Response& out);

private:
    using Handler = void (Executor::*)(const std::vector<std::string>&, resp::Response&);

    struct CommandSpec {
        Handler handler;
        int arity;       ///< >0：参数个数必须恰为该值；<0：至少 |arity| 个
        bool is_write;
    };

    static const std::unordered_map<std::string, CommandSpec>& table();

    // ---- 连接/服务器 ----
    void cmdPing(const std::vector<std::string>&, resp::Response&);
    void cmdEcho(const std::vector<std::string>&, resp::Response&);
    void cmdFlushAll(const std::vector<std::string>&, resp::Response&);
    void cmdDbsize(const std::vector<std::string>&, resp::Response&);
    void cmdQuit(const std::vector<std::string>&, resp::Response&);
    void cmdShutdown(const std::vector<std::string>&, resp::Response&);
    void cmdInfo(const std::vector<std::string>&, resp::Response&);
    void cmdConfig(const std::vector<std::string>&, resp::Response&);
    void cmdCommand(const std::vector<std::string>&, resp::Response&);

    // ---- 字符串 ----
    void cmdSet(const std::vector<std::string>&, resp::Response&);
    void cmdSetnx(const std::vector<std::string>&, resp::Response&);
    void cmdGetSet(const std::vector<std::string>&, resp::Response&);
    void cmdGet(const std::vector<std::string>&, resp::Response&);
    void cmdMget(const std::vector<std::string>&, resp::Response&);
    void cmdMset(const std::vector<std::string>&, resp::Response&);
    void cmdAppend(const std::vector<std::string>&, resp::Response&);
    void cmdIncrBy(const std::vector<std::string>&, resp::Response&);

    // ---- 键空间 ----
    void cmdKeys(const std::vector<std::string>&, resp::Response&);
    void cmdType(const std::vector<std::string>&, resp::Response&);
    void cmdExists(const std::vector<std::string>&, resp::Response&);
    void cmdDel(const std::vector<std::string>&, resp::Response&);
    void cmdExpire(const std::vector<std::string>&, resp::Response&);
    void cmdExpireAt(const std::vector<std::string>&, resp::Response&);
    void cmdTtl(const std::vector<std::string>&, resp::Response&);
    void cmdPttl(const std::vector<std::string>&, resp::Response&);
    void cmdPersist(const std::vector<std::string>&, resp::Response&);
    void cmdRename(const std::vector<std::string>&, resp::Response&);

    // ---- 列表 ----
    void cmdLpush(const std::vector<std::string>&, resp::Response&);
    void cmdRpush(const std::vector<std::string>&, resp::Response&);
    void cmdLpop(const std::vector<std::string>&, resp::Response&);
    void cmdRpop(const std::vector<std::string>&, resp::Response&);
    void cmdLlen(const std::vector<std::string>&, resp::Response&);
    void cmdLget(const std::vector<std::string>&, resp::Response&);
    void cmdLrange(const std::vector<std::string>&, resp::Response&);
    void cmdLindex(const std::vector<std::string>&, resp::Response&);
    void cmdLset(const std::vector<std::string>&, resp::Response&);
    void cmdLrem(const std::vector<std::string>&, resp::Response&);

    // ---- 哈希 ----
    void cmdHset(const std::vector<std::string>&, resp::Response&);
    void cmdHmset(const std::vector<std::string>&, resp::Response&);
    void cmdHget(const std::vector<std::string>&, resp::Response&);
    void cmdHexists(const std::vector<std::string>&, resp::Response&);
    void cmdHdel(const std::vector<std::string>&, resp::Response&);
    void cmdHgetall(const std::vector<std::string>&, resp::Response&);
    void cmdHkeys(const std::vector<std::string>&, resp::Response&);
    void cmdHvals(const std::vector<std::string>&, resp::Response&);
    void cmdHlen(const std::vector<std::string>&, resp::Response&);

    Store& store_;
};

/// 把命令结果码翻译成 RESP 追加到 out。
void emit(const Reply& reply, resp::Response& out);

/// 进程级唯一的数据库实例（故意不析构：避免静态析构与工作线程竞争）。
Store& database();

/// 规范化命令名（转大写，走 unsigned char）。
std::string upperAscii(const std::string& s);

/// 命令是否要求关闭当前连接。
bool commandClosesConnection(const std::string& command_name);
/// 命令是否要求整个进程优雅退出。
bool commandShutsDownServer(const std::string& command_name);

}  // namespace redis
