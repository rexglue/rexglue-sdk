<#
.SYNOPSIS
    Initialize Visual Studio Developer Shell environment.
.NOTES
    Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
    All rights reserved.
    Licensed under the BSD 3-Clause License.
    See LICENSE file in the project root for full license text.
#>
function Initialize-VsEnvironment {
    if (-not $IsWindows) { return }
    if ($env:VSCMD_VER) { return }

    $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vsWhere)) { return }

    $vsInstall = & $vsWhere -latest -property installationPath
    if (-not $vsInstall) { return }

    $launchScript = Join-Path $vsInstall "Common7\Tools\Launch-VsDevShell.ps1"
    if (-not (Test-Path $launchScript)) { return }

    $arch = if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64") { "arm64" } else { "amd64" }
    & $launchScript -Arch $arch -HostArch amd64 -SkipAutomaticLocation
}
