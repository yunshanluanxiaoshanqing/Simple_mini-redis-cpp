# 01-original — 原始项目（低级错误已修正）

这里是你最初编写的 C++ Redis 服务器**原始项目**。编译失败、崩溃、命令失效
一类的**低级错误已修正**，项目可正常构建与运行；语义行为与网络模型保持原始
设计（未做增强），适合与 [`02-fixed`](../02-fixed/)、[`03-improved`](../03-improved/)
对照，观察三个阶段的演进。

## 修正记录
**MiMo补修的 4 处低级错误**：

| # | 位置 | 问题 | 修复 |
|---|---|---|---|
| 1 | `src/RedisServer.cpp` | `sockaddr_in serverAddr();` 最烦人解析，`serverAddr` 实为未声明标识符（编译失败） | 改为 `sockaddr_in serverAddr{};` |
| 2 | `src/RedisCommandHandler.cpp` | 命令分发写成 `"LLNE"`（拼写错误，`LLEN` 失效） | 改为 `"LLEN"` |
| 3 | `src/RedisCommandHandler.cpp` | `parseRespCommand` 中 `stoi` 无异常捕获，畸形 RESP 输入（如 `*abc`）会使服务器崩溃 | 两处加 `try/catch`，返回空 token |
| 4 | `src/main.cpp` | 端口参数 `stoi` 无校验，非数字参数直接崩溃 | 加 `try/catch`，打印用法并退出 1 |

## 目录结构

```
01-original/
├── dump.my_rdb          # 原始的空持久化文件
├── inc/
│   ├── RedisCommandHandler.h
│   ├── RedisDatabase.h
│   └── RedisServer.h
├── src/
│   ├── main.cpp
│   ├── RedisDatabase.cpp   
│   ├── RedisCommandHandler.cpp
│   └── RedisServer.cpp
├── tests/run_tests.sh       # 冒烟测试（77 项，基于 nc 原始协议）
├── CMakeLists.txt
└── README.md
```

## 构建与测试（Linux / WSL / macOS，POSIX）

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
bash tests/run_tests.sh        # 77/77 通过
./build/redis-server 6379      
                               # 02-fixed / 03-improved 的 redis-cli 连接）
```

## 保留的原始特征（与后两版的差异）

- `dump/load` 按空白分隔：值含空格或换行会丢失；TTL 不持久化
- `EXPIRE` 成功返回 `+OK`（非 `:1`）、`HSET` 返回 `+OK`（非 `:1/:0`）
- 无 `WRONGTYPE` 类型冲突检查；`KEYS` 不支持通配符 pattern
- 错误文案为原始写法（如 `floowed` 拼写）
- 线程 `join` 模型、无粘包/半包处理（一次 `recv` 一条命令）
- 仅 Linux 平台
