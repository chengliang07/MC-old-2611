# 唯一允许烧录的入口：抢锁 → 编译 → 烧录 → 解锁。
#
# 为什么要锁：一台设备只有一个串口，两个窗口同时烧会互相把对方"踢下线"，
# 表现为 "Could not open COMxx: PermissionError" 或烧到一半失败。
#
# 用法（在任何 worktree 里都可以，锁是全局的）：
#   pwsh -File scripts/flash-device.ps1                 # 默认 COM12，先增量编译
#   pwsh -File scripts/flash-device.ps1 -Port COM5      # 换串口
#   pwsh -File scripts/flash-device.ps1 -SkipBuild      # 已经编译过，直接烧
#   pwsh -File scripts/flash-device.ps1 -Force           # 抢锁（确认另一个窗口没在烧）

param(
    [string]$Port = 'COM12',
    [int]$Baud = 460800,
    [switch]$SkipBuild,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$lockRoot = Split-Path -Parent $repoRoot
$lockPath = Join-Path $lockRoot '.flash.lock'

$idfPath = 'D:\esp\v5.5.6\esp-idf'
$idfTools = 'D:\Espressif'
$python = 'D:\Espressif\tools\python\v5.5.6\venv\Scripts\python.exe'

function Write-Step($text) { Write-Host "[flash] $text" -ForegroundColor Cyan }
function Write-Warn($text) { Write-Host "[flash] $text" -ForegroundColor Yellow }

if (-not (Test-Path $python)) {
    throw "找不到 Python：$python"
}

# ---- 抢锁 ----------------------------------------------------------------
$lockStream = $null
try {
    if ($Force -and (Test-Path $lockPath)) {
        Write-Warn "强制抢锁：删除 $lockPath"
        Remove-Item $lockPath -Force
    }
    $lockStream = [System.IO.File]::Open($lockPath, 'CreateNew', 'Write', 'None')
    $info = @{
        pid      = $PID
        repo     = $repoRoot
        port     = $Port
        startedAt = (Get-Date).ToString('s')
        user     = $env:USERNAME
    } | ConvertTo-Json -Compress
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($info)
    $lockStream.Write($bytes, 0, $bytes.Length)
    $lockStream.Flush()
} catch {
    $holder = ''
    if (Test-Path $lockPath) {
        try { $holder = Get-Content $lockPath -Raw } catch { }
    }
    Write-Host ''
    Write-Host '设备正在被另一个窗口使用。' -ForegroundColor Red
    Write-Host "  锁文件：$lockPath"
    Write-Host "  占用者：$holder"
    Write-Host '  确认没有别的窗口在烧录时，加 -Force 重试。'
    Write-Host ''
    exit 2
}

try {
    Write-Step "已获得烧录锁：$lockPath"

    # 统一从 idf-env.ps1 取环境（含 ccache 配置）
    . (Join-Path $PSScriptRoot 'idf-env.ps1')

    Push-Location $repoRoot
    try {
        $rev = (git rev-parse --short HEAD) 2>$null
        $dirty = (git status --porcelain) 2>$null
        Write-Step "代码版本：$rev$(if ($dirty) { '（工作区有未提交改动）' } else { '' })"

        if (-not $SkipBuild) {
            Write-Step '增量编译中...'
            & $python -u "$idfPath\tools\idf.py" -DCMAKE_NINJA_FORCE_RESPONSE_FILE=ON -DCCACHE_ENABLE=ON build 2>&1 |
                Tee-Object -FilePath (Join-Path $repoRoot 'flash_build.log') | Out-Null
            if ($LASTEXITCODE -ne 0) {
                Write-Host '[flash] 编译失败，日志：flash_build.log' -ForegroundColor Red; exit 1
            }
            Write-Step '编译完成'
        }

        $binDir = Join-Path $repoRoot 'build'
        if (-not (Test-Path (Join-Path $binDir 'flash_args'))) {
            Write-Host '[flash] 找不到 build\flash_args，请先完整编译一次' -ForegroundColor Red; exit 1
        }

        Write-Step "烧录到 $Port ..."
        Push-Location $binDir
        try {
            & $python -m esptool --chip esp32p4 --port $Port --baud $Baud write_flash '@flash_args' 2>&1 |
                Tee-Object -FilePath (Join-Path $repoRoot 'flash_device.log') | Out-Null
            $code = $LASTEXITCODE
        } finally {
            Pop-Location
        }

        $verified = (Select-String -Path (Join-Path $repoRoot 'flash_device.log') -Pattern 'Hash of data verified' -ErrorAction SilentlyContinue).Count
        if ($code -ne 0) {
            Write-Host ("[flash] 烧录失败（退出码 " + $code + "），日志：flash_device.log") -ForegroundColor Red; exit 1
        }
        Write-Step "烧录完成，校验 $verified/7 个分区"
    } finally {
        Pop-Location
    }
} finally {
    if ($lockStream) {
        try { $lockStream.Close(); $lockStream.Dispose() } catch { }
    }
    if (Test-Path $lockPath) {
        try { Remove-Item $lockPath -Force } catch { }
    }
    Write-Step '锁已释放'
}
