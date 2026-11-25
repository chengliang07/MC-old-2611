<#
.SYNOPSIS
  Verify that the current diff stays inside the paths claimed by this window.

.DESCRIPTION
  Reads docs/parallel/claims.json. Any modified/added/deleted path outside the
  -Owner claim prefixes is reported as a violation. Run this BEFORE building:
  DSH file writes are last-writer-wins with no conflict detection, so two
  windows editing the same file silently overwrite each other.

.EXAMPLE
  ./scripts/parallel/check_scope.ps1 -Owner oral
  ./scripts/parallel/check_scope.ps1 -All      # list every change grouped by owner
#>
param(
    [string]$Owner = "",
    [switch]$All
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root

$claimsPath = Join-Path $root "docs\parallel\claims.json"
if (-not (Test-Path $claimsPath)) { Write-Host "missing $claimsPath"; exit 2 }
$claims = Get-Content $claimsPath -Raw -Encoding UTF8 | ConvertFrom-Json

$paths = @()
foreach ($line in (git status --porcelain -uall)) {
    if ([string]::IsNullOrWhiteSpace($line)) { continue }
    $p = $line.Substring(3).Trim()
    if ($p -match '^(.+?) -> (.+)$') {
        $paths += $Matches[1].Trim('"')
        $paths += $Matches[2].Trim('"')
    } else {
        $paths += $p.Trim('"')
    }
}
$paths = $paths | Sort-Object -Unique

$ownerOf = @{}
foreach ($w in $claims.windows.PSObject.Properties) {
    foreach ($pre in $w.Value.paths) { $ownerOf[$pre] = $w.Name }
}

function Get-Owner([string]$path) {
    $norm = $path -replace '\\', '/'
    $best = $null
    $bestLen = -1
    foreach ($pre in $ownerOf.Keys) {
        $p = ($pre -replace '\\', '/').TrimEnd('/')
        if ($norm -eq $p -or $norm.StartsWith($p + '/')) {
            if ($p.Length -gt $bestLen) { $bestLen = $p.Length; $best = $ownerOf[$pre] }
        }
    }
    return $best
}

if ($All) {
    Write-Host "current changes grouped by claimed owner:"
    $grouped = @{}
    foreach ($p in $paths) {
        $o = Get-Owner $p
        if (-not $o) { $o = "<unclaimed>" }
        if (-not $grouped.ContainsKey($o)) { $grouped[$o] = @() }
        $grouped[$o] += $p
    }
    foreach ($k in ($grouped.Keys | Sort-Object)) {
        Write-Host ("  [{0}] {1} file(s)" -f $k, $grouped[$k].Count)
        $grouped[$k] | ForEach-Object { Write-Host "      $_" }
    }
    exit 0
}

if ([string]::IsNullOrWhiteSpace($Owner)) {
    Write-Host "need -Owner <window> or -All"
    Write-Host ("known windows: " + (($claims.windows.PSObject.Properties.Name) -join ', '))
    exit 2
}

$mine = @()
$stolen = @()
$unclaimed = @()
foreach ($p in $paths) {
    $o = Get-Owner $p
    if ($null -eq $o) { $unclaimed += $p }
    elseif ($o -eq $Owner) { $mine += $p }
    else { $stolen += [pscustomobject]@{ Path = $p; Owner = $o } }
}

Write-Host ("[scope] owner={0}  own changes: {1}" -f $Owner, $mine.Count)
$mine | ForEach-Object { Write-Host "    ok  $_" }

if ($stolen.Count -gt 0) {
    Write-Host "[scope] OUT OF SCOPE (owned by another window; stop and coordinate):"
    $stolen | ForEach-Object { Write-Host ("    !!  {0}  (owner {1})" -f $_.Path, $_.Owner) }
}
if ($unclaimed.Count -gt 0) {
    Write-Host "[scope] UNCLAIMED (register in docs/parallel/claims.json first):"
    $unclaimed | ForEach-Object { Write-Host "    ??  $_" }
}

if ($stolen.Count -gt 0 -or $unclaimed.Count -gt 0) { exit 1 }
Write-Host "[scope] OK"
exit 0
