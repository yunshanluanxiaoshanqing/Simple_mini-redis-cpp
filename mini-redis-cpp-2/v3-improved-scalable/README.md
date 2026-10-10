# v3-improved-scalable

`mini-redis-cpp/03-improved` 的改进版：解决**规模化**层面的问题——
线程数与连接数解耦、落盘不长时间阻塞服务、过期清理不随 TTL 键数增长、
配置可热改、运行状态可观测。完整的"缺点 → 方向 → 措施"清单见
[IMPROVEMENTS.md](IMPROVEMENTS.md)。

## 构建

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

> `cmake_minimum_required` 为 **3.16**（基线 `03-improved` 要求 4.3.3，会导致旧环境直接配置失败）。

## 测试

```powershell
.\build\v3-server.exe --selftest      # 151 项（含时间轮 / 并发 INCR / snapshots / CONFIG）
pwsh -File tests/run_tests.ps1        # 60 项网络集成测试
ctest --test-dir build --output-on-failure
```

## 运行

```powershell
# 4 个固定工作线程，最多 64 个连接，10 分钟空闲超时，后台每 60 秒快照一次
.\build\v3-server.exe --port 6379 --dump dump.my_rdb --threads 4 --maxclients 64 `
                      --timeout 600000 --dump-interval 60000

.\build\v3-cli.exe -p 6379 SET hello "hello world"
.\build\v3-cli.exe -p 6379 INCR counter
.\build\v3-cli.exe -p 6379 CONFIG SET maxmemory 104857600
.\build\v3-cli.exe -p 6379 INFO
```

### 服务器参数

| 参数 | 说明 |
|---|---|
| `--port N` / `-p N` | 监听端口（1..65535，默认 6379） |
| `--dump FILE` / `-d` | 持久化文件路径（默认 `dump.my_rdb`） |
| `--threads N` | 工作线程数（1..64；默认 = CPU 核数，夹在 2..64） |
| `--maxclients N` | 最大连接数 = 线程池队列上界（1..100000，默认 1024） |
| `--timeout MS` | 空闲连接超时（0..86400000，默认 300000，`0` = 不限制） |
| `--dump-interval MS` | 后台自动快照间隔（默认 300000，`0` = 关闭后台快照） |
| `--selftest` | 跑内置自检后退出 |
| `-h`, `--help` | 帮助 |

### 环境变量

| 变量 | 说明 |
|---|---|
| `MINI_REDIS_MAXMEMORY` | 启动时的内存上限（字节）；运行期可用 `CONFIG SET maxmemory` 修改 |

### 客户端参数

与 [v1](../v1-baseline-hardened/README.md#客户端参数) 相同（`-c` / `--stdin` / `--raw` / REPL）。

## 本版本相对 v2 的增量

| 能力 | 说明 | 实测效果 |
|---|---|---|
| **固定线程池 + 有界队列** | `--threads` 决定线程数，与连接数无关；队列 = `maxclients` | 50 个空闲连接下线程数 **8 → 8**（v1/v2 为 3 → 53） |
| **最大连接数** | 超限回 `-ERR max number of clients reached` 再关闭，并计入 `rejected_connections` | 第 5 个连接（`--maxclients 4`）收到明确错误 |
| **快照式落盘** | 锁内只构造快照字节流，写盘/校验/替换在锁外 | 落盘不再长时间阻塞客户端 |
| **落盘重叠保护** | 后台快照与 `SHUTDOWN`/`SAVE` 并发时，后到者得到 `Busy` 并跳过 | 不会排队做第二次全量落盘 |
| **过期时间轮** | 按秒分桶，只处理已到期的桶，均摊 O(1) | 清理代价不再随 TTL 键数增长 |
| **批量回写** | 一次 `recv` 内的流水线响应累积到 256KB 再写出 | 减少系统调用 |
| **零搬移协议解析** | 读游标 + 惰性压缩，替代 `erase(0,n)` | 长连接累计代价 O(n²) → 均摊 O(1) |
| **运行期配置** | `CONFIG GET\|SET` 热改 maxmemory / timeout / maxclients / dump-interval | 无需重启 |
| **可观测性** | `INFO` 输出 hits / misses / expired_keys / 命令数 / 连接数 / 拒绝数 / 快照数 / 上次落盘是否成功 | 压测与排障有据可依 |
| **更多命令** | `APPEND INCR DECR INCRBY DECRBY SETNX GETSET MGET MSET COMMAND` | `INCR` 含溢出保护；并发 `INCR` 实测不丢更新 |
| **后台自动落盘** | `--dump-interval` 驱动的快照线程，退出时可被唤醒 | 停机顺序：停接入 → join 快照线程 → 停线程池 → 最终落盘 |

## 命令集

v2 的全部命令，外加：

```
APPEND key value
INCR key | DECR key | INCRBY key delta | DECRBY key delta
SETNX key value | GETSET key value
MGET key [key ...] | MSET key value [key value ...]
COMMAND                     # 返回支持的命令名列表（已排序）
CONFIG GET <param> | CONFIG SET <param> <value>
```

`CONFIG` 支持的参数：`maxmemory`、`timeout`、`maxclients`、`dump-interval`（`CONFIG GET *` 返回全部）。

`INCR`/`DECR` 系列对非整数值返回 `-ERR value is not an integer or out of range`，
对会溢出的自增返回 `-ERR increment or decrement would overflow`（不触发有符号溢出 UB）。

## 持久化

与 [v2](../v2-fixed-robust/) **完全互通**（`MINIRDB2` + 版本 + 记录区 + CRC32），
区别只在写入流程：v3 在锁内只做快照拷贝，写盘与校验都在锁外，并带重叠保护。
`load()` 之后会重建过期时间轮，保证 TTL 语义延续。

## 本版本仍然存在的限制

- 单机单进程（无复制/集群）
- 快照期间内存峰值约为数据量的 2 倍（无 AOF/增量持久化）
- `KEYS` 仍是 O(全部键)（无游标式 `SCAN`）
- 全局单锁（无分片）
- 仅 IPv4；未实现 RESP3 / `MULTI-EXEC` / 发布订阅 / `EXPIRE` 的 `NX|XX|GT|LT`
- Windows 侧不调用 `fsync`（依赖 flush + 回读校验）

对比与选型建议见 [COMPARISON.md](../COMPARISON.md)。

## 目录结构

```
v3-improved-scalable/
├── CMakeLists.txt
├── README.md  IMPROVEMENTS.md
├── inc/    NetCompat.h Resp.h Glob.h Storage.h Commands.h Client.h
│           ThreadPool.h Server.h Selftest.h
├── src/    NetCompat.cpp Resp.cpp Glob.cpp
│           Storage.cpp Snapshot.cpp StoreFile.cpp
│           Commands.cpp Client.cpp ThreadPool.cpp
│           Server.cpp main.cpp Selftest.cpp
├── client/ main.cpp
└── tests/  run_tests.ps1
```
