# 页面内存回归检查：录一段串口日志，把每个页面的"进入/离开"内存差值列成表。
#
# 用法（先把设备导航到你关心的页面附近）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\mem-check.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\mem-check.ps1 -Seconds 120
#
# 运行期间请按提示进出页面；脚本结束后会输出：
#   - 每个页面每次进出的 internal / largest / psram 变化
#   - 最可疑的项（离开后没有回收的页面）

param(
    [string]$Port = 'COM12',
    [int]$Seconds = 120
)

$repoRoot = Split-Path -Parent $PSScriptRoot
$logPath = Join-Path $repoRoot 'mem_check.log'
$python = 'D:\Espressif\tools\python\v5.5.6\venv\Scripts\python.exe'

if (-not (Test-Path $python)) { Write-Host "找不到 Python：$python" -ForegroundColor Red; exit 1 }

$tmpPy = Join-Path $env:TEMP ("mem_check_{0}.py" -f ([guid]::NewGuid().ToString('N')))
$script = @"
import serial, time, sys
port = r'$Port'
secs = $Seconds
out = r'$logPath'
s = None
for _ in range(60):
    try:
        s = serial.Serial(port, 115200, timeout=0.2); break
    except Exception:
        time.sleep(0.5)
if s is None:
    print('PORT_MISSING'); sys.exit(2)
print('recording %ds ... 请进出页面' % secs, flush=True)
end = time.time() + secs
f = open(out, 'wb')
try:
    while time.time() < end:
        try:
            d = s.read(8192)
        except Exception:
            time.sleep(0.2); continue
        if d:
            f.write(d)
            for line in d.decode('utf-8','replace').splitlines():
                if '[mem]' in line:
                    print(line.strip().encode('ascii','replace').decode('ascii')[:180], flush=True)
finally:
    f.close()
    try: s.close()
    except Exception: pass
print('DONE', flush=True)
"@
[System.IO.File]::WriteAllText($tmpPy, $script, [System.Text.UTF8Encoding]::new($false))
Remove-Item $logPath -ErrorAction SilentlyContinue
& $python -u $tmpPy
Remove-Item $tmpPy -ErrorAction SilentlyContinue

if (-not (Test-Path $logPath)) { Write-Host "没抓到日志" -ForegroundColor Yellow; exit 0 }

Write-Host ""
Write-Host "=== 页面内存差值汇总 ===" -ForegroundColor Cyan
$text = Get-Content $logPath -Raw -Encoding UTF8
$rows = [regex]::Matches($text, '\[mem\] (load|unload) scr=(0x[0-9a-fA-F]+) internal=(\d+)KB\(([+-]?\d+)\) largest=(\d+)KB\(([+-]?\d+)\) psram=(\d+)KB\(([+-]?\d+)\) tasks=(\d+)\(([+-]?\d+)\)')
$table = @{}
foreach ($m in $rows) {
    $phase = $m.Groups[1].Value
    $scr = $m.Groups[2].Value
    if (-not $table.ContainsKey($scr)) { $table[$scr] = @() }
    $table[$scr] += [pscustomobject]@{
        Phase   = $phase
        dInt    = [int]$m.Groups[4].Value
        dLargest= [int]$m.Groups[6].Value
        dPsram  = [int]$m.Groups[8].Value
        dTasks  = [int]$m.Groups[10].Value
    }
}

if ($table.Count -eq 0) {
    Write-Host "日志里没有 [mem] 记录（固件可能还没烧入体检版）" -ForegroundColor Yellow
    exit 0
}

$bad = @()
foreach ($scr in $table.Keys) {
    $items = $table[$scr]
    Write-Host ("屏幕 {0}：" -f $scr) -ForegroundColor White
    foreach ($it in $items) {
        $sign = { param($v) if ($v -ge 0) { "+$v" } else { "$v" } }
        $line = "  {0,-6} internal {1,6} KB   largest {2,6} KB   psram {3,7} KB   tasks {4,4}" -f `
            $it.Phase, (& $sign $it.dInt), (& $sign $it.dLargest), (& $sign $it.dPsram), (& $sign $it.dTasks)
        if ($it.dInt -le -8 -or $it.dPsram -le -1024) { Write-Host $line -ForegroundColor Red } else { Write-Host $line }
    }
    $sumInt = ($items | Measure-Object -Property dInt -Sum).Sum
    $sumPs  = ($items | Measure-Object -Property dPsram -Sum).Sum
    if ($sumInt -le -8 -or $sumPs -le -1024) { $bad += [pscustomobject]@{ Screen = $scr; NetInternalKB = $sumInt; NetPsramKB = $sumPs } }
}

Write-Host ""
if ($bad.Count -gt 0) {
    Write-Host "=== 可疑：这些页面来回之后没有把内存还回来 ===" -ForegroundColor Red
    $bad | ForEach-Object { Write-Host ("  屏幕 {0}: 内部净减 {1} KB，PSRAM 净减 {2} KB" -f $_.Screen, (0 - $_.NetInternalKB), (0 - $_.NetPsramKB)) }
} else {
    Write-Host "=== 没有发现明显泄漏（单页净减 < 8KB 且 PSRAM 净减 < 1MB）===" -ForegroundColor Green
}
Write-Host ""
Write-Host "完整日志：$logPath" -ForegroundColor Cyan
