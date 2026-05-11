# Build CsoundNativeInput.dll for Windows x64 (WASAPI) and copy it into the
# CsoundUnity Unity package.
#
# Requirements:
#   - Visual Studio 2022 with C++ workload (or 2019, adjust generator below)
#   - CMake 3.18+ (downloaded via VS installer or standalone)
#
# Output: $env:CSOUNDUNITY_PACKAGE_PATH\Runtime\Win64\CsoundNativeInput.dll
#
# Usage (PowerShell):
#   $env:CSOUNDUNITY_PACKAGE_PATH = "C:\path\to\Packages\CsoundUnity"
#   .\build-windows.ps1

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not $env:CSOUNDUNITY_PACKAGE_PATH) {
    Write-Error "CSOUNDUNITY_PACKAGE_PATH is not set."
    exit 1
}

if (-not (Test-Path $env:CSOUNDUNITY_PACKAGE_PATH)) {
    Write-Error "CSOUNDUNITY_PACKAGE_PATH does not exist: $env:CSOUNDUNITY_PACKAGE_PATH"
    exit 1
}

$BuildDir = Join-Path $ScriptDir "build-windows"
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
Push-Location $BuildDir

try {
    & cmake $ScriptDir `
        -G "Visual Studio 17 2022" `
        -A x64 `
        "-DCSOUNDUNITY_PACKAGE_PATH=$env:CSOUNDUNITY_PACKAGE_PATH"
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

    & cmake --build . --config Release
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

    Write-Host "Done: $env:CSOUNDUNITY_PACKAGE_PATH\Runtime\Win64\CsoundNativeInput.dll"
} finally {
    Pop-Location
}
