#requires -Version 7
<#
.SYNOPSIS
    Run the built executable.
.PARAMETER Preset
    Which build preset to run: 'debug' (default) or 'release'.
.PARAMETER Args
    Optional arguments forwarded to the application.
#>

[CmdletBinding()]
param(
    [ValidateSet('debug', 'release')]
    [string]$Preset = 'debug',

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Args
)

$ErrorActionPreference = 'Stop'
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = Resolve-Path (Join-Path $ScriptDir '..')

$binDir = if ($Preset -eq 'release') { 'build-release' } else { 'build' }
$exe = Join-Path $RepoRoot "$binDir/vulkan-text-cpp.exe"

if (-not (Test-Path $exe)) {
    throw "Executable not found: $exe. Run scripts/build.ps1 first."
}

& $exe @Args
exit $LASTEXITCODE
