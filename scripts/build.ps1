#requires -Version 7
<#
.SYNOPSIS
    Configure and build the project with CMake/Ninja.
.PARAMETER Preset
    CMake preset to use: 'debug' or 'release'.
.PARAMETER ConfigureOnly
    Configure without building.
.PARAMETER Run
    Launch the built executable after a successful build.
#>

[CmdletBinding()]
param(
    [ValidateSet('debug', 'release')]
    [string]$Preset = 'debug',

    [switch]$ConfigureOnly,
    [switch]$Run
)

$ErrorActionPreference = 'Stop'
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = Resolve-Path (Join-Path $ScriptDir '..')

$env:PATH = 'E:\dev\bin;' + $env:PATH

if (-not $env:VULKAN_SDK) { throw "VULKAN_SDK environment variable is not set." }
if (-not $env:VCPKG_ROOT) { throw "VCPKG_ROOT environment variable is not set." }

function Step($msg)  { Write-Host "==> $msg" -ForegroundColor Cyan }
function Good($msg)  { Write-Host "    $msg" -ForegroundColor Green }
function Warn($msg)  { Write-Host "    $msg" -ForegroundColor Yellow }

Push-Location $RepoRoot
try {
    Step "Configuring (preset: $Preset)"
    & cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }

    if ($ConfigureOnly) { Good "Configure complete."; return }

    Step "Building (preset: $Preset)"
    & cmake --build --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($LASTEXITCODE)" }

    Good "Build complete."

    if ($Run) {
        $binDir = if ($Preset -eq 'release') { 'build-release' } else { 'build' }
        $exe = Join-Path $RepoRoot "$binDir/vulkan-text-cpp.exe"
        if (-not (Test-Path $exe)) { throw "Executable not found: $exe" }
        Step "Running $exe"
        & $exe
    }
}
finally {
    Pop-Location
}
