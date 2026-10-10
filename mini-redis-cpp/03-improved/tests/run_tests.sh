#!/usr/bin/env bash
# 服务器 + 客户端全面测试（WSL / Linux / macOS）
# 覆盖：编译、连接、字符串、KEYS 通配符、TTL、RENAME、List、Hash、
#       WRONGTYPE、协议健壮性（粘包/半包/畸形/二进制/大value）、
#       流水线、并发、持久化（SIGINT + 重启）、进程与客户端行为。
#
# 用法: bash tests/run_tests.sh
# 环境变量: TEST_PORT (默认 6399), BUILD_DIR (默认 <repo>/build)

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}"
PORT="${TEST_PORT:-6399}"
HOST=127.0.0.1
CLI="$BUILD/redis-cli"
SRV="$BUILD/redis-server"

PASS=0
FAIL=0
FAILED=()

check() {  # check <name> <expected> <actual>
    local name="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then
        PASS=$((PASS + 1))
        printf 'PASS  %s\n' "$name"
    else
        FAIL=$((FAIL + 1))
        FAILED+=("$name")
        printf 'FAIL  %s\n      expected: [%s]\n      actual:   [%s]\n' \
            "$name" "$expected" "$actual"
    fi
}

check_bytes() {  # check_bytes <name> <printf-buf-fmt> <outfile>
    local name="$1" fmt="$2" outfile="$3"
    local expf="$WORK/exp.bin"
    printf '%b' "$fmt" >"$expf"
    if cmp -s "$expf" "$outfile"; then
        PASS=$((PASS + 1))
        printf 'PASS  %s\n' "$name"
    else
        FAIL=$((FAIL + 1))
        FAILED+=("$name")
        printf 'FAIL  %s\n' "$name"
        printf '      expected: %s\n' "$(xxd -p "$expf" | tr -d '\n' | head -c 200)"
        printf '      actual:   %s\n' "$(xxd -p "$outfile" | tr -d '\n' | head -c 200)"
    fi
}

cli() { "$CLI" -p "$PORT" "$@"; }

raw_out() {  # raw_out <printf-buf-fmt> <outfile>  通过 nc 发送原始字节
    printf '%b' "$1" | nc -q1 -w3 "$HOST" "$PORT" >"$2" 2>/dev/null
}

start_server() {
    (cd "$WORK" && exec "$SRV" "$PORT" >>"$SRV_LOG" 2>&1) &
    SRV_PID=$!
    local i
    for i in $(seq 1 50); do
        if "$CLI" -p "$PORT" PING >/dev/null 2>&1; then return 0; fi
        sleep 0.1
    done
    echo "FATAL: server failed to start" >&2
    cat "$SRV_LOG" >&2
    exit 1
}

stop_server() {  # SIGINT，触发优雅关闭 + dump；返回等待状态
    kill -INT "$SRV_PID" 2>/dev/null
    wait "$SRV_PID"
    return $?
}

# ---------------- 构建 ----------------
echo "===== [0] build ====="
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null || exit 1
if ! cmake --build "$BUILD" -j; then
    echo "FATAL: build failed" >&2
    exit 1
fi
echo "build OK"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
SRV_LOG="$WORK/server.log"
WORK2="$(mktemp -d)"
trap 'rm -rf "$WORK" "$WORK2"' EXIT

start_server

# ---------------- 1. 连接与基础命令 ----------------
echo "===== [1] basic ====="
check "PING"                       "PONG" "$(cli PING)"
check "ECHO arg"                   "hello" "$(cli ECHO hello)"
check "ECHO no-arg"                "(error) ERR wrong number of arguments for 'echo' command" "$(cli ECHO)"
check "unknown command"            "(error) ERR unknown command" "$(cli NOTACOMMAND)"
check "case-insensitive (ping)"    "PONG" "$(cli ping)"

# ---------------- 2. 字符串 ----------------
echo "===== [2] string ====="
check "SET"                        "OK" "$(cli SET k1 v1)"
check "GET"                        "v1" "$(cli GET k1)"
check "GET missing"                "(nil)" "$(cli GET nokey)"
check "SET overwrite"              "OK" "$(cli SET k1 v2)"
check "GET overwritten"            "v2" "$(cli GET k1)"
check "SET empty value"            "OK" "$(cli SET ev "")"
check "GET empty value"            "" "$(cli GET ev)"
check "TYPE string"                "string" "$(cli TYPE k1)"
check "TYPE missing"               "none" "$(cli TYPE nokey)"
check "DEL existing"               "1" "$(cli DEL k1)"
check "DEL missing"                "0" "$(cli DEL k1)"
check "SET for unlink"             "OK" "$(cli SET uk 1)"
check "UNLINK alias"               "1" "$(cli UNLINK uk)"
cli LPUSH wtl x >/dev/null
check "SET on list wrongtype"      "(error) WRONGTYPE Operation against a key holding the wrong kind of value" \
                                   "$(cli SET wtl v)"
check "GET on list wrongtype"      "(error) WRONGTYPE Operation against a key holding the wrong kind of value" \
                                   "$(cli GET wtl)"

# ---------------- 3. KEYS 通配符 ----------------
echo "===== [3] keys ====="
cli FLUSHALL >/dev/null
cli SET user:1 a >/dev/null
cli SET user:2 b >/dev/null
cli SET order:1 c >/dev/null
check "KEYS *"                     $'1) "order:1"\n2) "user:1"\n3) "user:2"' "$(cli KEYS '*')"
check "KEYS prefix pattern"        $'1) "user:1"\n2) "user:2"' "$(cli KEYS 'user:*')"
check "KEYS ? wildcard"            $'1) "user:1"\n2) "user:2"' "$(cli KEYS 'user:?')"
check "KEYS exact match"           '1) "user:1"' "$(cli KEYS 'user:1')"
check "KEYS no match"              "" "$(cli KEYS 'nomatch')"

# ---------------- 4. TTL / EXPIRE ----------------
echo "===== [4] expire ====="
cli SET tk v >/dev/null
check "EXPIRE set"                 "1" "$(cli EXPIRE tk 1)"
check "EXPIRE missing key"         "0" "$(cli EXPIRE nokey 10)"
check "EXPIRE bad seconds"         "(error) ERR value is not an integer or out of range" "$(cli EXPIRE tk abc)"
check "GET before expiry"          "v" "$(cli GET tk)"
sleep 2
check "GET after expiry"           "(nil)" "$(cli GET tk)"
check "TYPE after expiry"          "none" "$(cli TYPE tk)"
# SET 应清除已有 TTL（回归：原实现不清）
cli SET tk2 v >/dev/null
cli EXPIRE tk2 1 >/dev/null
cli SET tk2 v2 >/dev/null
sleep 2
check "SET clears TTL"             "v2" "$(cli GET tk2)"

# ---------------- 5. RENAME ----------------
echo "===== [5] rename ====="
cli SET rn1 a >/dev/null
check "RENAME"                     "OK" "$(cli RENAME rn1 rn2)"
check "RENAME old gone"            "(nil)" "$(cli GET rn1)"
check "RENAME new present"         "a" "$(cli GET rn2)"
check "RENAME missing src"         "(error) ERR no such key" "$(cli RENAME nokey xyz)"
check "RENAME bad argc"            "(error) ERR wrong number of arguments for 'rename' command" "$(cli RENAME onlyone)"
cli SET s1 v >/dev/null
check "RENAME same key"            "OK" "$(cli RENAME s1 s1)"
check "RENAME same key keeps value" "v" "$(cli GET s1)"
# RENAME 应覆盖目标（含类型不同的目标）
cli SET ra string >/dev/null
cli LPUSH rb x >/dev/null
check "RENAME overwrites dest"     "OK" "$(cli RENAME ra rb)"
check "dest type after overwrite"  "string" "$(cli TYPE rb)"
check "dest value after overwrite" "string" "$(cli GET rb)"

# ---------------- 6. List ----------------
echo "===== [6] list ====="
cli FLUSHALL >/dev/null
check "LPUSH multi"                "3" "$(cli LPUSH mylist a b c)"
check "RPUSH"                      "4" "$(cli RPUSH mylist d)"
check "LLEN"                       "4" "$(cli LLEN mylist)"
check "LLEN missing"               "0" "$(cli LLEN nolist)"
check "LPOP head"                  "c" "$(cli LPOP mylist)"
check "RPOP tail"                  "d" "$(cli RPOP mylist)"
check "LLEN after pops"            "2" "$(cli LLEN mylist)"
check "LGET"                       $'1) "b"\n2) "a"' "$(cli LGET mylist)"
check "LINDEX 0"                   "b" "$(cli LINDEX mylist 0)"
check "LINDEX -1"                  "a" "$(cli LINDEX mylist -1)"
check "LINDEX out of range"        "(nil)" "$(cli LINDEX mylist 5)"
check "LINDEX bad index"           "(error) ERR value is not an integer or out of range" "$(cli LINDEX mylist abc)"
check "LSET"                       "OK" "$(cli LSET mylist 0 X)"
check "LGET after LSET"            $'1) "X"\n2) "a"' "$(cli LGET mylist)"
check "LSET out of range"          "(error) ERR index out of range" "$(cli LSET mylist 9 X)"
check "LPOP missing"               "(nil)" "$(cli LPOP nolist)"
check "RPOP missing"               "(nil)" "$(cli RPOP nolist)"

# 空列表后 key 应被删除（Redis 行为）
cli LPUSH el x >/dev/null
check "LPOP last element"          "x" "$(cli LPOP el)"
check "empty list key removed"     "none" "$(cli TYPE el)"
check "KEYS after empty list"      "" "$(cli KEYS 'el')"

# LREM
cli LPUSH lr a b a c a >/dev/null   # -> [a c a b a]
check "LREM count=0"               "3" "$(cli LREM lr 0 a)"
check "LLEN after LREM"            "2" "$(cli LLEN lr)"
check "LREM no match"              "0" "$(cli LREM lr 2 z)"
check "LREM missing key"           "0" "$(cli LREM nolist 0 x)"
cli LPUSH lr2 x y x y x >/dev/null  # -> [x y x y x]
check "LREM negative count"        "2" "$(cli LREM lr2 -2 x)"
check "LGET after negative LREM"   $'1) "x"\n2) "y"\n3) "y"' "$(cli LGET lr2)"

# 列表 WRONGTYPE
cli SET str1 v >/dev/null
check "LPUSH on string"            "(error) WRONGTYPE Operation against a key holding the wrong kind of value" "$(cli LPUSH str1 x)"
check "LLEN on string"             "(error) WRONGTYPE Operation against a key holding the wrong kind of value" "$(cli LLEN str1)"
check "LPOP on string"             "(error) WRONGTYPE Operation against a key holding the wrong kind of value" "$(cli LPOP str1)"

# ---------------- 7. Hash ----------------
echo "===== [7] hash ====="
cli FLUSHALL >/dev/null
check "HSET new field"             "1" "$(cli HSET h f1 v1)"
check "HSET update field"          "0" "$(cli HSET h f1 v2)"
check "HGET"                       "v2" "$(cli HGET h f1)"
check "HGET missing field"         "(nil)" "$(cli HGET h nofield)"
check "HGET missing key"           "(nil)" "$(cli HGET nokey f)"
check "HEXISTS yes"                "1" "$(cli HEXISTS h f1)"
check "HEXISTS no"                 "0" "$(cli HEXISTS h x)"
check "HEXISTS missing key"        "0" "$(cli HEXISTS nokey f)"
check "HDEL"                       "1" "$(cli HDEL h f1)"
check "HDEL again"                 "0" "$(cli HDEL h f1)"
check "HMSET"                      "OK" "$(cli HMSET h2 a 1 b 2)"
check "HLEN"                       "2" "$(cli HLEN h2)"
check "HKEYS"                      $'1) "a"\n2) "b"' "$(cli HKEYS h2)"
check "HVALS"                      $'1) "1"\n2) "2"' "$(cli HVALS h2)"
check "HGETALL set"                $'1\n2\na\nb' \
    "$(cli HGETALL h2 | sed 's/^[0-9]*) //; s/"//g' | sort)"
check "HGETALL missing key"        "" "$(cli HGETALL nokey)"
check "HMSET bad argc"             "(error) ERR wrong number of arguments for 'hmset' command" "$(cli HMSET h3 onlykey)"
check "HMSET odd pairs"            "(error) ERR wrong number of arguments for 'hmset' command" "$(cli HMSET h4 a 1 b)"
check "HDEL last field removes key" "1" "$(cli HDEL h2 a)"
check "HDEL second field"          "1" "$(cli HDEL h2 b)"
check "TYPE after hash emptied"    "none" "$(cli TYPE h2)"
check "HLEN missing"               "0" "$(cli HLEN nokey)"

# Hash WRONGTYPE
cli SET strh v >/dev/null
check "HSET on string"             "(error) WRONGTYPE Operation against a key holding the wrong kind of value" "$(cli HSET strh f x)"
check "HGET on string"             "(error) WRONGTYPE Operation against a key holding the wrong kind of value" "$(cli HGET strh f)"
cli LPUSH listh v >/dev/null
check "HGET on list"               "(error) WRONGTYPE Operation against a key holding the wrong kind of value" "$(cli HGET listh f)"

# ---------------- 8. 协议健壮性（原始字节 / nc） ----------------
echo "===== [8] protocol ====="
raw_out 'PING\r\n' "$WORK/out.bin"
check_bytes "inline command"        '+PONG\r\n' "$WORK/out.bin"

raw_out 'ping\r\n' "$WORK/out.bin"
check_bytes "inline lowercase"      '+PONG\r\n' "$WORK/out.bin"

raw_out 'SET x 5\r\nGET x\r\n' "$WORK/out.bin"
check_bytes "inline two commands"   '+OK\r\n$1\r\n5\r\n' "$WORK/out.bin"

raw_out '\r\n\r\nPING\r\n' "$WORK/out.bin"
check_bytes "skip empty lines"      '+PONG\r\n' "$WORK/out.bin"

raw_out '*1\r\n$4\r\nPING\r\n' "$WORK/out.bin"
check_bytes "RESP PING"             '+PONG\r\n' "$WORK/out.bin"

# 粘包：3 条命令一次发送
raw_out '*3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\n1\r\n*2\r\n$3\r\nGET\r\n$1\r\na\r\n*2\r\n$3\r\nDEL\r\n$1\r\na\r\n' \
    "$WORK/out.bin"
check_bytes "pipelined 3 commands"  '+OK\r\n$1\r\n1\r\n:1\r\n' "$WORK/out.bin"

# 半包：分三次发送同一条命令（SET pk splitvalue + GET pk）
{
    printf '%b' '*3\r\n$3\r\nSET\r\n$2\r\n'
    sleep 0.3
    printf '%b' 'pk\r\n$10\r\nsplit'
    sleep 0.3
    printf '%b' 'value\r\n*2\r\n$3\r\nGET\r\n$2\r\npk\r\n'
} | nc -q1 -w5 "$HOST" "$PORT" >"$WORK/out.bin" 2>/dev/null
check_bytes "fragmented (half-packet) send" '+OK\r\n$10\r\nsplitvalue\r\n' "$WORK/out.bin"

# 畸形 RESP：不应崩溃，应回复 Protocol error
raw_out '*abc\r\n' "$WORK/out.bin"
check_bytes "malformed *abc"        '-ERR Protocol error\r\n' "$WORK/out.bin"

raw_out '*1\r\nx\r\n' "$WORK/out.bin"
check_bytes "malformed element"     '-ERR Protocol error\r\n' "$WORK/out.bin"

raw_out '*1\r\n$abc\r\n' "$WORK/out.bin"
check_bytes "malformed bulk length" '-ERR Protocol error\r\n' "$WORK/out.bin"

raw_out '*0\r\n' "$WORK/out.bin"
check_bytes "empty array command"   '-ERR Protocol error\r\n' "$WORK/out.bin"

# 半条命令后断开：服务器应保持存活
printf '%b' '*1\r\n$4\r\nPI' | nc -q1 -w3 "$HOST" "$PORT" >/dev/null 2>&1
check "server alive after truncated client" "PONG" "$(cli PING)"

# 二进制安全：value 含空格 + CRLF
raw_out '*3\r\n$3\r\nSET\r\n$2\r\nbk\r\n$6\r\na b\r\nc\r\n*2\r\n$3\r\nGET\r\n$2\r\nbk\r\n' \
    "$WORK/out.bin"
check_bytes "binary/space value round-trip" '+OK\r\n$6\r\na b\r\nc\r\n' "$WORK/out.bin"

# 大 value（100KB，超过旧实现 1024 字节缓冲）
BIG="$(python3 -c 'print("A" * 100000)')"
check "SET 100KB value"            "OK" "$(cli SET big "$BIG")"
ACTUAL_BIG="$(cli GET big)"
check "GET 100KB value (len)"      "100000" "${#ACTUAL_BIG}"
check "GET 100KB value (content)"  "$BIG" "$ACTUAL_BIG"

check "server still healthy"       "PONG" "$(cli PING)"

# ---------------- 9. 客户端功能 ----------------
echo "===== [9] client ====="
check "-h option"                  "PONG" "$("$CLI" -h 127.0.0.1 -p "$PORT" PING)"
PIPE_OUT="$(printf 'SET p1 1\nGET p1\nDEL p1\n' | "$CLI" -p "$PORT" --stdin)"
check "--stdin pipeline"           $'OK\n1\n1' "$PIPE_OUT"
printf 'SET qk "hello world"\n' | "$CLI" -p "$PORT" --stdin >/dev/null
check "quoted arg via --stdin"     "hello world" "$(cli GET qk)"
cli DEL qk >/dev/null
rc=0
"$CLI" -p 1 PING >/dev/null 2>&1 || rc=$?
check "connect failure exit code"  "2" "$rc"

# ---------------- 10. 并发 ----------------
echo "===== [10] concurrency ====="
cli FLUSHALL >/dev/null
PIDS=()
for i in $(seq 1 8); do
    cli SET "ck$i" "v$i" >/dev/null &
    PIDS+=($!)
done
wait "${PIDS[@]}"
CONC_OK=1
for i in $(seq 1 8); do
    v="$(cli GET "ck$i")"
    [ "$v" = "v$i" ] || CONC_OK=0
done
check "8 parallel SET/GET"         "1" "$CONC_OK"

PIDS=()
for i in $(seq 1 8); do
    cli PING >/dev/null &
    PIDS+=($!)
done
wait "${PIDS[@]}"
check "parallel PING alive"        "PONG" "$(cli PING)"

# ---------------- 11. 持久化（SIGINT + 重启） ----------------
echo "===== [11] persistence ====="
cli FLUSHALL >/dev/null
cli SET pk "hello world with spaces" >/dev/null
cli SET nk "$(printf 'line1\nline2')" >/dev/null
cli LPUSH pl x y >/dev/null
cli HMSET ph a b c d >/dev/null
cli SET tk2 soon >/dev/null
cli EXPIRE tk2 3 >/dev/null

stop_server
RC_STOP=$?
check "SIGINT exit code"           "0" "$RC_STOP"
if grep -q "Database dumped" "$SRV_LOG"; then
    check "SIGINT triggers dump"   "1" "1"
else
    check "SIGINT triggers dump"   "1" "0"
fi

start_server
if grep -q "Loaded dump.my_rdb" "$SRV_LOG"; then
    check "restart loads dump"     "1" "1"
else
    check "restart loads dump"     "1" "0"
fi
check "string with spaces survives" "hello world with spaces" "$(cli GET pk)"
check "string with newline survives" "$(printf 'line1\nline2')" "$(cli GET nk)"
check "list survives"              $'1) "y"\n2) "x"' "$(cli LGET pl)"
check "hash survives"              "b" "$(cli HGET ph a)"
check "TTL key still present"      "soon" "$(cli GET tk2)"
sleep 3.5
check "TTL survives restart (expires)" "(nil)" "$(cli GET tk2)"

# ---------------- 12. 第二实例 bind 失败 ----------------
echo "===== [12] process behavior ====="
BIND_OUT="$(cd "$WORK2" && "$SRV" "$PORT" 2>&1)"
BIND_RC=$?
check "second instance exit code"  "1" "$BIND_RC"
case "$BIND_OUT" in
    *"Failed to bind"*) check "second instance message" "1" "1" ;;
    *)                   check "second instance message" "1" "0" ;;
esac
check "first instance unaffected"  "PONG" "$(cli PING)"

# 服务器日志中不应出现崩溃信息
if grep -qiE "terminate|abort|segmentation|core dumped" "$SRV_LOG"; then
    check "no crash in server log" "1" "0"
else
    check "no crash in server log" "1" "1"
fi

# ---------------- 汇总 ----------------
stop_server >/dev/null 2>&1 || true
echo
echo "=============================="
echo "PASS: $PASS  FAIL: $FAIL"
if [ "$FAIL" -gt 0 ]; then
    printf 'Failed:\n'
    printf '  - %s\n' "${FAILED[@]}"
    echo "=============================="
    exit 1
fi
echo "ALL TESTS PASSED"
echo "=============================="
