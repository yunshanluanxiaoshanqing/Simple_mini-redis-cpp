#!/usr/bin/env bash
# 01-original（o_tt1，原始项目 + 低级错误修正）冒烟测试 —— WSL / Linux
# 项目无客户端，全部通过 nc 原始协议断言；按本版本实际行为验证。
#
# 用法: bash tests/run_tests.sh
# 环境变量: TEST_PORT (默认 6489)

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}"
PORT="${TEST_PORT:-6489}"
HOST=127.0.0.1
SRV="$BUILD/redis-server"

PASS=0
FAIL=0
FAILED=()

check() {
    local name="$1" expected actual
    expected=$(printf '%b' "$2")   # 期望值支持 \n \r 转义
    actual="$3"
    if [ "$expected" = "$actual" ]; then
        PASS=$((PASS + 1)); printf 'PASS  %s\n' "$name"
    else
        FAIL=$((FAIL + 1)); FAILED+=("$name")
        printf 'FAIL  %s\n      expected: [%s]\n      actual:   [%s]\n' \
            "$name" "$expected" "$actual"
    fi
}

check_has() {
    local name="$1" sub="$2" actual="$3"
    case "$actual" in
        *"$sub"*) PASS=$((PASS + 1)); printf 'PASS  %s\n' "$name" ;;
        *) FAIL=$((FAIL + 1)); FAILED+=("$name")
           printf 'FAIL  %s\n      expected to contain: [%s]\n      actual:   [%s]\n' \
               "$name" "$sub" "$actual" ;;
    esac
}

check_bytes() {
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

# 发送内联命令，返回去掉 \r 的原始响应
q() { printf '%b' "$1" | nc -N -w3 "$HOST" "$PORT" | tr -d '\r'; }

# 从 RESP 数组响应中提取值行（跳过 *N 与 $L 行），排序后空格拼接
alist() { q "$1" | grep -v '^[$*]' | sort | tr '\n' ' ' | sed 's/ $//'; }

raw_out() { printf '%b' "$1" | nc -N -w3 "$HOST" "$PORT" >"$2" 2>/dev/null; }

start_server() {
    (cd "$WORK" && exec "$SRV" "$PORT" >>"$WORK/server.log" 2>&1) &
    SRV_PID=$!
    local i
    for i in $(seq 1 50); do
        if [ "$(q 'PING\n')" = "+PONG" ]; then return 0; fi
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

# ---------------- 1. 基础 ----------------
echo "===== [1] basic ====="
check "PING"                    "+PONG" "$(q 'PING\n')"
check "inline PING (crlf)"      "+PONG" "$(q 'PING\r\n')"
check "ECHO arg"                "+hello" "$(q 'ECHO hello\n')"
check "ECHO no-arg"             "-ERR wrong  'echo' command" "$(q 'ECHO\n')"
check "unknown command"         "-ERR unknown command" "$(q 'NOTACOMMAND\n')"
check "case-insensitive (ping)" "+PONG" "$(q 'ping\n')"

# ---------------- 2. 字符串 ----------------
echo "===== [2] string ====="
check "SET"                     "+OK" "$(q 'SET k1 v1\n')"
check "GET"                     '$2\nv1' "$(q 'GET k1\n')"
check "GET missing"             '$-1' "$(q 'GET nokey\n')"
check "SET overwrite"           "+OK" "$(q 'SET k1 v2\n')"
check "GET overwritten"         '$2\nv2' "$(q 'GET k1\n')"
check "TYPE string"             "+string" "$(q 'TYPE k1\n')"
check "TYPE missing"            "+none" "$(q 'TYPE nokey\n')"
check "DEL existing"            ":1" "$(q 'DEL k1\n')"
check "DEL missing"             ":0" "$(q 'DEL k1\n')"
check "UNLINK alias"            "+OK" "$(q 'SET uk 1\n')"  # 下一行为实际删除
check "UNLINK removes"          ":1" "$(q 'UNLINK uk\n')"

# ---------------- 3. TTL / EXPIRE ----------------
echo "===== [3] expire ====="
q 'SET tk v\n' >/dev/null
check "EXPIRE set"              "+OK" "$(q 'EXPIRE tk 2\n')"
check "EXPIRE missing key"      "-ERR  key not found" "$(q 'EXPIRE nokey 10\n')"
check_has "EXPIRE bad seconds"  "invalid expiration time" "$(q 'EXPIRE tk abc\n')"
raw_out 'GET tk\n' "$WORK/out.bin"
check_bytes "GET before expiry" '$1\r\nv\r\n' "$WORK/out.bin"
sleep 3
check "GET after expiry"        '$-1' "$(q 'GET tk\n')"
check "TYPE after expiry"       "+none" "$(q 'TYPE tk\n')"
# 修复验证：SET 覆盖清除旧 TTL
q 'SET tk2 v\n' >/dev/null
q 'EXPIRE tk2 1\n' >/dev/null
q 'SET tk2 v2\n' >/dev/null
sleep 2
check "SET clears old TTL"      '$2\nv2' "$(q 'GET tk2\n')"

# ---------------- 4. KEYS / FLUSHALL ----------------
echo "===== [4] keys ====="
q 'FLUSHALL\n' >/dev/null
q 'SET user:1 a\n' >/dev/null
q 'SET user:2 b\n' >/dev/null
q 'LPUSH mylist x\n' >/dev/null
check "KEYS all"                'mylist user:1 user:2' "$(alist 'KEYS\n')"
q 'FLUSHALL\n' >/dev/null
check "FLUSHALL clears"         "" "$(alist 'KEYS\n')"

# ---------------- 5. RENAME ----------------
echo "===== [5] rename ====="
q 'SET rn1 a\n' >/dev/null
check "RENAME"                  "+OK" "$(q 'RENAME rn1 rn2\n')"
check "RENAME old gone"         '$-1' "$(q 'GET rn1\n')"
check "RENAME new present"      '$1\na' "$(q 'GET rn2\n')"
check "RENAME missing src"      "-ERR  key not found" "$(q 'RENAME nokey xyz\n')"
check "RENAME bad argc"         "-ERR RENAME requires key and new key" "$(q 'RENAME onlyone\n')"

# ---------------- 6. List ----------------
echo "===== [6] list ====="
q 'FLUSHALL\n' >/dev/null
check "LPUSH multi"             ":3" "$(q 'LPUSH mylist a b c\n')"
check "RPUSH"                   ":4" "$(q 'RPUSH mylist d\n')"
check "LLEN (was LLNE typo)"    ":4" "$(q 'LLEN mylist\n')"
check "LLEN missing"            ":0" "$(q 'LLEN nolist\n')"
check "LPOP head"               '$1\nc' "$(q 'LPOP mylist\n')"
check "RPOP tail"               '$1\nd' "$(q 'RPOP mylist\n')"
check "LGET"                    'a b' "$(alist 'LGET mylist\n')"
check "LINDEX 0"                '$1\nb' "$(q 'LINDEX mylist 0\n')"
check "LINDEX -1"               '$1\na' "$(q 'LINDEX mylist -1\n')"
check "LINDEX out of range"     '$-1' "$(q 'LINDEX mylist 5\n')"
check_has "LINDEX bad index"    "invalid index" "$(q 'LINDEX mylist abc\n')"
check "LSET"                    "+OK" "$(q 'LSET mylist 0 X\n')"
check "LPOP missing"            '$-1' "$(q 'LPOP nolist\n')"
q 'LPUSH lr a b a c a\n' >/dev/null
check "LREM count=0"            ":3" "$(q 'LREM lr 0 a\n')"
check "LLEN after LREM"         ":2" "$(q 'LLEN lr\n')"
check "LREM missing key"        ":0" "$(q 'LREM nolist 0 x\n')"
q 'LPUSH lr2 x y x y x\n' >/dev/null
check "LREM negative count"     ":2" "$(q 'LREM lr2 -2 x\n')"

# ---------------- 7. Hash ----------------
echo "===== [7] hash ====="
q 'FLUSHALL\n' >/dev/null
check "HSET"                    "+OK" "$(q 'HSET h f1 v1\n')"
check "HSET update"             "+OK" "$(q 'HSET h f1 v2\n')"
check "HGET"                    '$2\nv2' "$(q 'HGET h f1\n')"
check "HGET missing field"      '$-1' "$(q 'HGET h nofield\n')"
check "HGET missing key"        '$-1' "$(q 'HGET nokey f\n')"
check "HEXISTS yes"             ":1" "$(q 'HEXISTS h f1\n')"
check "HEXISTS no"              ":0" "$(q 'HEXISTS h x\n')"
check "HDEL"                    ":1" "$(q 'HDEL h f1\n')"
check "HDEL again"              ":0" "$(q 'HDEL h f1\n')"
check "HMSET"                   "+OK" "$(q 'HMSET h2 a 1 b 2\n')"
check "HLEN"                    ":2" "$(q 'HLEN h2\n')"
check "HKEYS"                   'a b' "$(alist 'HKEYS h2\n')"
check "HVALS (return was missing)" '1 2' "$(alist 'HVALS h2\n')"
check "HGETALL (name was broken)"  '1 2 a b' "$(alist 'HGETALL h2\n')"
check "HGETALL missing key"     "" "$(alist 'HGETALL nokey\n')"
check_has "HMSET odd pairs"     "floowed" "$(q 'HMSET h3 a 1 b\n')"
check "HLEN missing"            ":0" "$(q 'HLEN nokey\n')"

# ---------------- 8. 协议健壮性 ----------------
echo "===== [8] protocol ====="
raw_out 'PING\r\n' "$WORK/out.bin"
check_bytes "inline RESP-crlf"  '+PONG\r\n' "$WORK/out.bin"
raw_out '*1\r\n$4\r\nPING\r\n' "$WORK/out.bin"
check_bytes "RESP PING"         '+PONG\r\n' "$WORK/out.bin"
# 畸形 RESP 不应崩溃（stoi 已加 try/catch）
raw_out '*abc\r\n' "$WORK/out.bin"
check_bytes "malformed RESP"    'Err: Empty command\r\n' "$WORK/out.bin"
check "server alive after malformed" "+PONG" "$(q 'PING\n')"
# 半条命令后断开
printf '%b' '*1\r\n$4\r\nPI' | nc -N -w3 "$HOST" "$PORT" >/dev/null 2>&1
check "server alive after truncated client" "+PONG" "$(q 'PING\n')"

# ---------------- 9. 并发 ----------------
echo "===== [9] concurrency ====="
q 'FLUSHALL\n' >/dev/null
PIDS=()
for i in $(seq 1 4); do
    printf '%b' "SET ck$i v$i\n" | nc -N -w3 "$HOST" "$PORT" >/dev/null &
    PIDS+=($!)
done
wait "${PIDS[@]}"
CONC_OK=1
for i in $(seq 1 4); do
    [ "$(q "GET ck$i\n" | tail -1)" = "v$i" ] || CONC_OK=0
done
check "4 parallel SET/GET"      "1" "$CONC_OK"

# ---------------- 10. 持久化 ----------------
echo "===== [10] persistence ====="
q 'FLUSHALL\n' >/dev/null
q 'SET pk myvalue\n' >/dev/null
q 'LPUSH pl a b\n' >/dev/null
q 'HMSET ph f1 v1\n' >/dev/null

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
check "string survives"         '$7\nmyvalue' "$(q 'GET pk\n')"
check "list survives"           'a b' "$(alist 'LGET pl\n')"
check "hash survives"           '$2\nv1' "$(q 'HGET ph f1\n')"

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
