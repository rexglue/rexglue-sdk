<#
.SYNOPSIS
    Build the rexglue-sdk project for a given configuration.
.NOTES
    Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
    All rights reserved.
    Licensed under the BSD 3-Clause License.
    See LICENSE file in the project root for full license text.
#>
function Invoke-ReXBuild {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [ValidateSet("Debug", "Release", "RelWithDebInfo")]
        [string]$Config
    )

    $root = Get-ReXRoot
    $preset = Get-ReXPreset
    $buildDir = Join-Path $root "out/build/$preset"

    Write-Host "=== Building $Config ==="
    cmake --build $buildDir --config $Config
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed (exit $LASTEXITCODE)" }
}
