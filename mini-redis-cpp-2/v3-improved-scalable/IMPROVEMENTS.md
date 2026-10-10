# v3-improved-scalable —— 03-improved 的改进版

> 位置：`mini-redis-cpp-2/v3-improved-scalable/`
> 基线：`mini-redis-cpp/03-improved`（三个版本中最新：跨平台、接收缓冲状态机、完整测试）
> 定位：**在"跨平台 + 协议健壮"已经做到的基础上，解决"规模化"层面的问题**：
> 线程数与连接数解耦、落盘不长时间阻塞服务、过期清理不随 TTL 键数增长、
> 配置可热改、运行状态可观测。

---

## 一、03-improved 的缺点

03-improved 已经修掉了前两版的大量问题（接收缓冲状态机、`-ERR Protocol error`、
跨平台 socket 层、CRC 无关但长度前缀二进制安全的持久化、132+126 项测试）。
它的问题集中在**架构与资源**层面——单机连接数、内存与锁竞争一旦上来就会暴露。

### A. 并发模型：线程数 = 连接数

| # | 缺点 | 现象与根因 |
|---|---|---|
| A1 | **每连接 `std::thread(...).detach()`** | 连接数直接决定线程数。10k 连接 = 10k 线程；每线程默认 1~8MB 栈，仅栈就要几十 GB 虚拟内存，线程切换与调度开销也会把吞吐压垮。 |
| A2 | **无最大连接数限制** | `accept` 成功后无条件 `detach` 新线程。连接数只受 OS 的 fd/线程上限约束，到达上限时表现为"accept 失败 → 打印错误 → 退出"，而不是可控地拒绝新连接。 |
| A3 | **`detach` 导致无法优雅停机** | 线程被 detach，Server 不持有任何句柄。`shutdown()` 只能关掉监听套接字，**已经在跑的连接线程既不会被打断也不会被等待**：进程可能在还有线程写数据时退出（dump 也可能与写线程并发）。 |
| A4 | **信号处理器里做 dump** | `signalHandler` 调用 `g_server->shutdown()`，其中 `RedisDatabase::dump()` 会加锁 + 文件 I/O，`std::cout` 也会加锁 —— 都不是异步信号安全操作。然后 `std::_Exit(0)` 直接跳过程序正常收尾。 |
| A5 | **无空闲/总时长超时** | 连上不发数据的连接会一直占着一个线程（慢速攻击面）。 |

### B. 持久化：锁与 I/O 耦合

| # | 缺点 | 现象与根因 |
|---|---|---|
| B1 | **落盘全程持有数据库锁** | `dump()` 第一行就是 `std::lock_guard<std::mutex> lock(db_mutex)`，之后才做序列化 + 写文件 + `fsync`。库越大、磁盘越慢，**所有客户端被阻塞得越久**（尖刺延迟）。 |
| B2 | **没有完整性校验** | 格式是长度前缀 + 剩余 TTL（比 01/02 好很多），但没有魔数、版本、CRC；字节被翻转/截断无法发现。 |
| B3 | **周期落盘无重叠保护** | 后台线程每 `REDIS_PERSIST_INTERVAL` 秒 `dump()` 一次，与信号路径的 `dump()` 可能同时进入（互斥锁能防数据竞争，但会出现"两次全量落盘排队"，且第二次的 `purgeExpired` 与第一次交错）。 |
| B4 | **落盘失败只打印一行** | 无法通过 `INFO` 之类的接口知道"上次落盘成功了没有"。 |

### C. 过期与命令执行

| # | 缺点 | 现象与根因 |
|---|---|---|
| C1 | **`purgeExpired()` 每次都全表扫描 `expiry_map`** | `get/type/keys/del/expire/hget/...` 几乎每个方法开头都调用它。带 TTL 的 key 有 10 万条时，**每条命令**都要遍历这 10 万条 —— 代价与 TTL 键数成正比。 |
| C2 | **`KEYS` 用 `std::regex`** | 每次 `KEYS` 调用都 `globToRegex()` + 构造 `std::regex`（编译正则），并且 `std::regex_match` 逐 key 匹配。模式匹配本该是线性扫描，这里成了"每请求编译一次正则"。 |
| C3 | **`std::string::erase(0, n)` 搬移** | `extractCommand()` 在循环里对接收缓冲做 `buffer.erase(0, start)` / `erase(0, pos)`。长连接 + 流水线下，每次提取都会搬移剩余数据，累计 O(n²)。 |
| C4 | **命令集偏窄** | 无 `INCR`/`APPEND`/`MSET`/`MGET`/`SETNX`/`GETSET`、无 `CONFIG`、无 `INFO`、无 `COMMAND`；`EXPIRE` 不支持 `NX/XX/GT/LT`。 |
| C5 | **运行期不可配置** | 持久化间隔只能靠环境变量在启动时定；超时、连接上限、内存上限都无法运行中调整。 |
| C6 | **无统计** | 没有 hits/misses/expired/拒绝连接/快照次数等计数，压测与排障时缺少观测面。 |

### D. 工程

| # | 缺点 | 说明 |
|---|---|---|
| D1 | `cmake_minimum_required(VERSION 4.3.3)` | 直接要求 CMake 4.3.3（非常新的版本）。任何低于它的环境直接配置失败。 |
| D2 | 客户端与服务器共用 `NetCompat`，但客户端 `hints.ai_family = AF_INET` | 只走 IPv4；`getaddrinfo` 拿到的第一个非 IPv4 结果会被忽略（不是错误，但 `-h ::1` 之类无法工作）。 |
| D3 | 无 `--selftest` 之类进程内验证入口 | 语义验证仍依赖 shell/PowerShell 脚本 + 网络。 |

---

## 二、改进方向

v3 的改进主线是**解耦**：

| 解耦对象 | 做法 |
|---|---|
| 连接数 ⊥ 线程数 | 固定大小线程池 + 有界连接队列；超出上限时**明确拒绝**而不是硬撑。 |
| 数据访问 ⊥ 磁盘 I/O | `save()` 拆成"锁内构造快照字节流"+"锁外写盘/校验/替换"。 |
| 过期清理代价 ⊥ TTL 键数 | 过期时间轮（按秒分桶），均摊 O(1) 清理。 |
| 模式匹配 ⊥ 正则编译 | 手写 glob 匹配，线性扫描、零分配。 |
| 配置 ⊥ 重启 | `CONFIG GET|SET` 热改 + `INFO` 统计。 |
| 停机 ⊥ 连接生命周期 | 线程池 `stop()` 先清队列再唤醒、限时 join；连接线程按 100ms 粒度观察退出标志。 |

---

## 三、改进措施与实现位置

### 1. 线程池 + 有界队列（解决 A1、A2、A3、A5）

`inc/ThreadPool.h` + `src/ThreadPool.cpp`：

- 固定 N 个工作线程（`--threads`，默认 = `hardware_concurrency()`，夹在 2..64），线程数**不随连接数变化**。
- `std::deque<net::Socket>` 有界队列（容量 = `maxclients`）；`submit()` 在队列满或已停止时返回 `false`。
- `stop(timeout_ms)`：先 `stopping_=true` 并**清空队列（关闭其中每个 socket）**，再 `notify_all()`，然后限时等待 `active_==0`，最后 join。不会再出现"关不掉的 accept 循环"。
- 线程内处理连接直到：对端关闭 / 出错 / 命令要求退出 / 空闲超时。

`src/Server.cpp`：

- 主循环用 `waitReadable(50)` 轮询监听套接字 → 退出信号 50ms 内被响应（阻塞 `accept` 在 Windows 上无法被 `closesocket` 唤醒）。
- 超过 `maxclients` 时先回 `-ERR max number of clients reached\r\n` 再关闭（客户端能拿到原因，而不是"连接被重置"）。
- 队列满时同样明确拒绝并计入 `rejected_connections`。

### 2. 快照式持久化（解决 B1、B2、B3、B4）

`src/Snapshot.cpp` + `src/StoreFile.cpp`：

- **两段式**：
  1. `exportSnapshot()` —— 持锁，把数据序列化成**内存里的完整文件字节流**（纯拷贝，不做 I/O）；
  2. 放锁 → 写 `.tmp` → `fsync` → **回读逐字节比对** → `rename` 原子替换。
- `saving_` 原子标志 + CAS：后台快照与 `SHUTDOWN`/`SAVE` 并发时，后到者得到 `SaveResult::Busy`（而不是排队做第二次全量落盘）。
- 文件格式沿用 v2 的 `魔数 MINIRDB2 + 版本 + 记录区 + CRC32`，因此 **v2 与 v3 的存档可互相读取**。
- `noteSnapshot(ok)` 记录成功/失败次数，`last_save_ok` 暴露给 `INFO`。
- `load()` 额外重建**过期时间轮**（把每条带 TTL 的记录重新分桶），保证 `load` 之后时间轮语义正确。

### 3. 过期时间轮（解决 C1）

`inc/Storage.h` + `src/Storage.cpp`：

- `expiry_buckets_ : map<long long 秒, vector<string>>` + `key_bucket_ : map<key, 秒>` + `wheel_cursor_`。
- `advanceExpiryWheelLocked()`：只处理 `cursor..now` 之间**存在**的桶，整桶删除，均摊 O(1)；桶是按秒分的，同一桶内可能有的键还没到点（毫秒边界），所以逐键再比一次 `now() >= expire_at`。
- `lookup()` 仍然做一次内联 deadline 检查，保证"刚过期就不可见"，不依赖时间轮被推进。
- `setExpireLocked/clearExpireLocked` 维护分桶；`persist`/`del`/`rename`/`removeIfEmpty` 都同步维护，不会留下悬空的桶条目。

### 4. 协议解析零搬移（解决 C3）

`inc/Resp.h` / `src/Resp.cpp` 使用**读游标 + 惰性压缩**：

- 消费数据只前移 `read_pos_`；只有当"已消费前缀 > 8KB 且超过缓冲一半"时才 `erase` 一次。
- 长连接 + 大流水的累计代价从 O(n²) 降到均摊 O(1)。
- 同时带 4 个上限（内联行、缓冲总量、参数个数、单参数长度），超限立即判协议错误。

### 5. 批量回写减少系统调用

`inc/Client.h` / `src/Client.cpp`：

- 一次 `recv` 内的多条流水线命令，响应累积到发送缓冲（阈值 256KB），只有"缓冲满"或"输入耗尽、即将阻塞等待"时才 `sendAll`。
- 返回 `hasPendingOutput()/outputFull()` 让连接循环做决策，I/O 策略集中在 Server 里。

### 6. 命令集与运行期配置（解决 C4、C5、C6、D1）

`inc/Commands.h` + `src/Commands.cpp`：

- 新增：`APPEND`、`INCR`/`DECR`/`INCRBY`/`DECRBY`（含**溢出保护**，不触发有符号溢出 UB）、`SETNX`、`GETSET`、`MGET`、`MSET`、`COMMAND`、`INFO`、`CONFIG GET|SET`。
- `INFO` 输出：`server / dump_format / keys / used_memory(_human) / maxmemory / keyspace_hits / keyspace_misses / expired_keys / total_commands_processed / total_connections_received / rejected_connections / snapshots / snapshot_errors / last_save_ok`。
- `CONFIG SET` 可热改 `maxmemory / timeout / maxclients / dump-interval`，非法值返回错误且**不影响已生效配置**（先复制再改）。
- `CMakeLists.txt` 的 `cmake_minimum_required` 从 `4.3.3` 降到 `3.16`（解决 D1）。

### 7. 优雅停机（解决 A4 的遗留风险）

- `installSignalHandlers()` 只做 `store()` + 自管道 `write()`（异步信号安全）。
- 停机顺序：`listener_.reset()` → 唤醒并 join 后台快照线程（先落盘）→ `pool_.stop(5000)`（清队列、限时 join）→ 最终落盘 → 打印统计。
- 连接线程每 100ms 检查一次 `running_`/退出标志，因此最多 100ms 内让出。

---

## 四、验证结果

```
构建（MinGW g++ 16.1.0, -Wall -Wextra -Wpedantic）   0 警告 0 错误
./build/v3-server --selftest                          151 通过 / 0 失败
pwsh -File tests/run_tests.ps1                        60 通过 / 0 失败
```

`--selftest` 相比 v2 新增的用例：

| 类别 | 用例 |
|---|---|
| 新增命令 | `SETNX`（新/已存在）、`APPEND`（新建/追加/WRONGTYPE）、`INCR/DECR/INCRBY/DECRBY`、`INCR` 非整数值、**`INCR` 溢出保护**、`GETSET`（旧值/nil）、`MSET`/`MGET`（含缺 key 返回 nil）、`MSET` 奇数参数、`COMMAND` 列表 |
| 时间轮 | 设 1 秒 TTL → 睡 1.2 秒 → `EXISTS`=0、`TYPE`=none、`KEYS short*` 为空、`expired_keys` 计数增加；**`load()` 重建时间轮后仍生效** |
| 快照 | 魔数/版本存在、**翻转 1 bit 判损坏**、原始字节流可正常导入、导入后内容正确 |
| 配置 | `CONFIG SET/GET maxmemory`、`CONFIG GET *`、`CONFIG GET` 未知参数、`CONFIG` 未知子命令（保留输入大小写）、非法值、解除限制 |
| 统计 | `INFO` 含 `server/keys/keyspace_hits/snapshots/expired_keys/used_memory_human` |
| 并发 | 4 线程 × 500 次 RPUSH+LLEN 计数一致；**4 线程 × 300 次并发 `INCR` 不丢更新**（读改写原子性） |

真实网络验证（`--threads 4 --maxclients 4`）：

| 场景 | 结果 |
|---|---|
| 4 个空闲连接占满 | 第 5 个连接收到 `-ERR max number of clients reached` |
| 释放后 | 正常客户端立即恢复 `PONG` |
| 20 客户端 × 40 次 `INCR` | 客户端 0 失败，服务端计数 **800**（无丢失） |
| `INFO` | 观察到 `keys/used_memory/keyspace_hits/total_connections_received` 等字段 |
| `SHUTDOWN` | 进程退出，`v3.rdb` 写出（`MINIRDB2+crc32`） |

---

## 五、本版本仍然存在的限制（诚实标注）

1. **单机单进程**：没有主从/集群；`INFO` 里的 `rejected_connections` 只能事后观测，没有自动降级策略。
2. **快照期间的内存峰值**：`exportSnapshot()` 会在内存里再放一份完整数据（约数据量的 2 倍峰值）；超大库应换成增量/AOF 方案（本版本明确未实现 AOF）。
3. **`KEYS` 仍是 O(全部键)**：手写 glob 只解决了"每请求编译正则"，没解决遍历成本；生产应改用游标式 `SCAN`（本版本未实现）。
4. **全局单锁**：所有命令串行通过一把 `db_mutex`。这是"简单且正确"的选择；要更高并发需要分片或无锁结构。
5. **`dump-interval` 与命令并发**：后台快照与写命令并发时，快照是"某一瞬间的一致视图"，但不会阻塞写；若需要严格时点备份，应加写时复制。
6. **仅 IPv4**：与基线保持一致（`AF_INET`），未做双栈。
7. **未实现 `EXPIRE` 的 `NX/XX/GT/LT` 选项**、`SCAN`/`TYPE`-aware `KEYS`、`RESP3`。
8. **Windows 上的 `fsync`**：只有 POSIX 分支调用 `fsync`；Windows 依赖 `ofstream::flush()` + 回读校验（能发现写入不完整，但不如 fsync 严格）。

---

## 六、目录结构

```
v3-improved-scalable/
├── CMakeLists.txt                  # cmake_minimum_required 3.16（基线要求 4.3.3）
├── README.md
├── IMPROVEMENTS.md                 # 本文档
├── inc/
│   ├── NetCompat.h                 # RAII socket + 带超时等待 + 轮询
│   ├── Resp.h                      # 增量解析（读游标 + 惰性压缩）
│   ├── Glob.h                      # 线性 glob 匹配
│   ├── Storage.h                   # 单一键空间 + 时间轮 + 统计 + 热配置
│   ├── Commands.h                  # 命令表（INFO/CONFIG/COMMAND/INCR/APPEND/...）
│   ├── Client.h                    # 连接状态 + 批量回写阈值
│   ├── ThreadPool.h                # 固定线程池 + 有界队列
│   ├── Server.h                    # accept 循环 + 后台快照 + 优雅停机
│   └── Selftest.h
├── src/
│   ├── NetCompat.cpp  Resp.cpp  Glob.cpp
│   ├── Storage.cpp    Snapshot.cpp  StoreFile.cpp     # 数据 / 落盘 / 快照辅助
│   ├── Commands.cpp   Client.cpp    ThreadPool.cpp
│   ├── Server.cpp     main.cpp      Selftest.cpp
├── client/main.cpp
└── tests/run_tests.ps1             # 60 项集成测试
```
