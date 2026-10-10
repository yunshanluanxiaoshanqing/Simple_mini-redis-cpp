# mini-redis-cpp

C++ 实现的教学用 Redis 服务器，同一仓库收录**三个阶段的版本**，按演进顺序
编号排列，便于对照学习：

| 目录 | 版本 | 一句话说明 | 成果归属 |
| --- | --- | --- | --- |
| [`01-original/`](01-original/) | 原始项目 | 最初编写的源码，低级错误已修正，保持原始语义 | **作者原作**（作者自行修正大部分低级错误，MiMo 补修 4 处，见其 README） |
| [`02-fixed/`](02-fixed/) | 修复版 | 在原始代码上完成语义/文案/持久化的系统性修复（POSIX） | MiMo（AI 助手） |
| [`03-improved/`](03-improved/) | 改进版 | **在 02-fixed 之上的全面改进**：跨平台、协议健壮性、完整测试 | MiMo（AI 助手） |

每个子项目带有独立文档：`README.md`（使用说明），`02-fixed` 另有
`CHANGES.md`（逐条修改清单）。所有配套**客户端**（`02-fixed/client/`、
`03-improved/client/`）亦为 MiMo 编写。

## 版本关系

**03-improved 是 02-fixed 的改进版，02-fixed 是 01-original 的修复版**，
构成一条完整的演进链：

```
01-original（原始 + 低级错误修正）
    └─→ 02-fixed（语义/文案/持久化修复，功能正确）
            └─→ 03-improved（网络模型、协议健壮性、跨平台、测试体系）
```

- `01-original` 的目标是让最初的代码**能编译、能运行**，语义保持原样；
- `02-fixed` 修复数据语义与协议细节（错误文案、返回值、持久化格式等），
  让行为对齐 Redis，但保留原始网络模型与仅 POSIX 的限制；
- `03-improved` 在相同命令集与修复成果之上做系统性改进，可实际使用与跨平台部署。

三者支持相同的命令集（`PING ECHO SET GET DEL TYPE KEYS FLUSHALL EXPIRE
RENAME LPUSH RPUSH LPOP RPOP LLEN LGET LINDEX LSET LREM HSET HGET HEXISTS
HDEL HGETALL HKEYS HVALS HLEN HMSET`），行为约定一致，可互相参照阅读。

## 三版本对比

下表为**项目能力层面**的差异（不含源码对照）：

| 维度 | 01-original（原始） | 02-fixed（修复版） | 03-improved（改进版） |
| --- | --- | --- | --- |
| 定位 | 能编译、能运行 | 功能正确、行为对齐 Redis | 可实际使用、跨平台部署 |
| 平台支持 | 仅 POSIX | 仅 POSIX | Linux / WSL / macOS + **Windows**（Winsock 兼容层，MinGW 静态链接） |
| 网络读写模型 | 单次读取当作一条命令；约 1KB 截断 | 同原始模型（保留） | 接收缓冲状态机：**粘包、半包、流水线、大报文**，循环发送应对短写 |
| 协议容错 | 畸形输入不崩溃（补修后），回复为简化格式 | 同左 | 标准 `-ERR Protocol error` 回复，校验协议帧完整性 |
| 数据语义 | 原始行为（TTL 清理已修） | **已对齐 Redis**：WRONGTYPE 拦截、空集合自动删 key、RENAME 覆盖、`HSET/EXPIRE` 返回 `:1/:0` | 与修复版一致，另含更严格的边界处理 |
| 持久化 | 按空白分隔（值含空格/换行丢失），TTL 不持久化 | 长度前缀二进制安全格式 + **剩余 TTL 重启恢复** | 同左，周期间隔可用环境变量配置 |
| 错误文案 | 原始写法（含拼写问题） | Redis 标准格式 | Redis 标准格式 |
| KEYS 通配符 | 不支持（忽略参数） | 支持 `*` `?`，排序输出 | 支持 `*` `?`，排序输出 |
| 客户端 | 无（可用 nc 或后两版客户端连接） | 单命令 + REPL | 单命令 + REPL + **stdin 流水线** + 引号分词 |
| 构建 | CMake（POSIX） | CMake（POSIX） | CMake（POSIX + Windows/MinGW，`-Wall -Wextra` 零警告） |
| 测试 | 冒烟 77 项（WSL，nc 协议） | 109 项（WSL） | 132 项（WSL）+ 126 项（Windows × win32/posix 两种 MinGW） |

02-fixed 相对 01-original 的逐条修复、03-improved 相对 02-fixed 的能力增强，
分别见 [`02-fixed/CHANGES.md`](02-fixed/CHANGES.md) 与本表。

## 快速开始

### 01-original（POSIX）

```bash
cd 01-original
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
bash tests/run_tests.sh            # 冒烟测试（77 项）
./build/redis-server 6379
```

### 02-fixed（POSIX）

```bash
cd 02-fixed
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
bash tests/run_tests.sh            # 测试（109 项）
./build/redis-server 6379          # 服务端
./build/redis-cli -p 6379 PING     # 客户端
```

### 03-improved（POSIX / Windows）

```bash
# Linux / WSL
cd 03-improved
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
bash tests/run_tests.sh            # 测试（132 项）
```

```powershell
# Windows（MinGW-w64，win32 或 posix 线程模型均可）
cd 03-improved
cmake -S . -B build-win -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build-win -j
pwsh -File tests/run_tests.ps1     # 测试（126 项，需 PowerShell 7+）
```

## 测试状态

| 测试 | 01-original | 02-fixed | 03-improved |
| --- | --- | --- | --- |
| WSL（g++，POSIX） | 77/77 通过 | 109/109 通过 | 132/132 通过 |
| Windows + MinGW（win32-seh） | 不适用 | 不适用 | 126/126 通过 |
| Windows + MinGW（posix-seh） | 不适用 | 不适用 | 126/126 通过 |
