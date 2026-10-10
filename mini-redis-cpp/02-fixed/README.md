# 02-fixed — C++ Redis 服务器（原始源码修复版）

> 本版本的全部修复、配套客户端与测试由 **MiMo** 完成。

本项目是最初的 C++ Redis 服务器源码，经两批修复后达到功能可用状态：
第一批修复编译/链接失败、崩溃与命令失效（17 处），第二批按反馈修复线程
模型、类型冲突、KEYS 通配符、Redis 标准返回值、持久化与数据语义问题
（8 类）。完整逐条清单见 [CHANGES.md](CHANGES.md)。

配套简易客户端（`client/`，由 MiMo 编写），服务端与客户端目录分离。
本仓库中的 `03-improved` 是本项目的**改进版**（跨平台 + 协议健壮性 + 全面测试），
两者对比见仓库根目录 `README.md`。

## 目录结构

```
.
├── inc/                     # 服务器头文件
│   ├── RedisServer.h
│   ├── RedisCommandHandler.h
│   └── RedisDatabase.h
├── src/                     # 服务器源码
│   ├── main.cpp
│   ├── RedisServer.cpp
│   ├── RedisCommandHandler.cpp
│   └── RedisDatabase.cpp    # 原文件名 ReadisDatabase.cpp（已更名）
├── client/
│   └── main.cpp             # 配套简易客户端（POSIX）
├── tests/
│   └── run_tests.sh         # 功能测试（109 项）
├── CMakeLists.txt
├── CHANGES.md               # 修改清单（修了什么、为什么、没修什么）
└── README.md
```

## 构建（Linux / WSL / macOS，POSIX）

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
# 产物: build/redis-server, build/redis-cli
```

> 仅支持 POSIX 平台。Windows 请使用改进版 `03-improved`。

## 运行

```bash
# 服务器（默认端口 6379，可指定）
./build/redis-server 6379

# 客户端：单命令
./build/redis-cli -p 6379 SET foo bar
./build/redis-cli -p 6379 GET foo

# 客户端：交互式 REPL（不带参数）
./build/redis-cli -p 6379
```

- 启动时自动加载工作目录下的 `dump.my_rdb`（若存在）。
- `Ctrl+C` 退出前 dump 数据库；另每 300 秒周期性 dump。
- 每个客户端连接由独立线程处理（线程 detach，不阻塞关闭流程）。

## 支持的命令

| 类别 | 命令 |
| --- | --- |
| 通用 | `PING` `ECHO` `FLUSHALL` `TYPE` `KEYS`（支持 `*` `?` 通配符） |
| 字符串 | `SET` `GET` `DEL`/`UNLINK` `EXPIRE` `RENAME` |
| 列表 | `LPUSH` `RPUSH` `LPOP` `RPOP` `LLEN` `LGET` `LINDEX` `LSET` `LREM` |
| 哈希 | `HSET` `HGET` `HEXISTS` `HDEL` `HGETALL` `HKEYS` `HVALS` `HLEN` `HMSET` |

行为对齐 Redis 的要点：

- `HSET` 返回 `:1`（新建 field）/ `:0`（更新）；`EXPIRE` 返回 `:1` / `:0`
- 类型冲突返回 `-WRONGTYPE ...`（如对 string 执行 `LPUSH`）
- 列表弹空 / 哈希删空后 key 自动删除；`RENAME` 覆盖目标 key 全部状态
- `SET` 覆盖时清除旧 TTL；错误文案为 Redis 标准格式
- 持久化为长度前缀二进制安全格式：值可含空格与换行，**TTL 重启后按剩余时间恢复**

## 测试

```bash
bash tests/run_tests.sh
```

当前 **109/109 全部通过**（WSL，g++）。覆盖：基础命令、字符串、TTL、
KEYS 通配符、RENAME（含覆盖）、列表、哈希、WRONGTYPE、RESP/内联协议、
畸形输入抗崩溃、客户端、并发、持久化（含空格/换行值与 TTL 恢复）。

## 已知限制

有意保留的架构/平台限制（详见 CHANGES.md 第五节）：

- 无粘包/半包处理：一次 `recv` 当作一条命令，>1023 字节被截断
- 端口占用时退出码仍为 0；畸形 RESP 回复 `Err: Empty command`（非标准格式）
- 仅 POSIX；`LGET` 为自定义命令（非 `LRANGE`）；`SET` 忽略 `EX/PX/NX/XX` 选项

需要跨平台与协议健壮性请使用改进版 `03-improved`。
