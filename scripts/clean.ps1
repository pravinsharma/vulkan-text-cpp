#requires -Version 7
<#
.SYNOPSIS
    Remove all CMake build directories.
#>

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = Resolve-Path (Join-Path $ScriptDir '..')

Get-ChildItem -LiteralPath $RepoRoot -Directory |
    Where-Object { $_.Name -eq 'build' -or $_.Name -like 'build-*' } |
    ForEach-Object {
        Write-Host "Removing $($_.FullName)" -ForegroundColor Yellow
        Remove-Item -LiteralPath $_.FullName -Recurse -Force
    }

Write-Host "Clean complete." -ForegroundColor Green
