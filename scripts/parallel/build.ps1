<#
.SYNOPSIS
  Serialized build gate for metalio-claw4: only one window builds at a time.

.DESCRIPTION
  The ESP-IDF build/ directory is NOT safe for concurrent ninja invocations
  (clashing CMakeCache.txt removal, locked mmap_build resources). Every window
  must build through this script; the file lock serializes access.

  Environment matches AGENTS.md "Verified local build" exactly.

.EXAMPLE
  ./scripts/parallel/build.ps1 -Owner oral
  ./scripts/parallel/build.ps1 -Owner integration -Mode fullclean
#>
param(
    [string]$Owner = "unknown",
    [ValidateSet("build", "fullclean")][string]$Mode = "build",
    [int]$LockTimeoutSec = 3600,
    # validates lock + env only, does not build (smoke test)
    [switch]$DryRun
)

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root

$py = 'D:\Espressif\tools\python\v5.5.6\venv\Scripts\python.exe'
$idfPy = 'D:\esp\v5.5.6\esp-idf\tools\idf.py'
# 锁与日志放在仓库根的 .gates/（build/ 在部分权限环境下不可写）
$stateDir = Join-Path $root ".gates"
$logDir = Join-Path $stateDir "logs"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$log = Join-Path $logDir "build_${stamp}_$Owner.log"

# ---------- build lock (directory lock: atomic mkdir, works in any shell) ----------
$gateDir = Join-Path $stateDir "build.lock"
$deadline = (Get-Date).AddSeconds($LockTimeoutSec)
$got = $false
$waited = $false
while (-not $got) {
    if (-not (Test-Path $gateDir)) {
        try { New-Item -ItemType Directory -Path $gateDir -ErrorAction Stop | Out-Null; $got = $true; break } catch { }
    }
    if ((Test-Path $gateDir) -and ((Get-Date) - (Get-Item $gateDir).LastWriteTime).TotalHours -gt 6) {
        Remove-Item $gateDir -Recurse -Force -ErrorAction SilentlyContinue
        continue
    }
    if (-not $waited) { Write-Host "[gate] another window is building, queued..."; $waited = $true }
    if ((Get-Date) -gt $deadline) {
        Write-Host "[gate] lock timeout ${LockTimeoutSec}s. If the holder crashed, remove $gateDir"
        exit 3
    }
    Start-Sleep -Seconds 10
}
Set-Content -Path (Join-Path $gateDir "owner.txt") -Encoding ASCII `
    -Value "owner=$Owner pid=$PID mode=$Mode start=$(Get-Date -Format o)"

$code = 0
try {
    Write-Host "[gate] build lock acquired owner=$Owner mode=$Mode"
    Write-Host "[gate] log: $log"

    $env:IDF_PATH = 'D:\esp\v5.5.6\esp-idf'
    $env:IDF_TOOLS_PATH = 'D:\Espressif'
    $env:IDF_PYTHON_ENV_PATH = 'D:\Espressif\tools\python\v5.5.6\venv'
    $env:ESP_IDF_VERSION = '5.5'
    $env:ESP_ROM_ELF_DIR = 'D:\Espressif\tools\esp-rom-elfs\20260528\'
    $env:IDF_PYTHON_CHECK_CONSTRAINTS = '0'
    $env:PYTHONUTF8 = '1'
    $env:PYTHONIOENCODING = 'utf-8'
    Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
    . 'D:\esp\v5.5.6\esp-idf\export.ps1' | Out-Null

    if ($DryRun) {
        $tool = Get-Command riscv32-esp-elf-gcc -ErrorAction SilentlyContinue
        Write-Host ("[gate] DRY RUN ok: gcc={0}" -f ($(if ($tool) { $tool.Source } else { 'MISSING' })))
        Write-Host ("[gate] IDF_PATH={0} ESP_IDF_VERSION={1}" -f $env:IDF_PATH, $env:ESP_IDF_VERSION)
        exit 0
    }

    if ($Mode -eq 'fullclean') {
        Write-Host "[gate] fullclean then full build (12-20 min)"
        & $py $idfPy -DCMAKE_NINJA_FORCE_RESPONSE_FILE=ON fullclean *> $log
    }

    $sw = [Diagnostics.Stopwatch]::StartNew()
    & $py $idfPy -DCMAKE_NINJA_FORCE_RESPONSE_FILE=ON build *>> $log
    $code = $LASTEXITCODE
    $sw.Stop()

    if ($code -eq 0) {
        Write-Host ("[gate] BUILD OK  {0:N0}s  ->  {1}" -f $sw.Elapsed.TotalSeconds, (Join-Path $root 'build\xiaozhi.bin'))
    } else {
        Write-Host ("[gate] BUILD FAILED exit={0}  {1:N0}s" -f $code, $sw.Elapsed.TotalSeconds)
        Write-Host "[gate] errors (full log: $log):"
        Select-String -Path $log -Pattern "error:|FAILED:|ninja: build stopped|CMake Error" -Encoding UTF8 |
            Select-Object -First 15 | ForEach-Object { "  " + $_.Line }
    }
} finally {
    Remove-Item $gateDir -Recurse -Force -ErrorAction SilentlyContinue
}

exit $code
