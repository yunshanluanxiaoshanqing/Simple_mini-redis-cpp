# v1-baseline-hardened

`mini-redis-cpp/01-original` 的改进版：保持最小可读规模，把原始设计里会造成
**数据损坏、崩溃、无法优雅退出**的结构性问题修掉。完整的"缺点 → 方向 → 措施"
清单见 [IMPROVEMENTS.md](IMPROVEMENTS.md)。

## 构建

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Linux / macOS：`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j`

## 测试

```powershell
.\build\v1-server.exe --selftest      # 104 项进程内语义自检（不依赖网络）
pwsh -File tests/run_tests.ps1        # 60 项网络集成测试
ctest --test-dir build --output-on-failure
```

## 运行

```powershell
.\build\v1-server.exe --port 6379 --dump dump.my_rdb
.\build\v1-cli.exe -p 6379 PING
.\build\v1-cli.exe -p 6379 SET hello "hello world"
.\build\v1-cli.exe -p 6379                     # 交互式 REPL
"SET a 1`nGET a" | .\build\v1-cli.exe -p 6379 --stdin   # 流水线
```

停止：按 `Ctrl+C`，或 `v1-cli -p 6379 SHUTDOWN`。两种方式都会**先落盘再退出**，
进程退出码为 0。

### 服务器参数

| 参数 | 说明 |
|---|---|
| `--port N` / `-p N` | 监听端口（1..65535，默认 6379）；非法值退出码 1 |
| `--dump FILE` / `-d` | 持久化文件路径（默认 `dump.my_rdb`） |
| `--selftest` | 跑内置自检后退出 |
| `-h`, `--help` | 帮助 |

### 客户端参数

| 参数 | 说明 |
|---|---|
| `-h host` / `-p port` | 目标地址（默认 127.0.0.1:6379） |
| `-c` | 显式声明"后面是单条命令"（便于 `-c PING` 这种无参数命令） |
| `--stdin` | 从标准输入逐行读命令，一次性流水线发送后按序打印响应 |
| `--raw` | 输出原始 RESP 字节（脚本/十六进制比对用） |
| 不带参数 | 进入交互式 REPL |

退出码：`0` 成功、`1` 用法错误、`2` 连接失败、`3` 协议/连接中断。

## 命令集

```
PING ECHO SET GET DEL UNLINK EXISTS TYPE KEYS FLUSHALL DBSIZE
EXPIRE EXPIREAT TTL PTTL PERSIST RENAME
LPUSH RPUSH LPOP RPOP LLEN LGET LRANGE LINDEX LSET LREM
HSET HMSET HGET HEXISTS HDEL HGETALL HKEYS HVALS HLEN
QUIT SHUTDOWN
```

- `SET key value [EX s|PX ms] [NX|XX] [KEEPTTL]`
- `LPOP/RPOP key [count]`；`LGET` 返回整个列表（兼容原项目自定义命令），`LRANGE` 为标准命令
- `HSET` 返回新建字段数（`:1` 新建 / `:0` 更新）；`HMSET` 恒返回 `+OK`
- 类型冲突：`-WRONGTYPE Operation against a key holding the wrong kind of value`
- 参数错误：`-ERR wrong number of arguments for 'x' command`（命令名为小写）
- 协议错误：`-ERR Protocol error: ...` 并关闭连接

## 持久化格式

```
K <klen> <vlen> <ttl_ms>\n<key><value>
L <klen> <count> <ttl_ms>\n<key>(<elen>\n<elem>)*count
H <klen> <count> <ttl_ms>\n<key>(<flen> <vlen>\n<field><value>)*count
```

长度前缀 → 键值可含空格、换行、`\0`；`ttl_ms` 为剩余毫秒，重启后按剩余时间继续过期。
落盘流程为"写 `.tmp` → `fsync` → 回读校验 → `rename` 原子替换"，
中途崩溃不会破坏旧存档；加载时能区分"文件不存在"与"文件损坏"（后者不会覆盖旧文件）。

> v1 的格式没有文件级校验和；能检测"格式不对"但检测不出"字节被翻转"。
> 需要 CRC32 级校验请用 v2/v3。

## 本版本有意保留的限制

- 每连接一线程（线程数 = 连接数）；无空闲超时、无最大连接数、无内存上限
- 落盘全程持有数据库锁
- 过期清理是 O(带 TTL 的键数)
- 持久化文件无魔数/CRC
- 命令集不含 `INCR`/`APPEND`/`MSET`/`INFO`/`CONFIG`

这些分别由 [v2](../v2-fixed-robust/) 与 [v3](../v3-improved-scalable/) 解决，
对比见 [COMPARISON.md](../COMPARISON.md)。

## 目录结构

```
v1-baseline-hardened/
├── CMakeLists.txt
├── README.md  IMPROVEMENTS.md
├── inc/    NetCompat.h Resp.h Glob.h Storage.h Commands.h Client.h Server.h Selftest.h
├── src/    NetCompat.cpp Resp.cpp Glob.cpp Storage.cpp Commands.cpp Client.cpp
│           Server.cpp main.cpp Selftest.cpp
├── client/ main.cpp
└── tests/  run_tests.ps1
```
