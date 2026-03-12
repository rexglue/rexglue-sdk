<#
.SYNOPSIS
    Run rexglue-sdk tests via ctest.
.NOTES
    Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
    All rights reserved.
    Licensed under the BSD 3-Clause License.
    See LICENSE file in the project root for full license text.
#>
function Invoke-ReXTest {
    [CmdletBinding()]
    param(
        [ValidateSet("Debug", "Release", "RelWithDebInfo")]
        [string]$Config = "Debug"
    )

    $preset = Get-ReXPreset
    $testPreset = "$preset-$($Config.ToLower())"

    Write-Host "=== Running tests: $testPreset ==="
    ctest --preset $testPreset
    if ($LASTEXITCODE -ne 0) { throw "ctest failed (exit $LASTEXITCODE)" }
}
