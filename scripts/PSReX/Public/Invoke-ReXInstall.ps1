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

    $cache = Join-Path $buildDir "CMakeCache.txt"
    $prefixLine = Select-String -Path $cache -Pattern '^CMAKE_INSTALL_PREFIX:PATH=(.+)$' | Select-Object -First 1
    if (-not $prefixLine) { throw "CMAKE_INSTALL_PREFIX not found in $cache" }
    $installDir = [System.IO.Path]::GetFullPath($prefixLine.Matches[0].Groups[1].Value)
    $installRoot = [System.IO.Path]::GetFullPath((Join-Path $root "out/install"))
    if (-not $installDir.StartsWith($installRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "install prefix $installDir is outside $installRoot"
    }
    if (Test-Path -LiteralPath $installDir) {
        Remove-Item -LiteralPath $installDir -Recurse -Force
    }

    foreach ($config in @("Debug", "Release", "RelWithDebInfo")) {
        Write-Host "=== Installing $config ==="
        cmake --install $buildDir --config $config
        if ($LASTEXITCODE -ne 0) { throw "cmake install ($config) failed (exit $LASTEXITCODE)" }
    }
}
