# mini-redis-cpp-2 —— 三个版本的改进实现

本目录是对 `mini-redis-cpp/` 中三个版本（`01-original` / `02-fixed` / `03-improved`）
的**改进实现**：先逐版分析缺点与改进方向，再在新目录中重写实现，并配套自检与集成测试。

| 目录 | 基线 | 定位 | 自检 | 集成测试 |
|---|---|---|---|---|
| [`v1-baseline-hardened/`](v1-baseline-hardened/) | `01-original` | 最小规模，修掉会造成数据损坏/崩溃/无法退出的结构性问题 | 104 / 0 | 60 / 0 |
| [`v2-fixed-robust/`](v2-fixed-robust/) | `02-fixed` | 语义正确之上加固数据完整性与资源安全 | 124 / 0 | 72 / 0 |
| [`v3-improved-scalable/`](v3-improved-scalable/) | `03-improved` | 线程池、快照式落盘、时间轮、热配置、可观测 | 151 / 0 | 60 / 0 |

三版对比、实测数据与选型建议见 **[COMPARISON.md](COMPARISON.md)**；
每版"缺点 → 改进方向 → 措施"的完整清单见各自的 `IMPROVEMENTS.md`。

---

## 快速开始（Windows / MinGW-w64）

需要 CMake ≥ 3.16 与支持 C++17 的编译器；本仓库在
MinGW-w64 g++ 16.1.0 + CMake 4.3.3 下验证通过。

```powershell
# 以 v3 为例
cd v3-improved-scalable
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

.\build\v3-server.exe --selftest                 # 151 项进程内语义自检
pwsh -File tests/run_tests.ps1                   # 60 项网络集成测试
.\build\v3-server.exe --port 6379 --dump dump.my_rdb
.\build\v3-cli.exe -p 6379 SET hello world       # 另开一个终端
.\build\v3-cli.exe -p 6379                         # 交互式 REPL
```

Linux / macOS：把生成器换成默认值即可（`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`），
测试脚本为 PowerShell，需要 `pwsh`（也可以用 `tests/` 目录下的方式手工验证）。

```bash
cd v3-improved-scalable
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/v3-server --selftest
```

---

## 三版共同的技术骨架

三版都建立在同一套正确性基础上（这正是它们与原始三版最大的区别）：

| 组件 | 文件 | 作用 |
|---|---|---|
| 跨平台 socket | `inc/NetCompat.h` `src/NetCompat.cpp` | RAII 句柄、`sendAll` 处理短写、带超时的 `waitReadable`、`SIGPIPE` 忽略、Windows `SO_EXCLUSIVEADDRUSE` |
| RESP 协议 | `inc/Resp.h` `src/Resp.cpp` | 增量解析器（粘包/半包/流水线/大报文）、严格整数解析、响应构造器 |
| 键模式匹配 | `inc/Glob.h` `src/Glob.cpp` | 手写线性 glob（`*` `?` `[a-z]` `[^..]` `\` 转义），不编译正则 |
| 数据层 | `inc/Storage.h` `src/Storage.cpp` | 单一键空间 + `variant` 类型不变量；类型检查与修改在同一把锁内原子完成 |
| 命令层 | `inc/Commands.h` `src/Commands.cpp` | 命令表 + arity 校验；语义码 → RESP 只有一处翻译 |
| 连接层 | `inc/Client.h` `src/Client.cpp` | 单连接状态机（解析器 + 发送缓冲） |
| 服务层 | `inc/Server.h` `src/Server.cpp` | accept 循环、退出标志、优雅停机（v3 另有线程池与后台快照） |
| 自检 | `src/Selftest.cpp` | `--selftest`：不依赖网络与外部工具即可验证语义 |

---

## 命令集

`v1` 与 `v2` 在原始三版命令集上补齐了 TTL 相关命令：

```
PING ECHO SET GET DEL UNLINK EXISTS TYPE KEYS FLUSHALL DBSIZE
EXPIRE EXPIREAT TTL PTTL PERSIST RENAME
LPUSH RPUSH LPOP RPOP LLEN LGET LRANGE LINDEX LSET LREM
HSET HMSET HGET HEXISTS HDEL HGETALL HKEYS HVALS HLEN
QUIT SHUTDOWN
```

`v2` 额外：`INFO` `MAXMEMORY`

`v3` 额外：`APPEND INCR DECR INCRBY DECRBY SETNX GETSET MGET MSET COMMAND CONFIG`

`SET` 支持 `EX` / `PX` / `NX` / `XX` / `KEEPTTL`；`LPOP`/`RPOP` 支持 `count`；
`HSET` 返回新建字段数（`:1`/`:0`）；`EXPIRE` 返回 `:1`/`:0`；
类型冲突统一 `-WRONGTYPE ...`；参数错误统一 `-ERR wrong number of arguments for 'x' command`。

---

## 三个改进版分别解决了什么

### v1 —— 让骨架正确

- **一个 key 只能有一个类型**：单一键空间 + `variant`，从类型层面消除"三张表同名 key"。
- **命令边界由协议决定**：增量解析器正确处理粘包/半包/流水线/大报文。
- **优雅退出**：信号处理只置标志，主循环用带超时的 accept 轮询；退出时先断开连接再 join，
  落盘一定完成，退出码为 0。
- **存档可信**：长度前缀（二进制安全）+ 剩余 TTL + 临时文件原子替换 + 损坏识别。

### v2 —— 让数据不被"悄悄弄坏"

- **魔数 + 版本 + CRC32**：翻转 1 bit 即判损坏，绝不装载；兼容读取 v1 旧格式。
- **落盘可验证**：写 `.tmp` 后回读逐字节比对，再原子替换；失败时旧文件不动。
- **资源护栏**：`MAXMEMORY`（超限 `-OOM`）、空闲连接超时。

### v3 —— 让规模与延迟可控

- **线程池 + 有界队列**：50 个空闲连接下线程数从 53 降到 8（`--threads 4`）。
- **最大连接数**：超限明确回 `-ERR max number of clients reached` 并计数。
- **快照式落盘**：锁内只拷贝数据，写盘与校验在锁外；重叠落盘短路为 `Busy`。
- **过期时间轮**：清理代价与 TTL 键数解耦，均摊 O(1)。
- **运行期配置与观测**：`CONFIG GET|SET`、`INFO`（hits/misses/expired/拒绝连接/快照次数）。
- **更多命令**：`APPEND/INCR/DECR/INCRBY/DECRBY/SETNX/GETSET/MGET/MSET/COMMAND`。

---

## 验证状态

```
                       编译警告   自检         集成测试
v1-baseline-hardened      0      104 / 0      60 / 0
v2-fixed-robust           0      124 / 0      72 / 0
v3-improved-scalable      0      151 / 0      60 / 0
```

编译条件：`Release` + `-Wall -Wextra -Wpedantic`，MinGW-w64 g++ 16.1.0。
集成测试覆盖真实 socket 的粘包/半包/大报文/协议错误、类型不变量、过期语义、
持久化与损坏检测、空闲超时、最大连接数拒绝、并发 `INCR` 精确性、
`SHUTDOWN` 后进程真的退出、端口占用与非法参数的退出码。
