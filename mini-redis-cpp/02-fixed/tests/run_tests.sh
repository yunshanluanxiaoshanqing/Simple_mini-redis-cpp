#!/usr/bin/env bash
# old_tt1（最小修复版）功能测试 —— WSL / Linux
# 只验证"代码所写的功能可以实现"：命令功能、TTL、持久化、基本健壮性。
# 不测增强能力（粘包流水线/WRONGTYPE/KEYS通配符等，见 CHANGES.md 已知限制）。
#
# 用法: bash tests/run_tests.sh
# 环境变量: TEST_PORT (默认 6499)

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}"
PORT="${TEST_PORT:-6499}"
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

check_has() {  # check_has <name> <substring> <actual>
    local name="$1" sub="$2" actual="$3"
    case "$actual" in
        *"$sub"*)
            PASS=$((PASS + 1)); printf 'PASS  %s\n' "$name" ;;
        *)
            FAIL=$((FAIL + 1)); FAILED+=("$name")
            printf 'FAIL  %s\n      expected to contain: [%s]\n      actual:   [%s]\n' \
                "$name" "$sub" "$actual" ;;
    esac
}

check_bytes() {  # check_bytes <name> <printf-buf-fmt> <outfile>
    local name="$1" fmt="$2" outfile="$3"
    printf '%b' "$fmt" >"$WORK/exp.bin"
    if cmp -s "$WORK/exp.bin" "$outfile"; then
        PASS=$((PASS + 1)); printf 'PASS  %s\n' "$name"
    else
        FAIL=$((FAIL + 1)); FAILED+=("$name")
        printf 'FAIL  %s\n      expected: %s\n      actual:   %s\n' "$name" \
            "$(xxd -p "$WORK/exp.bin" | tr -d '\n')" \
            "$(xxd -p "$outfile" | tr -d '\n')"
    fi
}

cli() { "$CLI" -p "$PORT" "$@"; }

raw_out() {
    printf '%b' "$1" | nc -q1 -w3 "$HOST" "$PORT" >"$2" 2>/dev/null
}

start_server() {
    (cd "$WORK" && exec "$SRV" "$PORT" >>"$WORK/server.log" 2>&1) &
    SRV_PID=$!
    local i
    for i in $(seq 1 50); do
        if "$CLI" -p "$PORT" PING >/dev/null 2>&1; then return 0; fi
        sleep 0.1
    done
    echo "FATAL: server failed to start" >&2
    cat "$WORK/server.log" >&2
    exit 1
}

stop_server() {
    kill -INT "$SRV_PID" 2>/dev/null
    wait "$SRV_PID" 2>/dev/null
}

# ---------------- 构建 ----------------
echo "===== [0] build ====="
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null || exit 1
cmake --build "$BUILD" -j || { echo "FATAL: build failed" >&2; exit 1; }
echo "build OK"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
start_server

# ---------------- 1. 基础命令 ----------------
echo "===== [1] basic ====="
check "PING"                    "PONG" "$(cli PING)"
check "ECHO arg"                "hello" "$(cli ECHO hello)"
check "ECHO no-arg"             "(error) ERR wrong number of arguments for 'echo' command" "$(cli ECHO)"
check "unknown command"         "(error) ERR unknown command" "$(cli NOTACOMMAND)"
check "case-insensitive (ping)" "PONG" "$(cli ping)"
check "TYPE no-arg"             "(error) ERR wrong number of arguments for 'type' command" "$(cli TYPE)"
check "DEL no-arg"              "(error) ERR wrong number of arguments for 'del' command" "$(cli DEL)"

# ---------------- 2. 字符串 ----------------
echo "===== [2] string ====="
check "SET"                     "OK" "$(cli SET k1 v1)"
check "GET"                     "v1" "$(cli GET k1)"
check "GET missing"             "(nil)" "$(cli GET nokey)"
check "SET overwrite"           "OK" "$(cli SET k1 v2)"
check "GET overwritten"         "v2" "$(cli GET k1)"
check "TYPE string"             "string" "$(cli TYPE k1)"
check "TYPE missing"            "none" "$(cli TYPE nokey)"
check "DEL existing"            "1" "$(cli DEL k1)"
check "DEL missing"             "0" "$(cli DEL k1)"
check "SET for unlink"          "OK" "$(cli SET uk 1)"
check "UNLINK removes"          "1" "$(cli UNLINK uk)"

# ---------------- 3. TTL / EXPIRE ----------------
echo "===== [3] expire ====="
cli SET tk v >/dev/null
check "EXPIRE set (:1)"         "1" "$(cli EXPIRE tk 1)"
check "EXPIRE missing key (:0)" "0" "$(cli EXPIRE nokey 10)"
check "EXPIRE bad seconds"      "(error) ERR value is not an integer or out of range" "$(cli EXPIRE tk abc)"
check "GET before expiry"       "v" "$(cli GET tk)"
sleep 2
check "GET after expiry"        "(nil)" "$(cli GET tk)"
check "TYPE after expiry"       "none" "$(cli TYPE tk)"
# 修复验证：SET 覆盖应清除旧 TTL
cli SET tk2 v >/dev/null
cli EXPIRE tk2 1 >/dev/null
cli SET tk2 v2 >/dev/null
sleep 2
check "SET clears old TTL"      "v2" "$(cli GET tk2)"

# ---------------- 4. KEYS 通配符 / FLUSHALL ----------------
echo "===== [4] keys ====="
cli FLUSHALL >/dev/null
cli SET user:1 a >/dev/null
cli SET user:2 b >/dev/null
cli SET order:1 c >/dev/null
KEYS_ALL="$(cli KEYS '*' | sed 's/^[0-9]*) //; s/"//g' | sort | tr '\n' ' ' | sed 's/ $//')"
check "KEYS all"                'order:1 user:1 user:2' "$KEYS_ALL"
check "KEYS prefix pattern"     'user:1 user:2' \
    "$(cli KEYS 'user:*' | sed 's/^[0-9]*) //; s/"//g' | sort | tr '\n' ' ' | sed 's/ $//')"
check "KEYS ? wildcard"         'user:1 user:2' \
    "$(cli KEYS 'user:?' | sed 's/^[0-9]*) //; s/"//g' | sort | tr '\n' ' ' | sed 's/ $//')"
check "KEYS exact match"        'user:1' \
    "$(cli KEYS 'user:1' | sed 's/^[0-9]*) //; s/"//g')"
check "KEYS no match"           "" "$(cli KEYS 'nomatch')"
cli FLUSHALL >/dev/null
check "FLUSHALL clears"         "" "$(cli KEYS)"
# 修复验证：FLUSHALL 应清除过期记录，否则同名 key 复用会被误删
cli SET a 1 >/dev/null
cli EXPIRE a 1 >/dev/null
sleep 2
cli FLUSHALL >/dev/null
cli SET a v >/dev/null
check "key reusable after FLUSHALL" "v" "$(cli GET a)"

# ---------------- 5. RENAME ----------------
echo "===== [5] rename ====="
cli SET rn1 a >/dev/null
check "RENAME"                  "OK" "$(cli RENAME rn1 rn2)"
check "RENAME old gone"         "(nil)" "$(cli GET rn1)"
check "RENAME new present"      "a" "$(cli GET rn2)"
check "RENAME missing src"      "(error) ERR no such key" "$(cli RENAME nokey xyz)"
check "RENAME bad argc"         "(error) ERR wrong number of arguments for 'rename' command" "$(cli RENAME onlyone)"
# RENAME 应覆盖目标（含类型不同的目标）
cli SET ra string >/dev/null
cli LPUSH rb x >/dev/null
check "RENAME overwrites dest"  "OK" "$(cli RENAME ra rb)"
check "dest type after overwrite" "string" "$(cli TYPE rb)"
check "dest value after overwrite" "string" "$(cli GET rb)"

# ---------------- 6. List ----------------
echo "===== [6] list ====="
cli FLUSHALL >/dev/null
check "LPUSH multi"             "3" "$(cli LPUSH mylist a b c)"
check "RPUSH"                   "4" "$(cli RPUSH mylist d)"
check "LLEN (was broken)"       "4" "$(cli LLEN mylist)"
check "LLEN missing"            "0" "$(cli LLEN nolist)"
check "LPOP head"               "c" "$(cli LPOP mylist)"
check "RPOP tail"               "d" "$(cli RPOP mylist)"
check "LGET (was broken)"       $'1) "b"\n2) "a"' "$(cli LGET mylist)"
check "LINDEX 0"                "b" "$(cli LINDEX mylist 0)"
check "LINDEX -1"               "a" "$(cli LINDEX mylist -1)"
check "LINDEX out of range"     "(nil)" "$(cli LINDEX mylist 5)"
check "LINDEX bad index"        "(error) ERR value is not an integer or out of range" "$(cli LINDEX mylist abc)"
check "LSET"                    "OK" "$(cli LSET mylist 0 X)"
check "LGET after LSET"         $'1) "X"\n2) "a"' "$(cli LGET mylist)"
check "LSET out of range"       "(error) ERR index out of range" "$(cli LSET mylist 9 X)"
check "LPOP missing"            "(nil)" "$(cli LPOP nolist)"
check "RPOP missing"            "(nil)" "$(cli RPOP nolist)"

# 空列表后 key 应被删除（Redis 行为）
cli LPUSH el x >/dev/null
check "LPOP last element"       "x" "$(cli LPOP el)"
check "empty list key removed"  "none" "$(cli TYPE el)"
cli LPUSH lr a b a c a >/dev/null
check "LREM count=0"            "3" "$(cli LREM lr 0 a)"
check "LLEN after LREM"         "2" "$(cli LLEN lr)"
check "LREM no match"           "0" "$(cli LREM lr 2 z)"
check "LREM missing key"        "0" "$(cli LREM nolist 0 x)"
cli LPUSH lr2 x y x y x >/dev/null
check "LREM negative count"     "2" "$(cli LREM lr2 -2 x)"
check "LGET after neg LREM"     $'1) "x"\n2) "y"\n3) "y"' "$(cli LGET lr2)"

# WRONGTYPE：类型冲突检查
WT="(error) WRONGTYPE Operation against a key holding the wrong kind of value"
cli SET str1 v >/dev/null
check "LPUSH on string"         "$WT" "$(cli LPUSH str1 x)"
check "LLEN on string"          "$WT" "$(cli LLEN str1)"
check "LPOP on string"          "$WT" "$(cli LPOP str1)"
cli LPUSH wtl x >/dev/null
check "SET on list"             "$WT" "$(cli SET wtl v)"
check "GET on list"             "$WT" "$(cli GET wtl)"

# ---------------- 7. Hash ----------------
echo "===== [7] hash ====="
cli FLUSHALL >/dev/null
check "HSET new field (:1)"     "1" "$(cli HSET h f1 v1)"
check "HSET update field (:0)"  "0" "$(cli HSET h f1 v2)"
check "HGET"                    "v2" "$(cli HGET h f1)"
check "HGET missing field"      "(nil)" "$(cli HGET h nofield)"
check "HGET missing key"        "(nil)" "$(cli HGET nokey f)"
check "HEXISTS yes"             "1" "$(cli HEXISTS h f1)"
check "HEXISTS no"              "0" "$(cli HEXISTS h x)"
check "HDEL"                    "1" "$(cli HDEL h f1)"
check "HDEL again"              "0" "$(cli HDEL h f1)"
check "empty hash key removed"  "none" "$(cli TYPE h)"
check "HMSET"                   "OK" "$(cli HMSET h2 a 1 b 2)"
check "HLEN"                    "2" "$(cli HLEN h2)"
check "HKEYS (unordered)"       'a b' \
    "$(cli HKEYS h2 | sed 's/^[0-9]*) //; s/"//g' | sort | tr '\n' ' ' | sed 's/ $//')"
check "HVALS (was broken)"      '1 2' \
    "$(cli HVALS h2 | sed 's/^[0-9]*) //; s/"//g' | sort | tr '\n' ' ' | sed 's/ $//')"
check "HGETALL (was broken)"    '1 2 a b' \
    "$(cli HGETALL h2 | sed 's/^[0-9]*) //; s/"//g' | sort | tr '\n' ' ' | sed 's/ $//')"
check "HGETALL missing key"     "" "$(cli HGETALL nokey)"
check "HMSET odd pairs"         "(error) ERR wrong number of arguments for 'hmset' command" "$(cli HMSET h3 a 1 b)"
check "HLEN missing"            "0" "$(cli HLEN nokey)"

# WRONGTYPE：对非 hash 执行哈希命令
cli SET strh v >/dev/null
check "HSET on string"          "$WT" "$(cli HSET strh f x)"
check "HGET on string"          "$WT" "$(cli HGET strh f)"
cli LPUSH listh v >/dev/null
check "HGET on list"            "$WT" "$(cli HGET listh f)"
check "HMSET on list"           "$WT" "$(cli HMSET listh a 1)"

# ---------------- 8. 协议基础（原始字节 / nc） ----------------
echo "===== [8] protocol ====="
raw_out 'PING\r\n' "$WORK/out.bin"
check_bytes "inline command"    '+PONG\r\n' "$WORK/out.bin"
raw_out '*1\r\n$4\r\nPING\r\n' "$WORK/out.bin"
check_bytes "RESP PING"         '+PONG\r\n' "$WORK/out.bin"
raw_out 'SET x5\r\nGET x\r\n' "$WORK/out.bin"
check_bytes "inline two lines (first only)" '+OK\r\n' "$WORK/out.bin"
# 畸形 RESP 不应使服务器崩溃（修复验证：stoi 已加 try/catch）
raw_out '*abc\r\n' "$WORK/out.bin"
check_bytes "malformed RESP"    'Err: Empty command\r\n' "$WORK/out.bin"
check "server alive after malformed" "PONG" "$(cli PING)"
# 半条命令后断开，服务器应保持存活
printf '%b' '*1\r\n$4\r\nPI' | nc -q1 -w3 "$HOST" "$PORT" >/dev/null 2>&1
check "server alive after truncated client" "PONG" "$(cli PING)"

# ---------------- 9. 客户端 ----------------
echo "===== [9] client ====="
check "-h option"               "PONG" "$("$CLI" -h 127.0.0.1 -p "$PORT" PING)"
rc=0
"$CLI" -p 1 PING >/dev/null 2>&1 || rc=$?
check "connect failure exit code" "2" "$rc"

# ---------------- 10. 并发 ----------------
echo "===== [10] concurrency ====="
cli FLUSHALL >/dev/null
PIDS=()
for i in $(seq 1 4); do
    cli SET "ck$i" "v$i" >/dev/null &
    PIDS+=($!)
done
wait "${PIDS[@]}"
CONC_OK=1
for i in $(seq 1 4); do
    [ "$(cli GET "ck$i")" = "v$i" ] || CONC_OK=0
done
check "4 parallel SET/GET"      "1" "$CONC_OK"

# ---------------- 11. 持久化 ----------------
echo "===== [11] persistence ====="
cli FLUSHALL >/dev/null
cli SET pk "hello world with spaces" >/dev/null
cli SET nk "$(printf 'line1\nline2')" >/dev/null
cli LPUSH pl a b >/dev/null
cli HMSET ph f1 v1 >/dev/null
cli SET tk2 soon >/dev/null
cli EXPIRE tk2 6 >/dev/null

stop_server
if grep -q "Database dumped" "$WORK/server.log"; then
    check "SIGINT dumps database" "1" "1"
else
    check "SIGINT dumps database" "1" "0"
fi

start_server
if grep -q "load success" "$WORK/server.log"; then
    check "restart loads dump"  "1" "1"
else
    check "restart loads dump"  "1" "0"
fi
check "string with spaces survives"  "hello world with spaces" "$(cli GET pk)"
check "string with newline survives" "$(printf 'line1\nline2')" "$(cli GET nk)"
check "list survives"           $'1) "b"\n2) "a"' "$(cli LGET pl)"
check "hash survives"           "v1" "$(cli HGET ph f1)"
check "TTL key still present"   "soon" "$(cli GET tk2)"
sleep 6
check "TTL survives restart (expires)" "(nil)" "$(cli GET tk2)"

# 服务器日志不应有崩溃信息
if grep -qiE "terminate|abort|segmentation|core dumped" "$WORK/server.log"; then
    check "no crash in log"     "1" "0"
else
    check "no crash in log"     "1" "1"
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
