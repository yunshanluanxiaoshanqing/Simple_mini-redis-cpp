# v1-baseline-hardened —— 01-original 的改进版

---

## 一、01-original 的缺点

先明确一点：01-original 已经"能编译、能运行"，它的问题不是拼写错误，而是
**设计层面的缺陷**——每一个都能用很小的输入复现，并且其中三个会直接导致
数据丢失或进程崩溃。

### A. 会丢数据 / 破坏数据

| # | 缺点 | 现象与根因 |
|---|---|---|
| A1 | **一个 key 可以同时存在于三张表** | `RedisDatabase` 把 string/list/hash 放在 `kv_store`/`l_store`/`h_store` 三个独立 map 里，而命令层假定 key 只有一种类型。`RENAME` 只搬移"存在的那一份"，`LPUSH` 对已存在的 string key 会**再建一个 list 同名条目**。此后 `TYPE` 看到的是 string，`LLEN` 看到的是 list，`DEL` 之后残留另一份。 |
| A2 | **持久化按空白切分，值含空格/换行即损坏** | `dump()` 写 `K<key> <value>`、`L<key> <elem> <elem>`、`H<key> <field>:<value>`，`load()` 用 `operator>>` 读回。`SET k "hello world"` 重启后只剩 `hello`；`H` 记录里 value 含 `:` 也会被截断。 |
| A3 | **TTL 完全不持久化** | `dump()` 不写 `expiry_map`，重启后所有带 TTL 的 key 变成永久有效。 |
| A4 | **过期键会被写进存档** | `dump()` 不调用 `purgeExpired()`，已经过期但还没被访问的 key 会被原样落盘，重启后"复活"。 |
| A5 | **落盘不是原子的** | 直接 `ofstream` 覆盖目标文件。写到一半崩溃/被 kill，`dump.my_rdb` 就是半截文件；下次启动 `load()` 还会返回 `true`（它从不做校验），静默装载半个库。 |
| A6 | **`load()` 对损坏文件返回成功** | 未知记录类型被忽略、长度越界直接 `break`，调用方无法区分"文件不存在"和"文件损坏"。 |

### B. 会崩溃 / 卡死

| # | 缺点 | 现象与根因 |
|---|---|---|
| B1 | **信号处理里 `exit()`** | `signalHandler` 直接调用 `shutdown()`（里面 `std::cout` + 加锁 + 文件 I/O）然后 `exit(signal)`。这些都不是异步信号安全操作：dump 可能被自己打断，输出可能死锁；`exit` 的退出码还是信号号（如 2），不是 0。 |
| B2 | **退出时 `join` 会永久卡住** | `run()` 把每个连接线程存进 `std::vector<std::thread>`，循环结束后 `join` 全部。只要还有一个客户端连着不退出，`join` 就永远阻塞——进程既不停也不落盘。 |
| B3 | **`SIGPIPE` 未处理** | 客户端在服务端 `send()` 之前断开，默认动作是**终止整个进程**。任何一次"发到一半对端挂了"都能把服务器打死。 |
| B4 | **一次 `recv` = 一条命令，缓冲区 1024 字节** | `char buffer[1024]` 读完直接 `processCommand`。① 一个 `SET` 的值超过 1KB 就被截断成两条垃圾命令；② 粘包时一次收到两条命令，第二条被丢掉；③ 半包时命令被腰斩。这是**结构性**缺陷：命令边界由传输层决定，而不是由协议决定。 |
| B5 | **畸形协议回复不是 RESP** | `processCommand` 对空 token 返回 `"Err: Empty command\r\n"`——不是 `-ERR ...` 格式，严格客户端会解析失败。 |
| B6 | **`stoi` 前缀截断** | `parseRespCommand`/`expire` 等用 `std::stoi`，`"12abc"` 被当成 12，`"99999999999"` 抛异常。输入校验完全缺失。 |
| B7 | **端口参数不做范围校验** | `port = std::stoi(argv[1])` 之后直接 `htons(port)`；传 `99999` 之类的值会静默截断到错误端口。 |
| B8 | **bind 失败仍返回 0** | `run()` 在 `bind`/`listen` 失败时只打印错误就返回 `void`，`main` 一律 `return 0`。启动失败的服务器对脚本来说"成功了"。 |

### C. 结构与工程问题

| # | 缺点 | 说明 |
|---|---|---|
| C1 | `del()` 先算 `erased` 再 `|=`，但作者已修；**TTL 三处不清理**的同类问题仍在 `rename`（不搬 TTL 冲突处理）等处 | 作者修了 `set/del/flushAll`，但 `rename` 的目标 key 残留、`removeIfEmpty` 不存在等问题依旧。 |
| C2 | **过期清理依赖"恰好有人来访问"** | `purgeExpired()` 只在被显式调用时执行，`DUMP`/`KEYS` 路径不全，过期 key 会长期可见（并被落盘）。 |
| C3 | **命令分发是 40 行 if-else 链** | 命令名大小写错误（`"Lget"`/`"LLNE"`）在编译期无法发现，作者已经因此踩过一次坑。 |
| C4 | **`::toupper(char)` 是 UB** | 非 ASCII 字节（≥0x80）传给 `::toupper` 属未定义行为。 |
| C5 | **CMake 不开任何警告** | `-Wall -Wextra` 完全缺席，所以"缺分号、`stD::cerr`、缺 return"这类问题才需要人来读代码发现。 |
| C6 | 无客户端、测试依赖 `nc` | 测试脚本要求 WSL/Linux + Netcat，Windows 上无法验证任何语义。 |

---

## 二、改进方向

一句话：**把"命令边界、类型归属、数据落盘"这三件最容易出错的事，从
"靠约定"改成"靠类型与代码结构保证"。**

| 方向 | 具体做法 |
|---|---|
| 命令边界 → 协议决定 | 新增增量解析器 `resp::RequestParser`：`feed()` 喂字节、`next()` 出完整命令，天然正确处理粘包/半包/流水线/大报文；带总量上限防内存耗尽。 |
| 类型归属 → 类型系统保证 | 单一键空间 `unordered_map<string, Entry>`，`Val = variant<string, List, Hash>`。"一个 key 一个类型"成为**类型不变量**，而不是注释里的约定。 |
| 校验与修改 → 原子化 | 把"判类型 + 改数据"合并为 `Store` 的单个方法，全部在同一把锁内完成，命令层不再有 check-then-act（TOCTOU）。 |
| 退出 → 只置标志 | signal handler 只做 `write()`/`store()`；主循环用**带超时的 accept** 定期检查退出标志（阻塞 `accept` 无法被信号可靠唤醒，这正是原版必须在 handler 里 `exit()` 的根因）。 |
| 落盘 → 可校验 | 长度前缀二进制安全格式 + 剩余 TTL + 临时文件原子替换 + `load` 区分 NotFound/Corrupt。 |
| 工程 → 可验证 | CMake 开 `-Wall -Wextra -Wpedantic` 零警告；新增 `--selftest`（进程内语义自检）与 PowerShell 集成测试（不依赖 `nc`）。 |

---

## 三、改进措施与实现位置

### 1. 协议层：粘包/半包/流水线/大报文（解决 B4、B5、B6）

- `inc/Resp.h` + `src/Resp.cpp`
  - `resp::RequestParser`：读游标 + 按需压缩缓冲（避免 `erase(0,n)` 的 O(n²) 搬移）。
  - `Limits{max_inline_line, max_buffer, max_args, max_arg_len}`：声明长度超过上限**立即**判协议错误，攻击者无法用一个巨大的 `$长度` 让连接永久等待。
  - `parseDecimal()` 自己实现十进制解析：**拒绝** `"12abc"`、拒绝溢出（`std::stoi` 两个问题都解决）。
  - `splitInline()` 支持双引号与反斜杠转义的内联命令。
- `inc/Client.h` + `src/Client.cpp`：把"一个连接"显式建模（socket + 解析器 + 发送缓冲），一次 `recv` 可产出多条命令，响应累积后统一 `sendAll`。
- 协议错误统一为 `-ERR Protocol error: ...` 并关闭连接（与 Redis 一致）。

### 2. 数据层：单一键空间 + 原子命令（解决 A1、C2）

- `inc/Storage.h` + `src/Storage.cpp`
  - `std::unordered_map<std::string, Entry>`，`Entry{Val value; bool has_expire; ExpireAt expire_at;}`。
  - 每个命令一个方法（`set/get/lpush/hset/...`），`lookup()` 在锁内做惰性过期删除；类型不符返回 `Code::WrongType`，**不会**偷偷改类型。
  - `SET` 覆盖任意旧类型；`LPUSH/HSET` 打到异类型 key 返回 `WRONGTYPE` 而不是"再建一份"。
  - `removeIfEmpty()`：列表/哈希清空即删 key（含 TTL 记录）。
  - `Reply{Code, number, text, items}`：命令层只做"语义码 → RESP"翻译，不再二次判断状态。

### 3. 过期语义（解决 A3、A4）

- TTL 用 `steady_clock` 时间点存储（不受系统时间调整影响）。
- `dump()` 为每个 key 记录**剩余 TTL 毫秒**；`load()` 重建为 `now + remain`。
- 新增 `TTL`/`PTTL`/`PERSIST`/`EXPIREAT`；`SET` 支持 `EX/PX/NX/XX/KEEPTTL`；`EXPIRE` 非正数立即删除（对齐 Redis）。
- `dump()` 先清理过期键，过期数据不会"复活"。

### 4. 持久化：二进制安全 + 原子替换（解决 A2、A5、A6）

- 格式：`K <klen> <vlen> <ttl_ms>\n<key><value>`、`L`/`H` 同理，长度前缀 → 值可含空格、换行、`:`、`\0`。
- 写 `.tmp` → flush → `fsync` → 回读校验 → `rename` 原子替换。
- `load()` 返回 `LoadResult{Ok, NotFound, Corrupt, IoError}`，`main` 对四种情况分别提示，损坏文件**不会被覆盖**（下次成功 dump 才覆盖）。

### 5. 网络与生命周期（解决 B1、B2、B3、B7、B8）

- `inc/NetCompat.h` + `src/NetCompat.cpp`：RAII `net::Socket`（析构自动关闭，禁止拷贝、允许移动）、`sendAll` 处理短写、`waitReadable(ms)` 用 `select` 实现跨平台带超时等待、`net::init()` 里 `signal(SIGPIPE, SIG_IGN)`。
- `inc/Server.h` + `src/Server.cpp`
  - signal handler 只 `store()` + `write()` 自管道；真正的清理在主循环 → dump 一定完整，退出码为 0。
  - 主循环 `waitReadable(50)` → `accept()`：**信号能唤醒它**，不再需要 `exit()`。
  - 记录每个连接，退出时先 `close` 掉所有连接再 `join`，绝不会卡死。
  - `bind`/`listen`/`socket` 失败 → `run()` 返回 `false` → `main` 退出码 1。
- `src/main.cpp`：`--port` 校验 1..65535、`--dump` 指定路径、`--selftest` 自检模式。

### 6. 键模式匹配（C3 的配套）

- `inc/Glob.h` + `src/Glob.cpp`：手写 `globMatch()`，支持 `*`、`?`、`[a-z]`、`[^...]`、`\` 转义。
  - 用"最近星号 + 单层回退"的贪心实现，**不递归**，避免指数回溯。
  - 不再每次请求构造 `std::regex`（02/03 的 `globToRegex` 每次调用都编译正则）。

### 7. 工程与可验证性（解决 C5、C6）

- `CMakeLists.txt`：`-Wall -Wextra -Wpedantic`，**本次构建零警告**；`ctest` 注册 `selftest`。
- `--selftest`：进程内直接驱动 `Store/Executor/RequestParser`，覆盖协议边界、类型不变量、过期、持久化（含二进制安全/损坏识别）、并发原子性 —— **104 项全部通过**。
- `tests/run_tests.ps1`：Windows 原生集成测试，**60 项全部通过**（含真实 socket 的粘包/半包/协议错误、`SHUTDOWN` 后进程真的退出、端口占用与非法参数的退出码）。
- `client/main.cpp`：配套 CLI（单命令 `-c` / `--stdin` 流水线 / 交互 REPL / `--raw`）。

---

## 四、验证结果

```
构建（MinGW g++ 16.1.0, -Wall -Wextra -Wpedantic）   0 警告 0 错误
./build/v1-server --selftest                          104 通过 / 0 失败
pwsh -File tests/run_tests.ps1                        60 通过 / 0 失败
```

复现方式：

```powershell
cd v1-baseline-hardened
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
.\build\v1-server.exe --selftest
pwsh -File tests/run_tests.ps1
```

---

## 五、本版本**有意保留**的限制

这些不是遗漏，而是"基线版"的边界（v2/v3 才解决）：

1. **每连接一线程**：连接数 = 线程数，10k 连接即 10k 线程。v3 用线程池解决。
2. **无空闲超时**：客户端连上不发数据会一直占着一个线程；只有 v2/v3 有超时。
3. **无内存上限**：一个巨大的 `SET` 可以打爆内存。v2 引入 `MAXMEMORY`。
4. **持久化文件无魔数/无 CRC**：只能发现"格式不对"，不能发现"字节被翻转"。v2 引入 `MINIRDB2 + CRC32`。
5. **落盘期间持有数据库锁**：大库落盘时所有客户端一起卡。v3 改成"锁内快照 + 锁外写盘"。
6. **过期清理是 O(TTL 键数)**：`lookup()` 内线性扫描 `expiry_map`。v3 用时间轮。
7. 命令集有限（无 `INCR`/`APPEND`/`MSET`/`INFO`/`CONFIG`）：v2/v3 逐步补齐。

---

## 六、目录结构

```
v1-baseline-hardened/
├── CMakeLists.txt                  # -Wall -Wextra -Wpedantic，ctest 集成
├── README.md                       # 使用说明
├── IMPROVEMENTS.md                 # 本文档：缺点 → 方向 → 措施
├── inc/
│   ├── NetCompat.h                 # RAII socket + 跨平台 poll/select + 超时等待
│   ├── Resp.h                      # 增量 RESP 解析器 + 响应构造器
│   ├── Glob.h                      # 键模式匹配（无正则）
│   ├── Storage.h                   # 单一键空间 + 原子命令 + Reply 语义码
│   ├── Commands.h                  # 命令表 + 分发
│   ├── Client.h                    # 单连接状态机
│   ├── Server.h                    # accept 循环 + 优雅退出
│   └── Selftest.h
├── src/
│   ├── NetCompat.cpp  Resp.cpp  Glob.cpp
│   ├── Storage.cpp    Commands.cpp  Client.cpp
│   ├── Server.cpp     main.cpp      Selftest.cpp
├── client/main.cpp                 # redis-cli（单命令 / 流水线 / REPL）
└── tests/run_tests.ps1             # 60 项集成测试
```
