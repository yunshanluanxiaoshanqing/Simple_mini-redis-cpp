# v2-fixed-robust

`mini-redis-cpp/02-fixed` 的改进版：在"语义已对齐 Redis"的基础上，把
**数据完整性**与**资源安全**做成可测试的工程属性。完整的"缺点 → 方向 → 措施"
清单见 [IMPROVEMENTS.md](IMPROVEMENTS.md)。

## 构建

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## 测试

```powershell
.\build\v2-server.exe --selftest      # 124 项（含 CRC 损坏 / 内存上限 / 旧格式兼容）
pwsh -File tests/run_tests.ps1        # 72 项网络集成测试（含存档翻转 1 bit 后重启）
ctest --test-dir build --output-on-failure
```

## 运行

```powershell
.\build\v2-server.exe --port 6379 --dump dump.my_rdb --timeout 300000
.\build\v2-cli.exe -p 6379 SET hello "hello world"
.\build\v2-cli.exe -p 6379 INFO
.\build\v2-cli.exe -p 6379 MAXMEMORY 104857600    # 限制为 100MB
```

### 服务器参数

| 参数 | 说明 |
|---|---|
| `--port N` / `-p N` | 监听端口（1..65535，默认 6379） |
| `--dump FILE` / `-d` | 持久化文件路径（默认 `dump.my_rdb`） |
| `--timeout MS` | 空闲连接超时毫秒（默认 300000，`0` = 不限制） |
| `--selftest` | 跑内置自检后退出 |
| `-h`, `--help` | 帮助 |

### 环境变量

| 变量 | 说明 |
|---|---|
| `MINI_REDIS_MAXMEMORY` | 单库内存上限（字节）；超过后写命令返回 `-OOM` |

### 客户端参数

与 [v1](../v1-baseline-hardened/README.md#客户端参数) 相同（`-c` / `--stdin` / `--raw` / REPL）。

## 本版本相对 v1 的增量

| 能力 | 说明 |
|---|---|
| **存档 CRC32** | 文件头 `MINIRDB2` + 版本号，尾部 4 字节 CRC32；翻转 1 bit 即判损坏，绝不装载 |
| **旧格式兼容** | 能读 v1 的无魔数格式并报告 `LegacyFormat`，退出时改写为新格式 |
| **落盘回读校验** | 写 `.tmp` 后重新读回并与内存逐字节比对，再原子替换 |
| **内存上限** | `MAXMEMORY` 命令 / 环境变量；超限返回 `-OOM command not allowed ...`，覆盖写会先扣除被替换的旧值 |
| **空闲超时** | `--timeout`；超时打印 `closing idle connection after N ms` 并关闭 |
| **`INFO`** | 输出 `version` / `dump_format` / `keys` / `used_memory` / `maxmemory` |
| **更好的错误信息** | 未知命令带命令名、`wrong number of arguments` 命令名小写 |

## 命令集

v1 的全部命令，外加：

```
INFO
MAXMEMORY [bytes]        # 无参 = 查询，给定值 = 设置（0 = 解除限制）
```

## 持久化格式

```
[0..7]   魔数 "MINIRDB2"
[8]      版本号 2
[9..]    记录区（与 v1 相同的 K/L/H 记录）
[末尾]   CRC32（小端 4 字节，覆盖记录区）
```

- 与 [v3](../v3-improved-scalable/) 的存档**完全互通**。
- 写入流程：`.tmp` → `fsync` → 回读逐字节校验 → `rename` 原子替换。
- 校验失败（魔数/版本/CRC 任一不符）→ `LoadResult::Corrupt`，**不装载**，
  且旧文件在下次成功落盘前不会被改写。

## 本版本有意保留的限制

- 每连接一线程；无最大连接数限制
- 落盘全程持有数据库锁
- 过期清理是 O(带 TTL 的键数)
- 无后台自动落盘、无 `CONFIG` 命令
- 命令集不含 `INCR`/`APPEND`/`MSET`

这些由 [v3](../v3-improved-scalable/) 解决，对比见 [COMPARISON.md](../COMPARISON.md)。

## 目录结构

```
v2-fixed-robust/
├── CMakeLists.txt
├── README.md  IMPROVEMENTS.md
├── inc/    NetCompat.h Resp.h Glob.h Storage.h Commands.h Client.h Server.h Selftest.h
├── src/    NetCompat.cpp Resp.cpp Glob.cpp Storage.cpp Commands.cpp Client.cpp
│           Server.cpp main.cpp Selftest.cpp
├── client/ main.cpp
└── tests/  run_tests.ps1
```
