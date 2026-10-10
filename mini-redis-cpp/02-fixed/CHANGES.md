# 02-fixed 修改说明（CHANGES.md）

本项目是**最初源码**：分两批修复了编译/链接失败、程序崩溃、
命令功能失效以及数据语义类错误；除修复外不引入新架构（无跨平台兼容层、
不重写网络读写模型）。

测试结论：WSL 下 `bash tests/run_tests.sh` **109/109 全部通过**。

## 一、基本逻辑错误修复（4 处）

| # | 原逻辑错误（现象） | 修复 |
|---|---|---|
| 14 | `del()` 计算了 `erased` 却 `return false`，**DEL/UNLINK 永远返回 0** | 改为 `return erased` |
| 15 | 命令分发 `"Lget"`（被 `toupper` 大写化后永假）和 `"LINE"`（拼写错误），**LGET、LLEN 完全失效** | 改为 `"LGET"`、`"LLEN"` |
| 16 | `parseRespCommand` 中 `stoi` 无异常捕获，畸形 RESP（如 `*abc`）**使服务器崩溃** | 两处 `stoi` 加 `try/catch` |
| 17 | `expiry_map` 三处不清理：`set()` 覆盖不清旧 TTL、`del()`/`flushAll()` 不删过期记录 → 同名 key 复用被误删 | 三处补清理 |

## 二、第二批修复（按反馈逐项）

| # | 问题 | 修复 |
|---|---|---|
| 18 | 线程 `join` 在客户端未断开时阻塞；`cmdHandler` 栈对象被线程引用 | 连接线程 `detach` 不再 join；`cmdHandler` 改为**在线程内部构造**，生命周期随线程 |
| 19 | 无 `WRONGTYPE` 类型冲突检查（对 string 执行 `LPUSH` 等被静默接受） | 全部数据命令入口先做类型检查，冲突返回 `-WRONGTYPE Operation against a key holding the wrong kind of value` |
| 20 | `KEYS` 忽略 pattern 参数，任何参数都返回全部 key | 支持 `*` 与 `?` 通配符（`*` 匹配任意序列，`?` 匹配单字符），结果排序输出 |
| 21 | `HSET` 恒返回 `+OK`；`EXPIRE` 成功返回 `+OK`、key 不存在返回 `-ERR` | 对齐 Redis：`HSET` 新建 field 返回 `:1`、更新返回 `:0`；`EXPIRE` 设置成功 `:1`、key 不存在 `:0` |
| 22 | `dump/load` 按空白分隔：**值含空格或换行会丢失**；TTL 不持久化 | 重写为长度前缀二进制安全格式（ASCII 头 + 精确长度原始字节），并新增 `E` 记录保存**剩余 TTL**，重启后按剩余时间恢复 |
| 23 | 列表弹空 / 哈希删空后 key 残留（Redis 会删除 key） | `LPOP`/`RPOP`/`LREM` 弹空列表、`HDEL` 删空哈希时自动删除 key 及其 TTL |
| 24 | `RENAME` 不清除目标 key 的其他类型残留（目标已有不同类型数据时共存） | 覆盖前先清除目标 key 的全部状态（三种类型 + TTL）；`oldkey==newkey` 直接成功返回 |
| 25 | 错误文案拼写/格式问题（见下表） | 全部统一为 Redis 标准格式 `ERR wrong number of arguments for 'x' command` 等 |

### 第 25 项：错误文案明细

| 位置 | 原文案 | 修正后 |
|---|---|---|
| HMSET | `HMSET requires key floowed by field value pairs`（拼写 `floowed`） | `wrong number of arguments for 'hmset' command` |
| ECHO | `wrong  'echo' command`（残缺+双空格） | `wrong number of arguments for 'echo' command` |
| TYPE / DEL | `-ERR TYPE ` / `-ERR DEL `（残缺） | `wrong number of arguments for 'type'/'del' command` |
| EXPIRE | `EXISTS requires key and time in seconds`、`ERR  key not found`、`ERR  invalid expiration time ` | 分别改为标准参数错误 / `:0` 返回 / `value is not an integer or out of range` |
| RENAME | `RENAME requires key and new key`、`ERR  key not found`（双空格） | `wrong number of arguments for 'rename' command`、`no such key` |
| 各命令参数不足 | `Lpush requires key and value` 等各自文案 | 统一 `wrong number of arguments for 'x' command` |
| LSET 越界 | `invalid index` | `index out of range`（Redis 文案） |
| LINDEX/LSET/LREM/EXPIRE 参数非整数 | `invalid index` / `invalid count` / `invalid expiration time` | 统一 `value is not an integer or out of range` |

## 四、非代码类规范化（不改变程序行为）

| # | 内容 |
|---|---|
| 26 | 文件更名：`src/ReadisDatabase.cpp` → `src/RedisDatabase.cpp`（原拼写错误） |
| 27 | 目录布局：服务端 `src/` + `inc/`，配套客户端独立于 `client/`（分离） |
| 28 | 新增 `CMakeLists.txt`（原项目无构建脚本） |
| 29 | 新增 `tests/run_tests.sh`（109 项）、`README.md`、`.gitignore`、`.gitattributes` |

## 五、明确未修改的部分

以下属于架构/平台层面的限制，有意保留（完整改进版见仓库中的 `03-improved`）：

- **无粘包/半包处理**：一次 `recv` 当作一条命令，多命令一次到达只处理第一条，
  超过 1023 字节的命令被截断
- 畸形 RESP 的响应为 `Err: Empty command`（不崩溃，但不是标准 RESP 错误格式）
- 端口被占用时 `bind` 失败，进程退出码仍为 0（仅打印错误）
- 仅 POSIX 平台（Linux / WSL / macOS），Windows 不可编译
- `LGET` 为自定义命令（返回整个列表，非标准 `LRANGE`）；无 `EXISTS`/`TTL`/`INCR` 等命令
- `SET` 忽略 `EX`/`PX`/`NX`/`XX` 选项参数
- `HGETALL` 字段顺序不保证（`HKEYS`/`HVALS` 已排序输出）

## 六、已知行为说明（测试所依据的实际行为）

- `HSET`：新 field → `:1`，更新 field → `:0`；`EXPIRE`：成功 → `:1`，key 不存在 → `:0`
- `KEYS pattern` 支持 `*` `?`，结果按字典序排序；无参等价于 `KEYS *`
- 类型冲突一律返回 `-WRONGTYPE Operation against a key holding the wrong kind of value`
- 列表/哈希被清空后 `TYPE` 返回 `none`（key 已删除）
- 持久化文件 `dump.my_rdb` 为长度前缀格式，值可含空格与换行；
  带 TTL 的 key 重启后按**剩余时间**继续过期
- `EXPIRE` 非正数 TTL：key 在下一次访问时被清除
