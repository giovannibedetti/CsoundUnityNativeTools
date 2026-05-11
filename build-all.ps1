# Master build script (Windows host).
# Builds the Windows DLL of CsoundNativeInput and copies it into the
# CsoundUnity Unity package.
#
# Host capability:
#   Windows host → builds Windows native input. (Android is buildable too if
#   the NDK is configured, but typically Android binaries are produced on
#   macOS/Linux hosts.)
#
# Required env var:
#   $env:CSOUNDUNITY_PACKAGE_PATH  Path to the local CsoundUnity package clone

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not $env:CSOUNDUNITY_PACKAGE_PATH) {
    Write-Error "CSOUNDUNITY_PACKAGE_PATH is not set."
    exit 1
}

Write-Host "═══════════════════════════════════════════════════════════════════════"
Write-Host " Target: $env:CSOUNDUNITY_PACKAGE_PATH"
Write-Host "═══════════════════════════════════════════════════════════════════════"

function Run-Step {
    param([string]$Label, [string]$Script)
    Write-Host ""
    Write-Host "─── $Label ─────────────────────────────────────────────"
    try {
        & $Script
        if ($LASTEXITCODE -ne 0) { throw "exit $LASTEXITCODE" }
    } catch {
        Write-Warning "$Label failed (skipping): $_"
    }
}

Run-Step "Native input — Windows" (Join-Path $ScriptDir "native-input\build-windows.ps1")

Write-Host ""
Write-Host "═══════════════════════════════════════════════════════════════════════"
Write-Host " All compatible builds complete."
Write-Host "═══════════════════════════════════════════════════════════════════════"
