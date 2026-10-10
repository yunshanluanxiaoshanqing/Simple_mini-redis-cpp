<#
    run_tests.ps1 — mini-redis v2 集成测试（Windows / PowerShell 7+）

    与 02-fixed 的 tests/run_tests.sh 相比：
      * 不依赖 nc（WSL/Netcat），Windows 原生可跑
      * 断言覆盖：协议边界（粘包/半包/大报文/协议错误）、类型不变量、
        过期语义、持久化（含二进制安全）、优雅退出与退出码
      * 每条用例独立记录 name/expected/actual，失败时给出可复制的现场

    用法:
        pwsh -File tests/run_tests.ps1 [-Port 7507] [-KeepDump]
#>
[CmdletBinding()]
param(
    [int]$Port = 7607,
    [switch]$KeepDump
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'
$server = Join-Path $build 'v2-server.exe'
$cli = Join-Path $build 'v2-cli.exe'
if (-not (Test-Path $server)) { $server = Join-Path $build 'v2-server' }
if (-not (Test-Path $cli)) { $cli = Join-Path $build 'v2-cli' }
$dump = Join-Path $root 'test-dump.rdb'
$log = Join-Path $root 'test-server.log'

if (-not (Test-Path $server)) {
    Write-Error "找不到服务器可执行文件，请先构建：cmake -S . -B build -G 'MinGW Makefiles' && cmake --build build -j"
}

$script:pass = 0
$script:fail = 0
$script:failures = @()

function Assert-Equal {
    param([string]$Name, [string]$Expected, [string]$Actual)
    if ($Expected -eq $Actual) {
        $script:pass++
        Write-Host ("PASS  {0}" -f $Name) -ForegroundColor DarkGreen
    } else {
        $script:fail++
        $script:failures += $Name
        Write-Host ("FAIL  {0}`n      expected: [{1}]`n      actual:   [{2}]" -f $Name, $Expected, $Actual) -ForegroundColor Red
    }
}

function Assert-True {
    param([string]$Name, [bool]$Value)
    Assert-Equal -Name $Name -Expected 'True' -Actual ($Value.ToString())
}

function Assert-Contains {
    param([string]$Name, [string]$Needle, [string]$Haystack)
    Assert-True -Name $Name -Value ($Haystack -like "*$Needle*")
}

# ---------- 与服务器的两种交互方式 ----------

# 通过命令行的 --stdin 模式发送一条命令：既避免 PowerShell 数组 splat 的歧义，
# 也顺带验证了客户端的流水线发送路径。
function Invoke-Cli {
    param([string[]]$Command)
    $line = ($Command | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
    $out = $line | & $cli -p $Port --stdin 2>&1
    return ($out | Out-String).Trim()
}

function Test-Ping {
    try {
        $old = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        $result = (& $cli -p $Port -c PING 2>$null | Out-String).Trim()
        $ErrorActionPreference = $old
        return $result -eq 'PONG'
    } catch {
        return $false
    }
}

# 原生 socket 交互：用于发送"非法/不完整"的字节流
function Send-Raw {
    param([byte[]]$Bytes, [int]$ReadTimeoutMs = 1500, [switch]$NoRead)
    $client = [System.Net.Sockets.TcpClient]::new('127.0.0.1', $Port)
    try {
        $client.ReceiveTimeout = $ReadTimeoutMs
        $stream = $client.GetStream()
        $stream.Write($Bytes, 0, $Bytes.Length)
        $stream.Flush()
        if ($NoRead) { return '' }
        $buffer = New-Object byte[] 65536
        $builder = [System.Text.StringBuilder]::new()
        $deadline = (Get-Date).AddMilliseconds($ReadTimeoutMs)
        while ((Get-Date) -lt $deadline) {
            try {
                if (-not $stream.DataAvailable) { Start-Sleep -Milliseconds 20; continue }
                $n = $stream.Read($buffer, 0, $buffer.Length)
                if ($n -le 0) { break }
                [void]$builder.Append([System.Text.Encoding]::UTF8.GetString($buffer, 0, $n))
            } catch { break }
        }
        return $builder.ToString()
    } finally {
        $client.Close()
    }
}

function ConvertTo-Resp {
    param([string[]]$Command)
    $text = "*$($Command.Count)`r`n"
    foreach ($a in $Command) { $text += "`$$($a.Length)`r`n$a`r`n" }
    return [System.Text.Encoding]::UTF8.GetBytes($text)
}

# 启动进程并取回 (退出码, 标准输出, 标准错误)。
# 用 System.Diagnostics.Process 而不是 Start-Process：在 Windows PowerShell 7
# 上 Start-Process -PassThru 的 ExitCode 有时取不到（返回 null）。
function Invoke-Executable {
    param(
        [string]$FilePath,
        [string[]]$Arguments = @(),
        [string]$StdOutFile,
        [string]$StdErrFile
    )
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $FilePath
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.WorkingDirectory = $root
    foreach ($a in $Arguments) { [void]$psi.ArgumentList.Add($a) }
    $proc = [System.Diagnostics.Process]::Start($psi)
    $stdout = $proc.StandardOutput.ReadToEnd()
    $stderr = $proc.StandardError.ReadToEnd()
    [void]$proc.WaitForExit()
    if ($StdOutFile) { Set-Content -Path $StdOutFile -Value $stdout -Encoding UTF8 }
    if ($StdErrFile) { Set-Content -Path $StdErrFile -Value $stderr -Encoding UTF8 }
    return @{ ExitCode = $proc.ExitCode; StdOut = $stdout; StdErr = $stderr }
}

function Wait-Server {
    for ($i = 0; $i -lt 60; $i++) {
        if (Test-Ping) { return $true }
        Start-Sleep -Milliseconds 100
    }
    return $false
}

# 等待进程结束并取回退出码；取不到时返回 -999（便于断言失败而非脚本崩溃）。
function Wait-ExitCode {
    param($Process)
    if ($null -eq $Process) { return -999 }
    try {
        if (-not $Process.WaitForExit(10000)) { return -998 }
        $Process.Refresh()
        if ($null -eq $Process.ExitCode) { return -997 }
        return [int]$Process.ExitCode
    } catch {
        return -996
    }
}

# ---------- 启动/停止 ----------

if (Test-Path $dump) { Remove-Item $dump -Force }
if (Test-Path "$dump.tmp") { Remove-Item "$dump.tmp" -Force }

$serverProc = Start-Process -FilePath $server `
    -ArgumentList @('--port', $Port, '--dump', $dump) `
    -PassThru -RedirectStandardOutput $log -RedirectStandardError "$log.err" `
    -WorkingDirectory $root

try {
    if (-not (Wait-Server)) {
        Write-Host '服务器启动失败，日志：' -ForegroundColor Red
        Get-Content $log -ErrorAction SilentlyContinue
        throw 'server failed to start'
    }

    Write-Host "`n=== v2 集成测试（端口 $Port）===" -ForegroundColor Cyan

    # ---------- 基础命令 ----------
    Assert-Equal 'PING' 'PONG' (Invoke-Cli @('PING'))
    Assert-Equal 'PING msg' 'hey' (Invoke-Cli @('PING', 'hey'))
    Assert-Equal 'ECHO' 'hello world' (Invoke-Cli @('ECHO', 'hello world'))
    Assert-Equal 'SET' 'OK' (Invoke-Cli @('SET', 'k', 'v'))
    Assert-Equal 'GET' 'v' (Invoke-Cli @('GET', 'k'))
    Assert-Equal 'GET 不存在' '(nil)' (Invoke-Cli @('GET', 'nope'))
    Assert-Equal 'TYPE string' 'string' (Invoke-Cli @('TYPE', 'k'))
    Assert-Equal 'EXISTS' '1' (Invoke-Cli @('EXISTS', 'k'))
    Assert-Equal 'DBSIZE' '1' (Invoke-Cli @('DBSIZE'))
    Assert-Equal 'DEL' '1' (Invoke-Cli @('DEL', 'k'))
    Assert-Equal 'DEL 二次' '0' (Invoke-Cli @('DEL', 'k'))
    Assert-Contains '未知命令带命令名' "unknown command 'bogus'" (Invoke-Cli @('BOGUS'))
    Assert-Contains '参数个数错误小写命令名' "for 'get' command" (Invoke-Cli @('GET'))

    # ---------- 二进制安全的值 ----------
    Assert-Equal 'SET 含空格与换行' 'OK' (Invoke-Cli @('SET', 'sp', 'a b`tc'))
    Assert-Equal 'GET 含空格与换行' 'a b`tc' (Invoke-Cli @('GET', 'sp'))

    # ---------- 列表 ----------
    Assert-Equal 'RPUSH' '3' (Invoke-Cli @('RPUSH', 'l', 'a', 'b', 'c'))
    Assert-Equal 'LLEN' '3' (Invoke-Cli @('LLEN', 'l'))
    Assert-Equal 'LINDEX 0' 'a' (Invoke-Cli @('LINDEX', 'l', '0'))
    Assert-Equal 'LINDEX -1' 'c' (Invoke-Cli @('LINDEX', 'l', '-1'))
    Assert-Equal 'LINDEX 越界' '(nil)' (Invoke-Cli @('LINDEX', 'l', '9'))
    Assert-Equal 'LPOP' 'a' (Invoke-Cli @('LPOP', 'l'))
    Assert-Contains 'LRANGE' '"b"' (Invoke-Cli @('LRANGE', 'l', '0', '-1'))
    Assert-Equal 'LPUSH 打到 string 报 WRONGTYPE' $true ((Invoke-Cli @('LPUSH', 'sp', 'x')) -like '*WRONGTYPE*')

    # ---------- 哈希 ----------
    Assert-Equal 'HSET 新建' '1' (Invoke-Cli @('HSET', 'h', 'f1', 'v1'))
    Assert-Equal 'HSET 更新' '0' (Invoke-Cli @('HSET', 'h', 'f1', 'v2'))
    Assert-Equal 'HGET' 'v2' (Invoke-Cli @('HGET', 'h', 'f1'))
    Assert-Equal 'HEXISTS' '1' (Invoke-Cli @('HEXISTS', 'h', 'f1'))
    Assert-Equal 'HMSET' 'OK' (Invoke-Cli @('HMSET', 'h', 'f2', 'x'))
    Assert-Equal 'HLEN' '2' (Invoke-Cli @('HLEN', 'h'))
    Assert-Contains 'HGETALL 含字段' '"f2"' (Invoke-Cli @('HGETALL', 'h'))
    Assert-Equal 'HDEL' '1' (Invoke-Cli @('HDEL', 'h', 'f2'))

    # ---------- 过期 ----------
    Assert-Equal 'SET EX' 'OK' (Invoke-Cli @('SET', 'ttl-key', 'v', 'EX', '100'))
    $ttl = [int](Invoke-Cli @('TTL', 'ttl-key'))
    Assert-True 'TTL 在 1..100' ($ttl -gt 0 -and $ttl -le 100)
    Assert-Equal 'PERSIST' '1' (Invoke-Cli @('PERSIST', 'ttl-key'))
    Assert-Equal 'PERSIST 后 TTL=-1' '-1' (Invoke-Cli @('TTL', 'ttl-key'))
    Assert-Equal 'TTL 不存在 key' '-2' (Invoke-Cli @('TTL', 'ghost'))
    Assert-Equal 'EXPIRE 非正数立即删除' '1' (Invoke-Cli @('EXPIRE', 'ttl-key', '0'))
    Assert-Equal '立即删除后不存在' '0' (Invoke-Cli @('EXISTS', 'ttl-key'))

    # ---------- 协议边界（原生 socket） ----------
    # 粘包：两条命令一次发出，服务端必须两条都处理
    $pipeline = [System.Text.Encoding]::UTF8.GetString((ConvertTo-Resp @('SET', 'p1', 'v1'))) +
                [System.Text.Encoding]::UTF8.GetString((ConvertTo-Resp @('GET', 'p1')))
    $resp = Send-Raw -Bytes ([System.Text.Encoding]::UTF8.GetBytes($pipeline))
    Assert-Equal '粘包-两条命令都处理' "+OK`r`n`$2`r`nv1`r`n" $resp

    # 半包：逐字节发送
    $client = [System.Net.Sockets.TcpClient]::new('127.0.0.1', $Port)
    try {
        $client.ReceiveTimeout = 1500
        $stream = $client.GetStream()
        $bytes = ConvertTo-Resp @('SET', 'p2', 'v2')
        foreach ($b in $bytes) { $stream.WriteByte($b); Start-Sleep -Milliseconds 2 }
        $stream.Flush()
        Start-Sleep -Milliseconds 150
        $buffer = New-Object byte[] 256
        $n = $stream.Read($buffer, 0, 256)
        Assert-Equal '半包-逐字节最终成功' '+OK' ([System.Text.Encoding]::UTF8.GetString($buffer, 0, $n).Trim())
    } finally { $client.Close() }

    # 大报文：远超原实现 1023 字节的截断边界
    $big = 'x' * 4000
    Assert-Equal '大值 (>1KB) SET' 'OK' (Invoke-Cli @('SET', 'big', $big))
    Assert-Equal '大值 GET 完整' $big (Invoke-Cli @('GET', 'big'))

    # 内联命令
    $inline = Send-Raw -Bytes ([System.Text.Encoding]::UTF8.GetBytes("PING`r`n"))
    Assert-Equal '内联命令' "+PONG`r`n" $inline

    # 协议错误：非法 multibulk 长度
    $badResp = Send-Raw -Bytes ([System.Text.Encoding]::UTF8.GetBytes("*abc`r`n"))
    Assert-Contains '非法 multibulk 返回协议错误' 'Protocol error' $badResp

    # 协议错误：声明长度超过上限
    $badResp2 = Send-Raw -Bytes ([System.Text.Encoding]::UTF8.GetBytes("*1`r`n`$70000000`r`n"))
    Assert-Contains '超大 bulk 长度被拒绝' 'Protocol error' $badResp2

    # ---------- KEYS 通配符 ----------
    Invoke-Cli @('FLUSHALL') | Out-Null
    Invoke-Cli @('SET', 'a1', '1') | Out-Null
    Invoke-Cli @('SET', 'a2', '2') | Out-Null
    Invoke-Cli @('SET', 'b1', '3') | Out-Null
    $akeys = (Invoke-Cli @('KEYS', 'a*'))
    Assert-Contains 'KEYS a* 含 a1' '"a1"' $akeys
    Assert-Contains 'KEYS a* 含 a2' '"a2"' $akeys
    Assert-True 'KEYS a* 不含 b1' (-not ($akeys -like '*b1*'))
    $bkeys = (Invoke-Cli @('KEYS', 'b*'))
    Assert-Contains 'KEYS b* 只含 b1' '"b1"' $bkeys
    Assert-True 'KEYS b* 不含 a1' (-not ($bkeys -like '*a1*'))
    $qkeys = (Invoke-Cli @('KEYS', 'a??'))
    Assert-True 'KEYS a?? 无匹配' (-not ($qkeys -like '*a1*'))

    # ---------- 优雅退出与持久化 ----------
    Invoke-Cli @('FLUSHALL') | Out-Null
    Assert-Equal 'SHUTDOWN 前写入' 'OK' (Invoke-Cli @('SET', 'persist me', 'value with spaces'))
    Assert-Equal 'SHUTDOWN' 'OK' (Invoke-Cli @('SHUTDOWN'))
    $exited = $serverProc.WaitForExit(5000)
    Assert-True 'SHUTDOWN 后进程真的退出' $exited
    Assert-True '退出时写出 dump 文件' (Test-Path $dump)

    # 重启并验证恢复
    $serverProc = Start-Process -FilePath $server `
        -ArgumentList @('--port', $Port, '--dump', $dump) `
        -PassThru -RedirectStandardOutput $log -RedirectStandardError "$log.err" `
        -WorkingDirectory $root
    if (-not (Wait-Server)) { throw 'server failed to restart' }
    Assert-Equal '重启后值恢复（含空格）' 'value with spaces' (Invoke-Cli @('GET', 'persist me'))


    # ---------- v2 特有：内存上限 / INFO ----------
    Assert-Equal 'MAXMEMORY 默认 0' '0' (Invoke-Cli @('MAXMEMORY'))
    Assert-Equal 'MAXMEMORY 设置' 'OK' (Invoke-Cli @('MAXMEMORY', '1000000'))
    Assert-Equal 'MAXMEMORY 查询' '1000000' (Invoke-Cli @('MAXMEMORY'))
    Assert-Equal 'MAXMEMORY 非法值' 'ERR value is not an integer or out of range' `
        ((Invoke-Cli @('MAXMEMORY', 'abc')) -replace '^\(error\) ', '')
    $info = Invoke-Cli @('INFO')
    Assert-Contains 'INFO 含 version' 'version:mini-redis 2.0' $info
    Assert-Contains 'INFO 含 dump_format' 'dump_format:MINIRDB2+crc32' $info
    Assert-Contains 'INFO 含 used_memory' 'used_memory:' $info

    # ---------- v2 特有：CRC32 完整性校验 ----------
    Assert-Equal '写入用于 CRC 测试的数据' 'OK' (Invoke-Cli @('SET', 'crc-probe', 'value'))
    Assert-Equal '触发落盘' 'OK' (Invoke-Cli @('SHUTDOWN'))
    [void]$serverProc.WaitForExit(5000)
    Assert-True 'dump 文件已生成' (Test-Path $dump)
    $bytes = [System.IO.File]::ReadAllBytes($dump)
    $bytes[[int]($bytes.Length / 2)] = $bytes[[int]($bytes.Length / 2)] -bxor 0x01
    [System.IO.File]::WriteAllBytes($dump, $bytes)
    $serverProc = Start-Process -FilePath $server `
        -ArgumentList @('--port', $Port, '--dump', $dump) `
        -PassThru -RedirectStandardOutput $log -RedirectStandardError "$log.err" `
        -WorkingDirectory $root
    if (-not (Wait-Server)) { throw 'server failed to restart after corruption' }
    Assert-Equal 'CRC 损坏后启动为空库' '0' (Invoke-Cli @('DBSIZE'))
    Assert-Contains '日志说明完整性校验失败' 'failed integrity check' (Get-Content "$log.err" -Raw)
    # ---------- 退出码：端口占用 ----------
    $conflict = Invoke-Executable -FilePath $server -Arguments @('--port', $Port) `
        -StdOutFile "$log.conflict" -StdErrFile "$log.conflict.err"
    Assert-Equal '端口被占用时退出码非 0' '1' $conflict.ExitCode.ToString()
    Assert-Contains '端口占用时打印 bind 失败' 'failed to bind' $conflict.StdErr

    # ---------- 退出码：非法参数 ----------
    $badPort = Invoke-Executable -FilePath $server -Arguments @('--port', '99999') `
        -StdOutFile "$log.badport" -StdErrFile "$log.badport.err"
    Assert-Equal '非法端口退出码非 0' '1' $badPort.ExitCode.ToString()
    Assert-Contains '非法端口给出用法提示' 'Invalid port' $badPort.StdErr
}
finally {
    if ($serverProc -and -not $serverProc.HasExited) {
        Invoke-Cli @('SHUTDOWN') | Out-Null
        if (-not $serverProc.WaitForExit(3000)) { $serverProc.Kill() }
    }
    foreach ($extra in @($dump, "$dump.tmp", $log, "$log.err", "$log.conflict",
                         "$log.conflict.err", "$log.badport", "$log.badport.err")) {
        if ((Test-Path $extra) -and -not $KeepDump) { Remove-Item $extra -Force -ErrorAction SilentlyContinue }
    }
}

Write-Host "`n=== 结果：$script:pass 通过 / $script:fail 失败 ===" -ForegroundColor Cyan
if ($script:fail -gt 0) {
    Write-Host ("失败用例：" + ($script:failures -join ', ')) -ForegroundColor Red
    exit 1
}
exit 0
