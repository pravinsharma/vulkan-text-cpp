#requires -Version 7
<#
.SYNOPSIS
    Verify that all required build tools and environment variables are present.
#>

[CmdletBinding()]
param()

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = Resolve-Path (Join-Path $ScriptDir '..')

$ErrorActionPreference = 'Continue'

function Check-Env($name) {
    $val = [Environment]::GetEnvironmentVariable($name)
    if ($val) {
        Write-Host "  [OK]   $name = $val" -ForegroundColor Green
    } else {
        Write-Host "  [MISS] $name is not set" -ForegroundColor Red
    }
}

function Check-Tool($cmd) {
    $found = Get-Command $cmd -ErrorAction SilentlyContinue
    if ($found) {
        & $found --version | Out-Null
        $ver = if ($LASTEXITCODE -eq 0) { (& $found --version 2>$null | Select-Object -First 1) -join ' ' } else { '' }
        Write-Host "  [OK]   $cmd $ver" -ForegroundColor Green
    } else {
        Write-Host "  [MISS] $cmd not found on PATH" -ForegroundColor Red
    }
}

Write-Host "Environment variables:" -ForegroundColor Cyan
Check-Env 'VULKAN_SDK'
Check-Env 'VCPKG_ROOT'

Write-Host "`nTools:" -ForegroundColor Cyan
$env:PATH = 'E:\dev\bin;' + $env:PATH
Check-Tool 'cmake'
Check-Tool 'ninja'
Check-Tool 'cl'

Write-Host "`nProject layout:" -ForegroundColor Cyan
Get-ChildItem -LiteralPath $RepoRoot -Depth 0 -Force |
    Where-Object { $_.Name -not in '.git' } |
    ForEach-Object { Write-Host "  $($_.Name)" }
