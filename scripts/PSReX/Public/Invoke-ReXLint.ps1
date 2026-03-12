<#
.SYNOPSIS
    Run clang-tidy on rexglue-sdk source files.
.NOTES
    Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
    All rights reserved.
    Licensed under the BSD 3-Clause License.
    See LICENSE file in the project root for full license text.
#>
function Invoke-ReXLint {
    [CmdletBinding()]
    param(
        [string]$BuildDir
    )

    $root = Get-ReXRoot
    if (-not $BuildDir) {
        $preset = Get-ReXPreset
        $BuildDir = Join-Path $root "out/build/$preset"
    }

    $compileDb = Join-Path $BuildDir "compile_commands.json"
    if (-not (Test-Path $compileDb)) {
        Write-Error "compile_commands.json not found at $BuildDir`nGenerate it first: rex-configure (or cmake --preset <preset>)"
        return
    }

    Write-Host "=== Running clang-tidy ==="
    $files = Get-ChildItem -Path (Join-Path $root "include"), (Join-Path $root "src") `
        -Recurse -Include *.cpp, *.h

    $files | ForEach-Object {
        clang-tidy --config-file="$root/.clang-tidy" -p $BuildDir $_.FullName
    }

    Write-Host "Done."
}
