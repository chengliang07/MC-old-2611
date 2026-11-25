# ESP-IDF 环境 + ccache 设置。其它脚本 dot-source 本文件即可。
#
# 用法：
#   . scripts\idf-env.ps1
#   idf.py -DCMAKE_NINJA_FORCE_RESPONSE_FILE=ON --ccache build

$ccacheDir = 'D:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64'

$env:PATH = "D:\Espressif\tools\cmake\3.30.2\bin;D:\Espressif\tools\ninja\1.12.1;D:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;D:\Espressif\tools\python\v5.5.6\venv\Scripts;$ccacheDir;$($env:PATH)"

$env:IDF_PATH = 'D:\esp\v5.5.6\esp-idf'
$env:IDF_TOOLS_PATH = 'D:\Espressif'
$env:IDF_PYTHON_ENV_PATH = 'D:\Espressif\tools\python\v5.5.6\venv'
$env:ESP_IDF_VERSION = '5.5'
$env:ESP_ROM_ELF_DIR = 'D:\Espressif\tools\esp-rom-elfs\20260528\'
$env:IDF_PYTHON_CHECK_CONSTRAINTS = '0'
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'

# ccache：所有 worktree 共用一个缓存。BASEDIR 取当前 worktree 根，
# 这样各目录里的绝对路径都被归一成 "main/..." 才能跨目录命中。
$env:CCACHE_DIR = 'D:\Espressif\ccache-cache'
$env:CCACHE_MAXSIZE = '8G'
$env:IDF_CCACHE_ENABLE = '1'
$env:CCACHE_COMPRESS = '1'
$env:CCACHE_BASEDIR = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path $env:CCACHE_DIR)) { New-Item -ItemType Directory -Force -Path $env:CCACHE_DIR | Out-Null }

$env:PATH = "$ccacheDir;$env:PATH"
