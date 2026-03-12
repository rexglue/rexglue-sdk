<#
.SYNOPSIS
    Configure the rexglue-sdk CMake project.
.NOTES
    Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
    All rights reserved.
    Licensed under the BSD 3-Clause License.
    See LICENSE file in the project root for full license text.
#>
function Invoke-ReXConfigure {
    [CmdletBinding()]
    param()

    $preset = Get-ReXPreset
    Write-Host "=== Configuring with preset: $preset ==="
    cmake --preset $preset
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed (exit $LASTEXITCODE)" }
}
