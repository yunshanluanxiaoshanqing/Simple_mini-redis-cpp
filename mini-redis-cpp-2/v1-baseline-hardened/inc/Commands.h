#pragma once

// Commands.h — 命令解析与分发
//
// 相对 01/02/03 的改进：
//   * 分发用哈希表 + 命令元数据（arity/写标志），未知命令返回带命令名的标准错误；
//     original 的 if-else 链在命令增多后既慢又容易漏（"Lget" 之类的拼写错误
//     在原实现里直接导致命令失效）。
//   * 命令名大小写转换使用 unsigned char 转换，避免非 ASCII 字节的 UB
//     （原实现 `::toupper` 直接传 char）。
//   * 参数校验（arity、整数解析、选项解析）集中在 handler，不散落在每个分支。

#include <string>
#include <unordered_map>
#include <vector>

#include "Resp.h"
#include "Storage.h"

namespace redis {

/// 命令执行器：无状态（只持有一个数据库引用），可被多个连接线程共享。
class Executor {
public:
    explicit Executor(Store& store) : store_(store) {}

    /// 执行一条已解析的命令（args[0] 为命令名）。
    /// 结果写入 out（RESP 字节流追加到 out 末尾）。
    void execute(const std::vector<std::string>& args, resp::Response& out);

private:
    using Handler = void (Executor::*)(const std::vector<std::string>&, resp::Response&);

    struct CommandSpec {
        Handler handler;
        int arity;       ///< >0：参数个数必须恰为该值；<0：至少 |arity| 个
        bool is_write;
    };

    const std::unordered_map<std::string, CommandSpec>& table();

    // ---- 命令实现 ----
    void cmdPing(const std::vector<std::string>&, resp::Response&);
    void cmdEcho(const std::vector<std::string>&, resp::Response&);
    void cmdFlushAll(const std::vector<std::string>&, resp::Response&);
    void cmdSet(const std::vector<std::string>&, resp::Response&);
    void cmdGet(const std::vector<std::string>&, resp::Response&);
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
    void cmdHset(const std::vector<std::string>&, resp::Response&);
    void cmdHmset(const std::vector<std::string>&, resp::Response&);
    void cmdHget(const std::vector<std::string>&, resp::Response&);
    void cmdHexists(const std::vector<std::string>&, resp::Response&);
    void cmdHdel(const std::vector<std::string>&, resp::Response&);
    void cmdHgetall(const std::vector<std::string>&, resp::Response&);
    void cmdHkeys(const std::vector<std::string>&, resp::Response&);
    void cmdHvals(const std::vector<std::string>&, resp::Response&);
    void cmdHlen(const std::vector<std::string>&, resp::Response&);
    void cmdDbsize(const std::vector<std::string>&, resp::Response&);
    void cmdQuit(const std::vector<std::string>&, resp::Response&);
    void cmdShutdown(const std::vector<std::string>&, resp::Response&);

    Store& store_;
};

/// 进程级唯一的数据库实例。
/// 故意以指针形式持有并永不释放：避免静态析构顺序问题（工作线程可能在
/// 静态析构之后仍在访问数据库 —— 03-improved 用同样的手法，这里保持）。
Store& database();

/// 把命令结果码翻译成 RESP 追加到 out。
void emit(const Reply& reply, resp::Response& out);

/// 把命令结果码翻译成 RESP 追加到 out（别名，语义更贴近调用点）。
inline void emitReply(const Reply& reply, resp::Response& out) { emit(reply, out); }

/// 命令是否要求关闭当前连接（QUIT）。
bool commandClosesConnection(const std::string& command_name);

/// 命令是否要求整个进程优雅退出（SHUTDOWN）；返回是否被接受。
bool commandShutsDownServer(const std::string& command_name);

}  // namespace redis
