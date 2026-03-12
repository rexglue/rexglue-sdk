<#
.SYNOPSIS
    Auto-detect the platform CMake preset.
.NOTES
    Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
    All rights reserved.
    Licensed under the BSD 3-Clause License.
    See LICENSE file in the project root for full license text.
#>
function Get-ReXPreset {
    if ($env:REXGLUE_PRESET) { return $env:REXGLUE_PRESET }
    $platform = if ($IsWindows) { "win" } else { "linux" }
    $arch = if ($IsWindows -and $env:PROCESSOR_ARCHITECTURE -eq "ARM64") { "arm64" } else { "amd64" }
    return "$platform-$arch"
}
