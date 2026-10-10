# 服务器 + 客户端全面测试（Windows / PowerShell 7+）
# 与 tests/run_tests.sh 等价覆盖：功能、协议、并发、持久化、进程行为。
# 持久化用 REDIS_PERSIST_INTERVAL 周期 dump（Windows 无法方便地发送 SIGINT）。
#
# 用法: pwsh -File tests\run_tests.ps1 [-BuildDir build-posix]
# 环境变量: TEST_PORT (默认 6401)

param(
    [int]$Port = 6401,
    [string]$BuildDir = "build-win"
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
$Build = Join-Path $Root $BuildDir
$Cli = Join-Path $Build "redis-cli.exe"
$Srv = Join-Path $Build "redis-server.exe"

$script:Pass = 0
$script:Fail = 0
$script:Failed = @()
$script:Work = $null
$script:SrvProc = $null
$script:SrvLog = $null

function Check([string]$Name, [string]$Expected, [string]$Actual) {
    if ($Expected -eq $Actual) {
        $script:Pass++
        Write-Host "PASS  $Name"
    } else {
        $script:Fail++
        $script:Failed += $Name
        Write-Host "FAIL  $Name"
        Write-Host "      expected: [$Expected]"
        Write-Host "      actual:   [$Actual]"
    }
}

# 调用 redis-cli，输出归一化（行数组 -> `n 连接，去掉 `r）
function Invoke-Cli {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]]$CliArgs)
    $lines = & $Cli -p $Port @CliArgs 2>$null
    if ($null -eq $lines) { return "" }
    if ($lines -is [string]) { return $lines }
    return ($lines -join "`n")
}

function Start-Server {
    param([string]$WorkDir, [string]$Interval = "")
    if ($Interval -ne "") { $env:REDIS_PERSIST_INTERVAL = $Interval }
    else { Remove-Item Env:REDIS_PERSIST_INTERVAL -ErrorAction SilentlyContinue }
    $outLog = Join-Path $WorkDir "server.log"
    $errLog = Join-Path $WorkDir "server.err.log"
    $script:SrvProc = Start-Process -FilePath $Srv -ArgumentList $Port `
        -WorkingDirectory $WorkDir `
        -RedirectStandardOutput $outLog -RedirectStandardError $errLog `
        -PassThru -NoNewWindow
    $script:SrvLog = $outLog
    # 等待就绪
    for ($i = 0; $i -lt 50; $i++) {
        $p = Invoke-Cli PING
        if ($p -eq "PONG") { return }
        Start-Sleep -Milliseconds 100
    }
    Write-Host "FATAL: server failed to start"
    if (Test-Path $outLog) { Get-Content $outLog | Write-Host }
    if (Test-Path $errLog) { Get-Content $errLog | Write-Host }
    exit 1
}

function Stop-Server {
    if ($script:SrvProc -and -not $script:SrvProc.HasExited) {
        Stop-Process -Id $script:SrvProc.Id -Force
        $script:SrvProc.WaitForExit()
    }
}

function Wait-Log([string]$Path, [string]$Pattern, [int]$TimeoutMs = 6000) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $TimeoutMs) {
        if (Test-Path $Path) {
            $t = Get-Content $Path -Raw -ErrorAction SilentlyContinue
            if ($t -and $t -match $Pattern) { return $true }
        }
        Start-Sleep -Milliseconds 200
    }
    return $false
}

function Read-UntilQuiet($Stream, [int]$QuietMs = 400, [int]$TotalMs = 3000) {
    $ms = New-Object System.IO.MemoryStream
    $buf = New-Object byte[] 8192
    $last = [Diagnostics.Stopwatch]::StartNew()
    $total = [Diagnostics.Stopwatch]::StartNew()
    while ($total.ElapsedMilliseconds -lt $TotalMs) {
        if ($Stream.DataAvailable) {
            $n = $Stream.Read($buf, 0, $buf.Length)
            if ($n -le 0) { break }
            $ms.Write($buf, 0, $n)
            $last.Restart()
        } elseif ($last.ElapsedMilliseconds -ge $QuietMs) {
            break
        } else {
            Start-Sleep -Milliseconds 30
        }
    }
    return $ms.ToArray()
}

# 单包发送原始 ASCII 协议并读取响应字节
function Send-Raw([string]$Ascii) {
    $c = New-Object System.Net.Sockets.TcpClient
    try {
        $c.Connect("127.0.0.1", $Port)
        $s = $c.GetStream()
        $bytes = [Text.Encoding]::ASCII.GetBytes($Ascii)
        $s.Write($bytes, 0, $bytes.Length)
        $s.Flush()
        return Read-UntilQuiet $s
    } finally { $c.Close() }
}

# 分片发送（模拟半包），各片间等待
function Send-RawChunks([string[]]$Chunks, [int]$GapMs = 300) {
    $c = New-Object System.Net.Sockets.TcpClient
    try {
        $c.Connect("127.0.0.1", $Port)
        $s = $c.GetStream()
        foreach ($chunk in $Chunks) {
            $bytes = [Text.Encoding]::ASCII.GetBytes($chunk)
            $s.Write($bytes, 0, $bytes.Length)
            $s.Flush()
            Start-Sleep -Milliseconds $GapMs
        }
        return Read-UntilQuiet $s
    } finally { $c.Close() }
}

function Check-Bytes([string]$Name, [string]$ExpectedAscii, [byte[]]$Actual) {
    $exp = [Text.Encoding]::ASCII.GetBytes($ExpectedAscii)
    $ok = ($exp.Length -eq $Actual.Length)
    if ($ok) {
        for ($i = 0; $i -lt $exp.Length; $i++) {
            if ($exp[$i] -ne $Actual[$i]) { $ok = $false; break }
        }
    }
    if ($ok) {
        $script:Pass++
        Write-Host "PASS  $Name"
    } else {
        $script:Fail++
        $script:Failed += $Name
        $hex = ($Actual | ForEach-Object { $_.ToString("x2") }) -join ""
        $ehex = ($exp | ForEach-Object { $_.ToString("x2") }) -join ""
        if ($hex.Length -gt 200) { $hex = $hex.Substring(0, 200) }
        if ($ehex.Length -gt 200) { $ehex = $ehex.Substring(0, 200) }
        Write-Host "FAIL  $Name"
        Write-Host "      expected: $ehex"
        Write-Host "      actual:   $hex"
    }
}

$WT = "(error) WRONGTYPE Operation against a key holding the wrong kind of value"

# ================= [0] build =================
Write-Host "===== [0] build ====="
& cmake -S $Root -B $Build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Host "FATAL: cmake configure failed"; exit 1 }
& cmake --build $Build -j | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Host "FATAL: build failed"; exit 1 }
Write-Host "build OK"

$script:Work = Join-Path $env:TEMP ("redis_test_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $script:Work | Out-Null
$Work2 = Join-Path $env:TEMP ("redis_test2_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $Work2 | Out-Null

Start-Server -WorkDir $script:Work

try {
    # ================= [1] 基础 =================
    Write-Host "===== [1] basic ====="
    Check "PING"                    "PONG" (Invoke-Cli PING)
    Check "ECHO arg"                "hello" (Invoke-Cli ECHO hello)
    Check "ECHO no-arg"             "(error) ERR wrong number of arguments for 'echo' command" (Invoke-Cli ECHO)
    Check "unknown command"         "(error) ERR unknown command" (Invoke-Cli NOTACOMMAND)
    Check "case-insensitive (ping)" "PONG" (Invoke-Cli ping)

    # ================= [2] 字符串 =================
    Write-Host "===== [2] string ====="
    Check "SET"                     "OK" (Invoke-Cli SET k1 v1)
    Check "GET"                     "v1" (Invoke-Cli GET k1)
    Check "GET missing"             "(nil)" (Invoke-Cli GET nokey)
    Check "SET overwrite"           "OK" (Invoke-Cli SET k1 v2)
    Check "GET overwritten"         "v2" (Invoke-Cli GET k1)
    # PS5.1 向原生命令传参会丢弃空串，空值用 --stdin 通道构造
    $setEv = 'SET ev ""' | & $Cli -p $Port --stdin 2>$null
    Check "SET empty value"         "OK" (($setEv | ForEach-Object { $_ }) -join "`n")
    Check "GET empty value"         "" (Invoke-Cli GET ev)
    Check "TYPE empty value"        "string" (Invoke-Cli TYPE ev)
    Check "TYPE string"             "string" (Invoke-Cli TYPE k1)
    Check "TYPE missing"            "none" (Invoke-Cli TYPE nokey)
    Check "DEL existing"            "1" (Invoke-Cli DEL k1)
    Check "DEL missing"             "0" (Invoke-Cli DEL k1)
    Check "SET for unlink"          "OK" (Invoke-Cli SET uk 1)
    Check "UNLINK alias"            "1" (Invoke-Cli UNLINK uk)
    Invoke-Cli LPUSH wtl x | Out-Null
    Check "SET on list wrongtype"   $WT (Invoke-Cli SET wtl v)
    Check "GET on list wrongtype"   $WT (Invoke-Cli GET wtl)

    # ================= [3] KEYS =================
    Write-Host "===== [3] keys ====="
    Invoke-Cli FLUSHALL | Out-Null
    Invoke-Cli SET user:1 a | Out-Null
    Invoke-Cli SET user:2 b | Out-Null
    Invoke-Cli SET order:1 c | Out-Null
    Check "KEYS *"                  ((@('1) "order:1"', '2) "user:1"', '3) "user:2"')) -join "`n") (Invoke-Cli KEYS "*")
    Check "KEYS prefix pattern"     ((@('1) "user:1"', '2) "user:2"')) -join "`n") (Invoke-Cli KEYS "user:*")
    Check "KEYS ? wildcard"         ((@('1) "user:1"', '2) "user:2"')) -join "`n") (Invoke-Cli KEYS "user:?")
    Check "KEYS exact match"        '1) "user:1"' (Invoke-Cli KEYS "user:1")
    Check "KEYS no match"           "" (Invoke-Cli KEYS "nomatch")

    # ================= [4] TTL =================
    Write-Host "===== [4] expire ====="
    Invoke-Cli SET tk v | Out-Null
    Check "EXPIRE set"              "1" (Invoke-Cli EXPIRE tk 1)
    Check "EXPIRE missing key"      "0" (Invoke-Cli EXPIRE nokey 10)
    Check "EXPIRE bad seconds"      "(error) ERR value is not an integer or out of range" (Invoke-Cli EXPIRE tk abc)
    Check "GET before expiry"       "v" (Invoke-Cli GET tk)
    Start-Sleep -Seconds 2
    Check "GET after expiry"        "(nil)" (Invoke-Cli GET tk)
    Check "TYPE after expiry"       "none" (Invoke-Cli TYPE tk)
    Invoke-Cli SET tk2 v | Out-Null
    Invoke-Cli EXPIRE tk2 1 | Out-Null
    Invoke-Cli SET tk2 v2 | Out-Null
    Start-Sleep -Seconds 2
    Check "SET clears TTL"          "v2" (Invoke-Cli GET tk2)

    # ================= [5] RENAME =================
    Write-Host "===== [5] rename ====="
    Invoke-Cli SET rn1 a | Out-Null
    Check "RENAME"                  "OK" (Invoke-Cli RENAME rn1 rn2)
    Check "RENAME old gone"         "(nil)" (Invoke-Cli GET rn1)
    Check "RENAME new present"      "a" (Invoke-Cli GET rn2)
    Check "RENAME missing src"      "(error) ERR no such key" (Invoke-Cli RENAME nokey xyz)
    Check "RENAME bad argc"         "(error) ERR wrong number of arguments for 'rename' command" (Invoke-Cli RENAME onlyone)
    Invoke-Cli SET s1 v | Out-Null
    Check "RENAME same key"         "OK" (Invoke-Cli RENAME s1 s1)
    Check "RENAME same key keeps"   "v" (Invoke-Cli GET s1)
    Invoke-Cli SET ra string | Out-Null
    Invoke-Cli LPUSH rb x | Out-Null
    Check "RENAME overwrites dest"  "OK" (Invoke-Cli RENAME ra rb)
    Check "dest type after"         "string" (Invoke-Cli TYPE rb)
    Check "dest value after"        "string" (Invoke-Cli GET rb)

    # ================= [6] List =================
    Write-Host "===== [6] list ====="
    Invoke-Cli FLUSHALL | Out-Null
    Check "LPUSH multi"             "3" (Invoke-Cli LPUSH mylist a b c)
    Check "RPUSH"                   "4" (Invoke-Cli RPUSH mylist d)
    Check "LLEN"                    "4" (Invoke-Cli LLEN mylist)
    Check "LLEN missing"            "0" (Invoke-Cli LLEN nolist)
    Check "LPOP head"               "c" (Invoke-Cli LPOP mylist)
    Check "RPOP tail"               "d" (Invoke-Cli RPOP mylist)
    Check "LGET"                    ((@('1) "b"', '2) "a"')) -join "`n") (Invoke-Cli LGET mylist)
    Check "LINDEX 0"                "b" (Invoke-Cli LINDEX mylist 0)
    Check "LINDEX -1"               "a" (Invoke-Cli LINDEX mylist -1)
    Check "LINDEX out of range"     "(nil)" (Invoke-Cli LINDEX mylist 5)
    Check "LSET"                    "OK" (Invoke-Cli LSET mylist 0 X)
    Check "LGET after LSET"         ((@('1) "X"', '2) "a"')) -join "`n") (Invoke-Cli LGET mylist)
    Check "LSET out of range"       "(error) ERR index out of range" (Invoke-Cli LSET mylist 9 X)
    Check "LPOP missing"            "(nil)" (Invoke-Cli LPOP nolist)
    Invoke-Cli LPUSH el x | Out-Null
    Check "LPOP last element"       "x" (Invoke-Cli LPOP el)
    Check "empty list key removed"  "none" (Invoke-Cli TYPE el)
    Invoke-Cli LPUSH lr a b a c a | Out-Null
    Check "LREM count=0"            "3" (Invoke-Cli LREM lr 0 a)
    Check "LLEN after LREM"         "2" (Invoke-Cli LLEN lr)
    Check "LREM no match"           "0" (Invoke-Cli LREM lr 2 z)
    Check "LREM missing key"        "0" (Invoke-Cli LREM nolist 0 x)
    Invoke-Cli LPUSH lr2 x y x y x | Out-Null
    Check "LREM negative count"     "2" (Invoke-Cli LREM lr2 -2 x)
    Check "LGET after neg LREM"     ((@('1) "x"', '2) "y"', '3) "y"')) -join "`n") (Invoke-Cli LGET lr2)
    Invoke-Cli SET str1 v | Out-Null
    Check "LPUSH on string"         $WT (Invoke-Cli LPUSH str1 x)
    Check "LLEN on string"          $WT (Invoke-Cli LLEN str1)
    Check "LPOP on string"          $WT (Invoke-Cli LPOP str1)

    # ================= [7] Hash =================
    Write-Host "===== [7] hash ====="
    Invoke-Cli FLUSHALL | Out-Null
    Check "HSET new field"          "1" (Invoke-Cli HSET h f1 v1)
    Check "HSET update field"       "0" (Invoke-Cli HSET h f1 v2)
    Check "HGET"                    "v2" (Invoke-Cli HGET h f1)
    Check "HGET missing field"      "(nil)" (Invoke-Cli HGET h nofield)
    Check "HGET missing key"        "(nil)" (Invoke-Cli HGET nokey f)
    Check "HEXISTS yes"             "1" (Invoke-Cli HEXISTS h f1)
    Check "HEXISTS no"              "0" (Invoke-Cli HEXISTS h x)
    Check "HDEL"                    "1" (Invoke-Cli HDEL h f1)
    Check "HDEL again"              "0" (Invoke-Cli HDEL h f1)
    Check "HMSET"                   "OK" (Invoke-Cli HMSET h2 a 1 b 2)
    Check "HLEN"                    "2" (Invoke-Cli HLEN h2)
    Check "HKEYS"                   ((@('1) "a"', '2) "b"')) -join "`n") (Invoke-Cli HKEYS h2)
    Check "HVALS"                   ((@('1) "1"', '2) "2"')) -join "`n") (Invoke-Cli HVALS h2)
    $hgetall = Invoke-Cli HGETALL h2
    $hset_norm = ($hgetall -split "`n" | ForEach-Object { $_ -replace '^[0-9]+\) ', '' -replace '"', '' } | Sort-Object) -join "`n"
    Check "HGETALL set"             ((@('1', '2', 'a', 'b')) -join "`n") $hset_norm
    Check "HMSET bad argc"          "(error) ERR wrong number of arguments for 'hmset' command" (Invoke-Cli HMSET h3 onlykey)
    Check "HMSET odd pairs"         "(error) ERR wrong number of arguments for 'hmset' command" (Invoke-Cli HMSET h4 a 1 b)
    Check "HDEL last removes key"   "1" (Invoke-Cli HDEL h2 a)
    Check "HDEL second field"       "1" (Invoke-Cli HDEL h2 b)
    Check "TYPE after hash emptied" "none" (Invoke-Cli TYPE h2)
    Check "HLEN missing"            "0" (Invoke-Cli HLEN nokey)
    Invoke-Cli SET strh v | Out-Null
    Check "HSET on string"          $WT (Invoke-Cli HSET strh f x)
    Check "HGET on string"          $WT (Invoke-Cli HGET strh f)
    Invoke-Cli LPUSH listh v | Out-Null
    Check "HGET on list"            $WT (Invoke-Cli HGET listh f)

    # ================= [8] 协议（原始 TCP） =================
    Write-Host "===== [8] protocol ====="
    Check-Bytes "inline command"      "+PONG`r`n" (Send-Raw "PING`r`n")
    Check-Bytes "inline lowercase"    "+PONG`r`n" (Send-Raw "ping`r`n")
    Check-Bytes "inline two cmds"     "+OK`r`n`$1`r`n5`r`n" (Send-Raw "SET x 5`r`nGET x`r`n")
    Check-Bytes "skip empty lines"    "+PONG`r`n" (Send-Raw "`r`n`r`nPING`r`n")
    Check-Bytes "RESP PING"           "+PONG`r`n" (Send-Raw "*1`r`n`$4`r`nPING`r`n")
    Check-Bytes "pipelined 3 cmds"    "+OK`r`n`$1`r`n1`r`n:1`r`n" `
        (Send-Raw "*3`r`n`$3`r`nSET`r`n`$1`r`na`r`n`$1`r`n1`r`n*2`r`n`$3`r`nGET`r`n`$1`r`na`r`n*2`r`n`$3`r`nDEL`r`n`$1`r`na`r`n")
    Check-Bytes "fragmented send"     "+OK`r`n`$10`r`nsplitvalue`r`n" `
        (Send-RawChunks @("*3`r`n`$3`r`nSET`r`n`$2`r`n", "pk`r`n`$10`r`nsplit", "value`r`n*2`r`n`$3`r`nGET`r`n`$2`r`npk`r`n"))
    Check-Bytes "malformed *abc"      "-ERR Protocol error`r`n" (Send-Raw "*abc`r`n")
    Check-Bytes "malformed element"   "-ERR Protocol error`r`n" (Send-Raw "*1`r`nx`r`n")
    Check-Bytes "malformed bulk len"  "-ERR Protocol error`r`n" (Send-Raw "*1`r`n`$abc`r`n")
    Check-Bytes "empty array cmd"     "-ERR Protocol error`r`n" (Send-Raw "*0`r`n")
    Check-Bytes "truncated no reply"  "" (Send-Raw "*1`r`n`$4`r`nPI")
    Check "server alive after trunc"  "PONG" (Invoke-Cli PING)
    Check-Bytes "binary value rt"     "+OK`r`n`$6`r`na b`r`nc`r`n" `
        (Send-Raw "*3`r`n`$3`r`nSET`r`n`$2`r`nbk`r`n`$6`r`na b`r`nc`r`n*2`r`n`$3`r`nGET`r`n`$2`r`nbk`r`n")
    $big = "A" * 100000
    # Windows 命令行长度上限 ~32KB，100KB 参数走 --stdin
    "SET big $big" | & $Cli -p $Port --stdin 2>$null | Out-Null
    $actualBig = Invoke-Cli GET big
    Check "GET 100KB len"           "100000" $actualBig.Length
    Check "GET 100KB content"       $big $actualBig
    Check "server still healthy"      "PONG" (Invoke-Cli PING)

    # ================= [9] 客户端 =================
    Write-Host "===== [9] client ====="
    $out = & $Cli -h 127.0.0.1 -p $Port PING 2>$null
    Check "-h option"                 "PONG" (($out | ForEach-Object { $_ }) -join "")
    $pipeOut = ("SET p1 1`nGET p1`nDEL p1" | & $Cli -p $Port --stdin 2>$null | ForEach-Object { $_ }) -join "`n"
    Check "--stdin pipeline"          ((@('OK', '1', '1')) -join "`n") $pipeOut
    'SET qk "hello world"' | & $Cli -p $Port --stdin 2>$null | Out-Null
    Check "quoted arg via --stdin"    "hello world" (Invoke-Cli GET qk)
    Invoke-Cli DEL qk | Out-Null
    try { & $Cli -p 1 PING 2>$null | Out-Null } catch { }
    Check "connect failure exit code" "2" "$LASTEXITCODE"

    # ================= [10] 并发 =================
    Write-Host "===== [10] concurrency ====="
    Invoke-Cli FLUSHALL | Out-Null
    $procs = @()
    for ($i = 1; $i -le 8; $i++) {
        $procs += Start-Process -FilePath $Cli -ArgumentList @("-p", "$Port", "SET", "ck$i", "v$i") `
            -PassThru -NoNewWindow -RedirectStandardOutput (Join-Path $Work "c$i.log")
    }
    $procs | Wait-Process
    $concOk = "1"
    for ($i = 1; $i -le 8; $i++) {
        $v = Invoke-Cli GET "ck$i"
        if ($v -ne "v$i") { $concOk = "0" }
    }
    Check "8 parallel SET/GET"        $concOk "1"
    Check "after concurrency alive"   "PONG" (Invoke-Cli PING)

    # ================= [11] 持久化（周期 dump + 硬杀 + 重启） =================
    Write-Host "===== [11] persistence ====="
    # 先换成 2 秒周期 dump 的实例，再写数据（否则硬杀时数据来不及 dump）
    Stop-Server
    Start-Server -WorkDir $script:Work -Interval "2"
    Invoke-Cli FLUSHALL | Out-Null
    Invoke-Cli SET pk "hello world with spaces" | Out-Null
    $nl = "line1`nline2"
    Invoke-Cli SET nk $nl | Out-Null
    Invoke-Cli LPUSH pl x y | Out-Null
    Invoke-Cli HMSET ph a b c d | Out-Null
    Invoke-Cli SET tk2 soon | Out-Null
    Invoke-Cli EXPIRE tk2 6 | Out-Null

    Check "periodic dump emitted"     "1" $(if (Wait-Log $script:SrvLog "dump success") { "1" } else { "0" })
    Stop-Server

    # 不带 env 重启，加载 dump
    Start-Server -WorkDir $script:Work
    Check "restart loads dump"        "1" $(if (Wait-Log $script:SrvLog "Loaded dump.my_rdb" 3000) { "1" } else { "0" })
    Check "spaces survive"            "hello world with spaces" (Invoke-Cli GET pk)
    Check "newline survives"          $nl (Invoke-Cli GET nk)
    Check "list survives"             ((@('1) "y"', '2) "x"')) -join "`n") (Invoke-Cli LGET pl)
    Check "hash survives"             "b" (Invoke-Cli HGET ph a)
    Check "TTL key still present"     "soon" (Invoke-Cli GET tk2)
    Start-Sleep -Seconds 5
    Check "TTL survives restart"      "(nil)" (Invoke-Cli GET tk2)

    # ================= [12] 进程行为 =================
    Write-Host "===== [12] process behavior ====="
    $bindOut = Join-Path $Work2 "server.err.log"
    $p2 = Start-Process -FilePath $Srv -ArgumentList $Port `
        -WorkingDirectory $Work2 `
        -RedirectStandardOutput (Join-Path $Work2 "server.log") `
        -RedirectStandardError $bindOut `
        -PassThru -NoNewWindow
    $p2.WaitForExit()
    Check "second instance exit code" "1" "$($p2.ExitCode)"
    $bmsg = ""
    if (Test-Path $bindOut) { $bmsg = Get-Content $bindOut -Raw }
    Check "second instance message"   "1" $(if ($bmsg -match "Failed to bind") { "1" } else { "0" })
    Check "first instance unaffected" "PONG" (Invoke-Cli PING)

    $crash = $false
    foreach ($f in @($script:SrvLog, (Join-Path $script:Work "server.err.log"))) {
        if (Test-Path $f) {
            $t = Get-Content $f -Raw -ErrorAction SilentlyContinue
            if ($t -and $t -match "terminate|abort|segmentation|Exception") { $crash = $true }
        }
    }
    Check "no crash in server log"    "1" $(if ($crash) { "0" } else { "1" })
}
finally {
    Stop-Server
    Remove-Item -Recurse -Force $script:Work -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $Work2 -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "=============================="
Write-Host "PASS: $($script:Pass)  FAIL: $($script:Fail)"
if ($script:Fail -gt 0) {
    Write-Host "Failed:"
    foreach ($f in $script:Failed) { Write-Host "  - $f" }
    Write-Host "=============================="
    exit 1
}
Write-Host "ALL TESTS PASSED"
Write-Host "=============================="
exit 0
