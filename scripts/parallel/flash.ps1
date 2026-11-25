<#
.SYNOPSIS
  Serialized flash gate: only one window owns the serial port at a time.

.DESCRIPTION
  There is a single flashing port (COM12). Concurrent flashes always fail, and
  USB-Serial-JTAG re-enumeration produces PermissionError(13) /
  FileNotFoundError(2) while resetting. This script serializes access and
  retries until the port settles.

  Modes:
    -Mode app     (default) flash bootloader/app/assets per build/flash_args
    -Mode merged  (release) write one merged image at 0x0

.EXAMPLE
  ./scripts/parallel/flash.ps1 -Owner integration
  ./scripts/parallel/flash.ps1 -Owner integration -Mode merged -Image build_artifacts/Metalio_Claw4_2.0.142.bin
#>
param(
    [string]$Owner = "unknown",
    [string]$Port = "COM12",
    [int]$Baud = 921600,
    [ValidateSet("app", "merged")][string]$Mode = "app",
    [string]$Image = "",
    [int]$Retries = 12,
    [int]$LockTimeoutSec = 900
)

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root

$py = 'D:\Espressif\tools\python\v5.5.6\venv\Scripts\python.exe'
# 锁与日志放在仓库根的 .gates/（build/ 在部分权限环境下不可写）
$stateDir = Join-Path $root ".gates"
$logDir = Join-Path $stateDir "logs"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$log = Join-Path $logDir "flash_${stamp}_$Owner.log"

if ($Mode -eq 'merged') {
    if ([string]::IsNullOrWhiteSpace($Image)) { Write-Host "[flash] -Mode merged requires -Image"; exit 2 }
    if (-not (Test-Path $Image)) { Write-Host "[flash] image not found: $Image"; exit 2 }
    $Image = (Resolve-Path $Image).Path
}
if ($Mode -eq 'app' -and -not (Test-Path (Join-Path $root 'build\flash_args'))) {
    Write-Host "[flash] build/flash_args missing, build first"
    exit 2
}

# ---------- flash lock (directory lock: atomic mkdir) ----------
$gateDir = Join-Path $stateDir "flash.lock"
$deadline = (Get-Date).AddSeconds($LockTimeoutSec)
$got = $false
$waited = $false
while (-not $got) {
    if (-not (Test-Path $gateDir)) {
        try { New-Item -ItemType Directory -Path $gateDir -ErrorAction Stop | Out-Null; $got = $true; break } catch { }
    }
    if ((Test-Path $gateDir) -and ((Get-Date) - (Get-Item $gateDir).LastWriteTime).TotalHours -gt 2) {
        Remove-Item $gateDir -Recurse -Force -ErrorAction SilentlyContinue
        continue
    }
    if (-not $waited) { Write-Host "[gate] another window is flashing, queued..."; $waited = $true }
    if ((Get-Date) -gt $deadline) { Write-Host "[gate] flash lock timeout"; exit 3 }
    Start-Sleep -Seconds 5
}
Set-Content -Path (Join-Path $gateDir "owner.txt") -Encoding ASCII `
    -Value "owner=$Owner pid=$PID port=$Port mode=$Mode start=$(Get-Date -Format o)"

$code = 1
try {
    Write-Host "[gate] flash lock acquired owner=$Owner port=$Port mode=$Mode"

    $ok = $false
    for ($i = 1; $i -le $Retries -and -not $ok; $i++) {
        if ($Mode -eq 'app') {
            Push-Location (Join-Path $root 'build')
            & $py -m esptool --chip esp32p4 -p $Port -b $Baud --before default_reset --after hard_reset write_flash "@flash_args" *> $log
            $rc = $LASTEXITCODE
            Pop-Location
        } else {
            & $py -m esptool --chip esp32p4 -p $Port -b $Baud --before default_reset --after hard_reset write_flash 0x0 $Image *> $log
            $rc = $LASTEXITCODE
        }

        $text = if (Test-Path $log) { Get-Content $log -Raw -Encoding UTF8 } else { "" }
        if ($text -match 'Hash of data verified') { $ok = $true; $code = 0; break }

        $portIssue = ($text -match 'PermissionError|FileNotFoundError|busy|Could not open|could not open|Failed to connect')
        $reason = if ($rc -eq 0) { 'no-hash' } else { "exit=$rc" }
        if ($portIssue) {
            Write-Host ("[flash] attempt {0}/{1} failed ({2}): port busy/renumerating, retry in 5s" -f $i, $Retries, $reason)
            Start-Sleep -Seconds 5
        } else {
            Write-Host ("[flash] attempt {0}/{1} failed ({2}): non-port error, stop" -f $i, $Retries, $reason)
            break
        }
    }

    if ($ok) {
        Write-Host "[flash] FLASH OK  $Mode  $Port  log: $log"
    } else {
        Write-Host "[flash] FLASH FAILED, last lines:"
        Get-Content $log -Tail 8 | ForEach-Object { "  " + $_ }
    }
} finally {
    Remove-Item $gateDir -Recurse -Force -ErrorAction SilentlyContinue
}

exit $code
