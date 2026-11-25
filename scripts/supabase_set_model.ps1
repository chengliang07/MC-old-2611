<#
.SYNOPSIS
  Switch the oral relay's Bailian realtime model by updating Supabase function secrets.

.DESCRIPTION
  Reads a Supabase personal access token (sbp_...) from -Token, or
  ./gates/supabase.pat, or $env:SUPABASE_ACCESS_TOKEN, then POSTs the new
  secrets to the Supabase Management API.

  Project ref is parsed from the relay host (oemsbxavqkmtuyasxsba.supabase.co)
  unless -ProjectRef is given.

  Secrets touched:
    QWEN_REALTIME_MODEL          default model used by the relay
    ORAL_COACH_ALLOWED_MODELS    allow-list gate (silent fallback if missing)

.EXAMPLE
  ./scripts/supabase_set_model.ps1 -Model qwen-audio-3.1-realtime-plus
  ./scripts/supabase_set_model.ps1 -ListSecrets
#>
param(
    [string]$Model = "qwen-audio-3.1-realtime-plus",
    [string]$ProjectRef = "",
    [string]$Token = "",
    [switch]$ListSecrets,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
# this script lives in <repo>/scripts, so the repo root is one level up
$root = Split-Path -Parent $PSScriptRoot

if ([string]::IsNullOrWhiteSpace($Token)) {
    $patFile = Join-Path $root ".gates\supabase.pat"
    if (Test-Path $patFile) {
        $Token = (Get-Content $patFile -Raw).Trim()
    } elseif ($env:SUPABASE_ACCESS_TOKEN) {
        $Token = $env:SUPABASE_ACCESS_TOKEN.Trim()
    }
}
if ([string]::IsNullOrWhiteSpace($Token)) {
    Write-Host "no token. put it in .gates\supabase.pat (gitignored) or pass -Token"
    exit 2
}
if ([string]::IsNullOrWhiteSpace($ProjectRef)) { $ProjectRef = "oemsbxavqkmtuyasxsba" }

$headers = @{
    "Authorization" = "Bearer $Token"
    "Content-Type"  = "application/json"
}
$base = "https://api.supabase.com/v1/projects/$ProjectRef/secrets"

[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

if ($ListSecrets) {
    try {
        $list = Invoke-RestMethod -Uri $base -Method GET -Headers $headers -TimeoutSec 30
        Write-Host "secrets on $ProjectRef (names only):"
        $list | ForEach-Object { Write-Host ("  " + $_.name) }
    } catch {
        Write-Host "GET failed: $($_.Exception.Message)"
        exit 1
    }
    exit 0
}

$body = @(
    @{ name = "QWEN_REALTIME_MODEL"; value = $Model },
    @{ name = "ORAL_COACH_ALLOWED_MODELS"; value = $Model }
) | ConvertTo-Json -Depth 4

if ($DryRun) {
    Write-Host "DRY RUN: would POST $base"
    Write-Host $body
    exit 0
}

Write-Host "POST $base  (model=$Model)"
try {
    $resp = Invoke-RestMethod -Uri $base -Method POST -Headers $headers -Body $body -TimeoutSec 40
    Write-Host "OK: secrets updated. Supabase restarts the function automatically."
    $resp | ForEach-Object { if ($_.name) { Write-Host ("  " + $_.name) } }
} catch {
    $status = $null
    if ($_.Exception.Response) { $status = [int]$_.Exception.Response.StatusCode }
    Write-Host "POST failed (HTTP $status): $($_.Exception.Message)"
    exit 1
}
