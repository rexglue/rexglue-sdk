<#
.SYNOPSIS
    Visual Studio Developer PowerShell profile for rexglue-sdk.
.NOTES
    Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
    All rights reserved.
    Licensed under the BSD 3-Clause License.
    See LICENSE file in the project root for full license text.
#>
param(
    [ValidateSet("amd64", "arm64")]
    [string]$Arch = $(if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64") { "arm64" } else { "amd64" })
)

$ErrorActionPreference = "Stop"

# -- Locate repo root ----------------------------------------------------------
$RepoRoot = (git rev-parse --show-toplevel 2>$null)
if (-not $RepoRoot) {
    $RepoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
}
$RepoRoot = [System.IO.Path]::GetFullPath($RepoRoot)

# -- Initialize VS Developer Shell --------------------------------------------
if (-not $env:VSCMD_VER) {
    $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vsWhere)) {
        Write-Error "vswhere.exe not found. Is Visual Studio installed?"
        return
    }

    $vsInstall = & $vsWhere -latest -property installationPath
    if (-not $vsInstall) {
        Write-Error "No Visual Studio installation found via vswhere."
        return
    }

    $launchScript = Join-Path $vsInstall "Common7\Tools\Launch-VsDevShell.ps1"
    if (-not (Test-Path $launchScript)) {
        Write-Error "Launch-VsDevShell.ps1 not found at: $launchScript"
        return
    }

    & $launchScript -Arch $Arch -HostArch amd64 -SkipAutomaticLocation
}

# -- ReXGlue environment ------------------------------------------------------
$Preset = "win-$Arch"

$env:REXGLUE_ROOT = $RepoRoot
$env:REXGLUE_PRESET = $Preset

# Add install bin directory to PATH
$InstallBin = Join-Path $RepoRoot "out\install\$Preset\bin"
if ($env:PATH -notlike "*$InstallBin*") {
    $env:PATH = "$InstallBin;$env:PATH"
}

# -- Import ReXGlue module ----------------------------------------------------
Import-Module "$PSScriptRoot/../PSReX" -Force -Global

# -- Banner --------------------------------------------------------------------
Write-Host ""
Write-Host "  ReXGlue SDK Developer Shell" -ForegroundColor Cyan
Write-Host "  Arch:   $Arch" -ForegroundColor Gray
Write-Host "  Preset: $Preset" -ForegroundColor Gray
Write-Host "  Root:   $RepoRoot" -ForegroundColor Gray
Write-Host ""
Write-Host "  Commands:" -ForegroundColor Yellow
Write-Host "    rex-configure              cmake --preset $Preset"
Write-Host "    rex-build -Config Debug    Build Debug"
Write-Host "    rex-build -Config Release  Build Release"
Write-Host "    rex-test                   Run tests (Debug)"
Write-Host "    rex-install                cmake --install (all configs)"
Write-Host "    rex-format                 Format all C/C++ sources"
Write-Host "    rex-lint                   Run clang-tidy"
Write-Host "    rex-setup                  Run developer environment setup"
Write-Host ""
