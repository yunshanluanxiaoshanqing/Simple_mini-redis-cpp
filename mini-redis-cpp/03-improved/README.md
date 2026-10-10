# mini-redis（C++ 实现的 Redis 服务器 + 客户端）

> 本版本的改进、配套客户端与测试由 **MiMo** 完成。
>
> 本项目是同仓库 [`02-fixed`](../02-fixed/)（原始源码修复版）的**改进版**：
> 在相同命令集与修复成果之上，增加了跨平台支持、协议健壮性与全面测试。
> 两版本的差异见[仓库根 README](../README.md)。

一个用 C++17 编写的轻量 Redis 兼容服务器，附带配套命令行客户端 `redis-cli`。
支持 RESP 与内联（inline）协议、多线程并发连接，以及自定义的持久化格式。

- 服务器：`redis-server`
- 客户端：`redis-cli`（单命令 / `--stdin` 流水线 / 交互式 REPL 三种模式）
- 跨平台：Linux / WSL（g++ + CMake）、Windows（MinGW-w64 g++ + CMake）

## 目录结构

```
.
├── CMakeLists.txt          # 跨平台构建脚本
├── inc/                    # 头文件
│   ├── NetCompat.h         # POSIX/Windows 网络兼容层
│   ├── RedisServer.h
│   ├── RedisCommandHandler.h
│   └── RedisDatabase.h
├── src/                    # 服务器源码
│   ├── main.cpp
│   ├── RedisServer.cpp
│   ├── RedisCommandHandler.cpp
│   └── RedisDatabase.cpp
├── client/
│   └── main.cpp            # redis-cli 客户端
├── tests/
│   ├── run_tests.sh        # Linux/WSL 全面测试（132 项）
│   └── run_tests.ps1       # Windows 全面测试（126 项，需 PowerShell 7+）
├── .clang-format           # 代码风格配置
├── .gitattributes
└── .gitignore
```

## 构建

### Linux / WSL / macOS

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
# 产物: build/redis-server, build/redis-cli
```

### Windows（MinGW-w64）

```powershell
# win32 或 posix 线程模型的 mingw64 均可（本仓库在两种模型下均测试通过）
cmake -S . -B build-win -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build-win -j
# 产物: build-win\redis-server.exe, build-win\redis-cli.exe
```

MinGW 下自动采用**静态链接**（`-static`），exe 不依赖 `libstdc++-6.dll` /
`libwinpthread-1.dll` 等运行时 DLL，避免与 PATH 中其他发行版的 DLL 混载
导致崩溃，拷贝单文件即可运行。

跨平台实现见 `inc/NetCompat.h`：POSIX 使用 BSD socket，Windows 映射为
Winsock2（`WSAStartup` / `closesocket` / `SO_EXCLUSIVEADDRUSE`）。

编译选项开启 `-Wall -Wextra`，当前零警告。

## 运行

```bash
# 启动服务器（默认端口 6379，可指定端口）
./build/redis-server 6379

# 客户端：单命令
./build/redis-cli -h 127.0.0.1 -p 6379 SET foo bar
./build/redis-cli -p 6379 GET foo

# 客户端：stdin 流水线（先全部发送，再统一读取响应）
printf 'SET a 1\nGET a\nDEL a\n' | ./build/redis-cli -p 6379 --stdin

# 客户端：交互式 REPL（不带参数启动）
./build/redis-cli -p 6379
```

Windows 下把 `./build/redis-server` 换成 `build-win\redis-server.exe` 即可。

### 服务器行为

- 每个客户端连接由独立线程处理，读写循环带接收缓冲区，正确处理**粘包 / 半包 / 流水线**。
- `Ctrl+C`（SIGINT）优雅退出：dump 数据到 `dump.my_rdb` 后退出，退出码 0。
- 启动时若工作目录存在 `dump.my_rdb` 自动加载。
- 周期持久化默认 300 秒，可通过环境变量 `REDIS_PERSIST_INTERVAL`（秒）调整。
- 端口被占用时打印错误并以退出码 1 结束。

## 支持的命令

| 类别 | 命令 |
| --- | --- |
| 通用 | `PING` `ECHO` `FLUSHALL` `TYPE` `KEYS`（支持 `*` `?` 通配符） |
| 字符串 | `SET` `GET` `DEL`/`UNLINK` `EXPIRE` `RENAME` |
| 列表 | `LPUSH` `RPUSH` `LPOP` `RPOP` `LLEN` `LGET` `LINDEX` `LSET` `LREM` |
| 哈希 | `HSET` `HGET` `HEXISTS` `HDEL` `HGETALL` `HKEYS` `HVALS` `HLEN` `HMSET` |

行为对齐官方 Redis 的要点：

- 命令名大小写不敏感；错误响应为 RESP 错误（`-ERR ...` / `-WRONGTYPE ...`）。
- 类型冲突返回 `WRONGTYPE` 错误（如对 string 执行 `LPUSH`）。
- `SET` 覆盖已有 key 时清除其 TTL；`EXPIRE` 非正数立即删除 key。
- 列表被弹空 / 哈希字段被删空后，key 自动删除（与 Redis 一致）。
- `HSET` 返回 `:1`（新建 field）/ `:0`（更新 field）；`EXPIRE` 返回 `:1`/`:0`。
- `RENAME` 覆盖目标 key（含类型不同的目标）。

### 与官方 Redis 的差异（已知限制）

- `LGET` 取代 `LRANGE`，返回整个列表，不支持 `start/stop` 参数。
- 未实现：`EXISTS`、`TTL`、`AUTH`、`SELECT`、`INCR` 等、集合/有序集合、事务、发布订阅、主从复制。
- `SET` 忽略 `EX`/`PX`/`NX`/`XX` 等选项参数。
- `KEYS` 通配符支持 `*` 与 `?`，不支持 `[abc]` 字符类。
- 持久化文件 `dump.my_rdb` 为自定义格式，**不兼容**官方 RDB。格式：
  每条记录一行 ASCII 头（类型 + 长度）后接精确长度的原始字节，键值可包含
  空格与换行（二进制安全），并记录剩余 TTL，重启后按剩余时间恢复。

## 测试

测试覆盖：编译、连接、字符串、KEYS 通配符、TTL、RENAME、列表、哈希、
WRONGTYPE、协议健壮性（粘包/半包/畸形 RESP/二进制值/100KB 大 value）、
流水线、并发、持久化（dump + 重启）、端口冲突与进程行为。

```bash
# Linux / WSL（需要 nc、python3、cmake）
bash tests/run_tests.sh
```

```powershell
# Windows（需要 PowerShell 7+ 与 MinGW 工具链）
pwsh -File tests/run_tests.ps1                 # 默认 build-win（win32 线程模型）
pwsh -File tests/run_tests.ps1 -BuildDir build-posix   # posix 线程模型构建
```

两端当前均为 **全部通过**（WSL 132 项 / Windows 126 项 × win32 与 posix
两种 MinGW 线程模型，退出码 0）。

## 代码风格

统一 4 空格缩进、行宽 100、附 `.clang-format` 配置，可随时执行：

```bash
clang-format -i inc/*.h src/*.cpp client/*.cpp
```
