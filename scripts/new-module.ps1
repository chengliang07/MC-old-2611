# 一键新开一个模块窗口：创建独立目录 + 分支，并从 main 同步最新代码。
#
# 用法：
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/new-module.ps1 -Name math
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/new-module.ps1 -Name math -Remove
#
# 执行完在「新对话窗口」里打开提示的目录即可。

param(
    [Parameter(Mandatory = $true)]
    [string]$Name,
    [switch]$Remove
)

$ErrorActionPreference = 'Continue'

if ($Name -notmatch '^[a-z][a-z0-9-]{1,20}$') {
    Write-Host "模块名只能用小写字母/数字/短横线，例如 math、oral-v2" -ForegroundColor Red
    exit 1
}

$parent = 'E:\09-Soft_DEV'
$mainDir = Join-Path $parent 'metalio-claw4'
$workDir = Join-Path $parent "wt-$Name"
$branch = "feat/$Name"

function Write-Step($t) { Write-Host "[module] $t" -ForegroundColor Cyan }

if ($Remove) {
    if (-not (Test-Path $workDir)) {
        Write-Host "目录不存在：$workDir" -ForegroundColor Yellow
        exit 0
    }
    Push-Location $mainDir
    try {
        git worktree remove $workDir --force | Out-Null
        Write-Step "已移除 $workDir（分支 $branch 仍保留，需要时 git branch -D $branch）"
    } finally {
        Pop-Location
    }
    exit 0
}

if (Test-Path $workDir) {
    Write-Host "目录已存在：$workDir" -ForegroundColor Yellow
    Write-Host "要重建请先加 -Remove" -ForegroundColor Yellow
    exit 1
}

Push-Location $mainDir
try {
    $env:GIT_SSH_COMMAND = 'ssh -o BatchMode=yes -o ConnectTimeout=20 -o StrictHostKeyChecking=accept-new'
    git -c core.sshCommand="$env:GIT_SSH_COMMAND" fetch ssh-ci main 2>$null | Out-Null

    $exists = (git branch --format='%(refname:short)') -contains $branch
    if ($exists) {
        git worktree add $workDir $branch | Out-Null
        Push-Location $workDir
        try { git merge --ff-only main | Out-Null } finally { Pop-Location }
        Write-Step "复用已有分支 $branch"
    } else {
        git worktree add $workDir -b $branch main | Out-Null
        Write-Step "新建分支 $branch"
    }
} finally {
    Pop-Location
}

Write-Host ''
Write-Step "目录：$workDir"
Write-Step "分支：$branch"
Write-Host ''
Write-Host '下一步：' -ForegroundColor Green
Write-Host "  1. 新开一个对话窗口，打开目录：$workDir"
Write-Host '  2. 直接说需求'
Write-Host "  3. 编译：idf.py -DCMAKE_NINJA_FORCE_RESPONSE_FILE=ON build"
Write-Host "  4. 烧录：powershell -NoProfile -ExecutionPolicy Bypass -File scripts\flash-device.ps1"
Write-Host "  5. 完成后：git add -A; git commit; git push -u ssh-ci $branch"
Write-Host '  6. 回主目录合并：git merge --no-ff ' + $branch
Write-Host ''
exit 0
