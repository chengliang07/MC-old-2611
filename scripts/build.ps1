# 在任意 worktree 里编译：自动设置 IDF 环境并启用 ccache。
#
# 用法：
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1 -Clean
#
# ccache 缓存目录：D:\Espressif\ccache-cache（多个 worktree 共用，跨目录命中）

param(
    [switch]$Clean
)

$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'idf-env.ps1')

$python = 'D:\Espressif\tools\python\v5.5.6\venv\Scripts\python.exe'
$idfPy = Join-Path $env:IDF_PATH 'tools\idf.py'

Push-Location $repoRoot
try {
    $rev = (git rev-parse --short HEAD) 2>$null
    Write-Host "[build] $repoRoot @ $rev" -ForegroundColor Cyan

    if ($Clean) {
        Write-Host '[build] 清理构建目录...' -ForegroundColor Yellow
        & $python $idfPy fullclean 2>$null | Out-Null
    }

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    & $python -u $idfPy -DCMAKE_NINJA_FORCE_RESPONSE_FILE=ON -DCCACHE_ENABLE=ON build 2>&1 |
        Tee-Object -FilePath (Join-Path $repoRoot 'build.log') | Out-Null
    $code = $LASTEXITCODE
    $sw.Stop()

    if ($code -ne 0) {
        Write-Host ("[build] 编译失败（{0:N0} 秒），日志：build.log" -f $sw.Elapsed.TotalSeconds) -ForegroundColor Red
        Select-String -Path (Join-Path $repoRoot 'build.log') -Pattern 'error:' | Select-Object -First 8 |
            ForEach-Object { Write-Host ('  ' + $_.Line.Trim()) -ForegroundColor Red }
        exit 1
    }

    Write-Host ("[build] 编译完成（{0:N0} 秒）" -f $sw.Elapsed.TotalSeconds) -ForegroundColor Green
} finally {
    Pop-Location
}

exit 0
