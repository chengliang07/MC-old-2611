# 抓崩溃：录制串口 → 检测 panic → 自动把 Backtrace 翻译成函数名和行号。
#
# 前提：固件用"崩溃停住"模式编译（CONFIG_ESP_SYSTEM_PANIC_PRINT_HALT=y，
#       本地 sdkconfig 已开启），否则设备会重启、截断日志。
#
# 用法：
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\capture-crash.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\capture-crash.ps1 -Seconds 180
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\capture-crash.ps1 -Reset   # 先复位设备

param(
    [string]$Port = 'COM12',
    [int]$Seconds = 150,
    [switch]$Reset
)

$repoRoot = Split-Path -Parent $PSScriptRoot
$elf = Join-Path $repoRoot 'build\xiaozhi.elf'
$logPath = Join-Path $repoRoot 'crash_capture.log'
$python = 'D:\Espressif\tools\python\v5.5.6\venv\Scripts\python.exe'
$addr2line = 'D:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin\riscv32-esp-elf-addr2line.exe'

if (-not (Test-Path $python)) { Write-Host "找不到 Python：$python" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $elf)) { Write-Host "找不到符号表 build\xiaozhi.elf，请先编译" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $addr2line)) { Write-Host "找不到 addr2line：$addr2line" -ForegroundColor Red; exit 1 }

if ($Reset) {
    Write-Host "[crash] 复位设备..." -ForegroundColor Cyan
    & $python -m esptool --chip esp32p4 --port $Port --before default_reset --after no_reset --no-stub run 2>$null | Out-Null
    Start-Sleep -Milliseconds 500
}

$tmpPy = Join-Path $env:TEMP ("crash_capture_{0}.py" -f ([guid]::NewGuid().ToString('N')))
$script = @"
import serial, time, sys
port = r'$Port'
secs = $Seconds
out = r'$logPath'
try:
    s = serial.Serial(port, 115200, timeout=0.2)
except Exception as e:
    print('OPEN_FAILED:', repr(e), flush=True)
    sys.exit(2)
buf = b''
end = time.time() + secs
print('capturing %ds on %s ...' % (secs, port), flush=True)
try:
    while time.time() < end:
        try:
            d = s.read(8192)
        except Exception:
            time.sleep(0.3)
            continue
        if d:
            buf += d
            open(out, 'ab').write(d)
            for line in d.decode('utf-8', 'replace').splitlines():
                l = line.strip()
                if l and not l.startswith('I ('):
                    print(l.encode('ascii', 'replace').decode('ascii')[:200], flush=True)
            if b'Backtrace:' in d and b'Guru' not in d:
                pass
finally:
    try: s.close()
    except Exception: pass
print('DONE bytes=%d' % len(buf), flush=True)
"@
[System.IO.File]::WriteAllText($tmpPy, $script, [System.Text.UTF8Encoding]::new($false))
Remove-Item $logPath -ErrorAction SilentlyContinue
& $python -u $tmpPy 2>&1 | Select-Object -Last 200
Remove-Item $tmpPy -ErrorAction SilentlyContinue

Write-Host ''
Write-Host "[crash] 日志：$logPath" -ForegroundColor Cyan
if (-not (Test-Path $logPath)) { Write-Host "没有捕获到日志" -ForegroundColor Yellow; exit 0 }

$text = Get-Content $logPath -Raw -Encoding UTF8
$bt = [regex]::Matches($text, 'Backtrace:\s*((?:0x[0-9a-fA-F]{8}:0x[0-9a-fA-F]{8}\s*)+)')
if ($bt.Count -eq 0) {
    Write-Host "[crash] 没有找到 Backtrace，可能没崩（或用的是重启模式）" -ForegroundColor Yellow
    $core = [regex]::Match($text, 'Guru Meditation[^\r\n]*')
    if ($core.Success) { Write-Host "  但发现：$($core.Value)" -ForegroundColor Yellow }
    exit 0
}

$frames = $bt[$bt.Count - 1].Groups[1].Value.Trim() -split '\s+'
$addrs = @()
foreach ($f in $frames) {
    $pc = ($f -split ':')[0]
    if ($pc -match '^0x[0-9a-fA-F]{8}$' -and $pc -ne '0x00000000') { $addrs += $pc }
}

Write-Host ''
Write-Host "[crash] 崩溃位置（由新到旧）：" -ForegroundColor Green
if ($addrs.Count -eq 0) { Write-Host "  没有可解析的地址" -ForegroundColor Yellow; exit 0 }
& $addr2line -pfiaC -e $elf @addrs

Write-Host ''
Write-Host "[crash] reset 原因：" -ForegroundColor Green
$reset = [regex]::Match($text, '@@RESET@@[^\r\n]*')
if ($reset.Success) { Write-Host "  $($reset.Value)" }
$core = [regex]::Match($text, 'Guru Meditation[^\r\n]*')
if ($core.Success) { Write-Host "  $($core.Value)" -ForegroundColor Red }
