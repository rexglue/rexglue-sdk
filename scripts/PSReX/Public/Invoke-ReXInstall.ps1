<#
.SYNOPSIS
    Install all rexglue-sdk build configurations.
.NOTES
    Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
    All rights reserved.
    Licensed under the BSD 3-Clause License.
    See LICENSE file in the project root for full license text.
#>
function Invoke-ReXInstall {
    [CmdletBinding()]
    param()

    $root = Get-ReXRoot
    $preset = Get-ReXPreset
    $buildDir = Join-Path $root "out/build/$preset"

    foreach ($config in @("Debug", "Release", "RelWithDebInfo")) {
        Write-Host "=== Installing $config ==="
        cmake --install $buildDir --config $config
        if ($LASTEXITCODE -ne 0) { throw "cmake install ($config) failed (exit $LASTEXITCODE)" }
    }
}
