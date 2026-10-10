# v2-fixed-robust —— 02-fixed 的改进版


---

## 一、02-fixed 的缺点

02-fixed 已经把 01-original 的 25 类问题逐条修完（见其 `CHANGES.md`），
命令语义基本对齐 Redis。但它**刻意保留**了原来的网络模型与平台限制，
而且新引入的修复本身留下了新的结构性缺口。

### A. 数据完整性：修了"格式"，没修"信任"

| # | 缺点 | 现象与根因 |
|---|---|---|
| A1 | **持久化文件没有任何完整性标识** | 格式是"长度前缀"，但没有魔数、没有版本号、没有校验和。文件被截断、被别的东西覆盖、被文本编辑器改过一个字节，`load()` 都只能靠"格式恰好还对"来猜；一旦记录头被破坏，可能装进半个库而返回成功。 |
| A2 | **`load()` 的返回值无法区分四种情况** | `bool load()`：文件不存在、读失败、格式损坏都返回 `false`，只有成功返回 `true`。调用方（`main`）无法给出准确提示，也无法决定"要不要保留旧文件"。 |
| A3 | **落盘仍会先破坏旧文件** | 直接写目标文件（无 `.tmp` + rename）。写到一半崩溃，旧存档和新数据**同时**没了。 |
| A4 | **`dump()` 不检查写入是否真的成功** | `ofs.good()` 未检查；磁盘满、权限变更都会静默返回 `true`。 |
| A5 | **修复引入的 TOCTOU 仍在** | 02-fixed 的类型检查是"命令层先 `db.type(key)`，再调用 `db.lpush(...)`"。多线程下两步之间别人可以把 key 改成 string —— 检查通过、操作打空。**02-fixed 从头到尾没有在数据层做原子性**。 |
| A6 | **`RENAME` 的目标 key 残留** | 02-fixed 修了"覆盖目标"，但 `expiry_map` 与三张表分别清理，`oldkey==newkey` 之外的边界（例如 old 在 `l_store`、new 在 `h_store`）仍靠"三处都 erase"来兜，依赖人工列举完整。 |
| A7 | **`std::regex` 每次请求重编译** | `handleKeys` 里 `std::regex re(globToRegex(pattern))` 对每个 `KEYS` 调用都编译一次正则；glob→regex 的转义集合靠手写字符表，容易漏。 |

### B. 稳定性与资源

| # | 缺点 | 现象与根因 |
|---|---|---|
| B1 | **`SIGPIPE` 未处理** | POSIX 上默认动作是终止进程。客户端在响应写回前断开，服务器直接死。02-fixed 继承了 01 的这个缺陷。 |
| B2 | **无空闲连接超时** | 每个连接一个线程，客户端连上不发数据就一直占着线程与 fd。几百个空闲连接就能把线程数、fd 数、内存吃光（慢速攻击）。 |
| B3 | **无内存上限** | 一个 `SET big <几百 MB>` 或 `RPUSH big a a a ...` 可以让进程被 OOM killer 杀掉，且杀掉时来不及落盘。 |
| B4 | **单次 `recv` 一条命令的模型没改** | 02-fixed 明确"保留原始网络模型"：`char buffer[1024]`，一次 `recv` 一条命令。超过 1KB 截断、粘包丢命令、半包腰斩。 |
| B5 | **畸形协议的回复不是标准 RESP** | 仍是 `"Err: Empty command\r\n"`。 |
| B6 | **`std::stoi` 前缀截断** | `"12abc"` 被当 12。 |
| B7 | **`bind` 失败仍返回 0** | `main` 一律 `return 0`。 |
| B8 | **端口不做范围校验** | `htons` 会静默截断。 |
| B9 | **恢复的是"绝对 TTL"而非"剩余 TTL"** | 02-fixed 的 `E` 记录写 `remain_ms`（这点是对的），但它依赖落盘时刻的 `steady_clock`；若落盘与加载之间进程被挂起（laptop 睡眠），`steady_clock` 不前进会低估已过时间，TTL 会**偏长**。 |
| B10 | **`purgeExpired()` 全表扫描** | 每次 `get/type/keys/...` 都遍历整个 `expiry_map`。带 TTL 的 key 一多，每条命令的代价都线性上升。 |

### C. 工程与可验证性

| # | 缺点 | 说明 |
|---|---|---|
| C1 | **仅 POSIX，Windows 不可编译** | 这是 02-fixed 自己声明的限制，但意味着"修复版"无法在作者的主力平台（Windows）上验证。 |
| C2 | **客户端与服务器各自实现一套 RESP 解析** | `02-fixed/client/main.cpp` 独立实现编码/解析，两边行为可能分叉。 |
| C3 | **无自检入口** | 语义验证完全依赖 `tests/run_tests.sh` + `nc`；一旦没有 `nc` 或端口冲突，什么也验证不了。 |
| C4 | **CMake 不开警告** | 仍无 `-Wall -Wextra`。 |
| C5 | **错误回复缺命令名** | `-ERR unknown command` 不带具体命令，排障困难。 |

---

## 二、改进方向

v2 的目标不是"再对齐一批 Redis 语义"，而是**让"数据完整性"和"资源安全"
成为可以被测试证明的属性**：

| 方向 | 具体做法 |
|---|---|
| 文件可信 | `魔数 + 版本号 + 记录区 + CRC32`；CRC 不匹配即判损坏，**绝不装载**；同时兼容读取 v1 旧格式并在日志中说明。 |
| 写入可信 | 临时文件 → flush → fsync → **回读逐字节比对** → 原子替换；失败时保留旧文件。 |
| 资源可控 | 单库内存上限（`MAXMEMORY` 命令 + 环境变量），超限返回 `-OOM`；连接空闲超时。 |
| 状态可观测 | `INFO` 输出 keys / used_memory / maxmemory / 持久化格式 / 快照状态。 |
| 原子性下沉 | 类型检查与写入合并到数据层的单方法内（v1 已建立，v2 继续并覆盖内存上限判断）。 |
| 平台与测试 | 跨平台 socket 层 + `--selftest`（124 项） + PowerShell 集成测试（72 项）。 |

---

## 三、改进措施与实现位置

### 1. 持久化：魔数 + 版本 + CRC32（解决 A1、A2、A3、A4）

文件布局（`inc/Storage.h` 定义常量，`src/Storage.cpp` 实现）：

```
[0..7]   魔数 "MINIRDB2"
[8]      版本号（当前 2）
[9..]    记录区：K/L/H 记录（长度前缀 + 剩余 TTL）
[末尾]   记录区的 CRC32（小端 4 字节）
```

- `crc32()`：`std::call_once` 惰性初始化 256 项查表（IEEE 反射多项式 `0xEDB88320`）。
- `save()`：组装完整字节流 → 写 `.tmp` → `fsync` → **回读并与内存逐字节比对**（能发现磁盘/网络盘的静默损坏）→ `rename` 原子替换。失败时清理 `.tmp`，**旧文件不动**。
- `load()` 返回新增的 `LoadResult::LegacyFormat`：
  - 有魔数：校验版本 + CRC32，不符 → `Corrupt`；
  - 无魔数：按 v1 格式解析，成功 → `LegacyFormat`（**升级不丢数据**，退出时会被重新写成新格式）；
  - `count > body_size` 之类的荒谬计数直接判 `Corrupt`（防畸形文件把内存吃光）。
- `main.cpp` 对 `Ok / LegacyFormat / NotFound / Corrupt / IoError` 分别提示；**损坏时明确说明旧文件在下次成功落盘前不会被改写**。

### 2. 内存上限（解决 B3）

- `Store::setMemoryLimit()` + `config_.maxmemory`；`wouldExceedLimit(incoming)` 在实际增长前判断。
- 覆盖写路径：`SET` 会先扣掉被替换的旧值大小，避免"改小值也被拒"。
- 触发时返回 `Code::OutOfMemory` → RESP 回复 `-OOM command not allowed when used memory > 'maxmemory'`（Redis 同款错误码，客户端能按错误类型处理）。
- 运行期可查可改：`MAXMEMORY`（查看/设置）、`INFO` 报告 `used_memory` 与 `maxmemory`；也可用环境变量 `MINI_REDIS_MAXMEMORY` 在启动时设定。

### 3. 空闲连接超时（解决 B2）

- `Server` 构造函数新增 `idle_timeout_ms`（默认 300000，`--timeout` 可改，0 = 不限制）。
- 连接线程在"需要更多数据"时先调用 `Socket::waitReadable(idle_timeout_ms)`；超时即打印原因并关闭。
- 该等待同时让线程能及时观察到进程退出标志，缩短停机时间。

### 4. 网络与退出（解决 B1、B7、B8）

- `net::init()` 内 `signal(SIGPIPE, SIG_IGN)`：对端断开只会让 `send` 返回 `EPIPE`，不再打死进程。
- `Socket::setReuseAddr(true)` 在 Windows 上使用 `SO_EXCLUSIVEADDRUSE`（`SO_REUSEADDR` 在 Windows 允许双绑定，反而危险）。
- 端口参数校验 1..65535；`bind/listen/socket` 失败 → `run()` 返回 `false` → 退出码 1。
- `shutdown()` 路径打印分阶段日志（stopping → listener closed → N connections closed → threads joined → dumped），出问题时一眼能看出卡在哪一步。

### 5. 保持 v1 已获得的结构性修复

v2 直接建立在 v1 的代码结构上，因此自动获得：

- 单一键空间 + `variant` 类型不变量（解决 A5、A6）；
- 增量 RESP 解析器（解决 B4、B5、B6）；
- 手写 glob 匹配、不编译正则（解决 A7）；
- 大小写安全的命令名转换、带命令名的未知命令错误（解决 C5）；
- `-Wall -Wextra -Wpedantic` 零警告（解决 C4）。

### 6. 可验证性（解决 C3）

- `--selftest`：在 v1 的 104 项之上，新增
  - 魔数/版本号存在性检查；
  - **翻转 1 个 bit 必须被判损坏**；
  - 截断文件必须被判损坏；
  - 未知版本号必须被拒；
  - v1 旧格式必须能读入并报告 `LegacyFormat`；
  - 内存上限：超限 `-OOM`、超限后 key 不存在、列表同样受限、解除限制后成功；
  - `INFO` 字段存在性。
  → **124 项全部通过**。
- `tests/run_tests.ps1`：在 v1 的 60 项之上新增 `MAXMEMORY` / `INFO` / **CRC 损坏后重启为空库** / 日志说明完整性校验失败。→ **72 项全部通过**。

---

## 四、验证结果

```
构建（MinGW g++ 16.1.0, -Wall -Wextra -Wpedantic）   0 警告 0 错误
./build/v2-server --selftest                          124 通过 / 0 失败
pwsh -File tests/run_tests.ps1                        72 通过 / 0 失败
```

手工验证过的关键场景（真实 socket）：

| 场景 | 结果 |
|---|---|
| 空闲 800ms 的连接（`--timeout 800`） | 服务端打印 `closing idle connection after 800 ms` 并关闭 |
| `SHUTDOWN` | 打印分阶段日志，进程真的退出，dump 已写出 |
| 重启 | `Loaded v2.rdb (format MINIRDB2+crc32)`，数据含空格完整恢复 |
| 存档翻转 1 bit 后重启 | `failed integrity check (magic/version/CRC32); starting empty`，`DBSIZE` = 0 |
| `MAXMEMORY 1000000` + 8KB 写入 | 返回 `-OOM ...`，key 未写入 |

---

## 五、本版本**有意保留**的限制

1. **每连接一线程**：线程数仍随连接数线性增长（v3 用线程池解决）。
2. **无最大连接数限制**：超出 fd 上限后行为取决于 OS（v3 用 `maxclients` + 队列上界解决）。
3. **落盘期间持有数据库锁**：`save()` 全程持锁（v3 改成"锁内快照 + 锁外写盘"）。
4. **`purgeExpired()` 全表扫描**：`ExpiryMap` 仍是哈希表逐个扫描（v3 用时间轮）。
5. **无后台自动落盘**：只有启动/退出/`SHUTDOWN` 时落盘（03-improved 有周期落盘；v3 提供可配置的后台快照线程）。
6. **无 `CONFIG` 命令**：配置只能在启动参数/环境变量给定（v3 支持 `CONFIG GET|SET` 热改）。
7. 命令集仍与 02-fixed 对齐（无 `INCR`/`APPEND`/`MSET`，v3 补齐）。

---

## 六、目录结构

```
v2-fixed-robust/
├── CMakeLists.txt                  # -Wall -Wextra -Wpedantic，ctest 集成
├── README.md
├── IMPROVEMENTS.md                 # 本文档
├── inc/
│   ├── NetCompat.h                 # RAII socket + SIGPIPE 忽略 + 带超时等待
│   ├── Resp.h                      # 增量 RESP 解析器
│   ├── Glob.h                      # 键模式匹配
│   ├── Storage.h                   # 单一键空间 + CRC32 + 内存上限
│   ├── Commands.h                  # 命令表（含 INFO / MAXMEMORY）
│   ├── Client.h
│   ├── Server.h                    # 空闲超时 + 优雅退出
│   └── Selftest.h
├── src/
│   ├── NetCompat.cpp  Resp.cpp  Glob.cpp
│   ├── Storage.cpp    Commands.cpp  Client.cpp
│   ├── Server.cpp     main.cpp      Selftest.cpp
├── client/main.cpp
└── tests/run_tests.ps1             # 72 项集成测试（含 CRC 损坏场景）
```
