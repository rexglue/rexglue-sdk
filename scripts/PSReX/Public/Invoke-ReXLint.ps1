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
        [string]$BuildDir,
        [string[]]$Files,
        [string]$Since
    )

    $root = Get-ReXRoot
    if (-not $BuildDir) {
        $preset = Get-ReXPreset
        $BuildDir = Join-Path $root "out/build/$preset"
    }

    $compileDb = Join-Path $BuildDir "compile_commands.json"
    if (-not (Test-Path $compileDb)) {
        Write-Error "compile_commands.json not found at $BuildDir`nConfigure first: rex-configure"
        return
    }

    $lintDir = Join-Path $BuildDir "lint"
    New-Item -ItemType Directory -Path $lintDir -Force | Out-Null
    $entries = Get-Content $compileDb -Raw | ConvertFrom-Json
    $debugEntries = @($entries | Where-Object { $_.output -match '[\\/]Debug[\\/]' })
    if ($debugEntries.Count -eq 0) { $debugEntries = @($entries) }
    ConvertTo-Json -InputObject $debugEntries -Depth 4 | Set-Content (Join-Path $lintDir "compile_commands.json")

    if ($Since) {
        $Files = @(git -C $root diff --name-only --diff-filter=ACMR "$Since...HEAD" -- include src tests)
    }
    if (-not $PSBoundParameters.ContainsKey('Files') -and -not $Since) {
        $Files = @(Get-ChildItem -Path (Join-Path $root "include"), (Join-Path $root "src") `
            -Recurse -Include *.cpp, *.h | ForEach-Object { $_.FullName })
    }
    $targets = $Files | Where-Object { $_ -match '\.(cpp|h)$' } | ForEach-Object {
        if ([System.IO.Path]::IsPathRooted($_)) { $_ } else { Join-Path $root $_ }
    } | Where-Object { Test-Path $_ }

    Write-Host "=== Running clang-tidy on $($targets.Count) file(s) ==="
    $failed = 0
    foreach ($file in $targets) {
        $extra = @()
        if ($file -match '[\\/]tests[\\/]') { $extra += '--checks=-bugprone-throwing-static-initialization' }
        $output = & clang-tidy --quiet --config-file="$root/.clang-tidy" @extra -p $lintDir $file 2>&1
        if ($LASTEXITCODE -ne 0 -or ($output -match 'warning:|error:')) {
            $output | Write-Host
            $failed++
        }
    }
    Write-Host "Done. $failed file(s) with findings."
    if ($failed -gt 0) { $global:LASTEXITCODE = 1 } else { $global:LASTEXITCODE = 0 }
}
