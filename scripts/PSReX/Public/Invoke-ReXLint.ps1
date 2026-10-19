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
        Write-Error "compile_commands.json not found at $BuildDir`nRun rex-configure first."
        return
    }

    $lintDir = Join-Path $BuildDir "lint"
    New-Item -ItemType Directory -Path $lintDir -Force | Out-Null
    $entries = Get-Content $compileDb -Raw | ConvertFrom-Json
    $debugEntries = @($entries | Where-Object { $_.output -match '[\\/]Debug[\\/]' })
    if ($debugEntries.Count -eq 0) { $debugEntries = @($entries) }
    ConvertTo-Json -InputObject $debugEntries -Depth 4 | Set-Content (Join-Path $lintDir "compile_commands.json")

    $headerUnits = [System.IO.Path]::GetFullPath((Join-Path $BuildDir "tests/headers"))
    $roots = @((Join-Path $root "src"), (Join-Path $root "tests"), $headerUnits) |
        ForEach-Object { [System.IO.Path]::GetFullPath($_) + [System.IO.Path]::DirectorySeparatorChar }
    $units = @($debugEntries | ForEach-Object { [System.IO.Path]::GetFullPath($_.file) } | Sort-Object -Unique)
    $inScope = { param($path) @($roots | Where-Object { $path.StartsWith($_, [System.StringComparison]::OrdinalIgnoreCase) }).Count -gt 0 }

    if ($Since) {
        $Files = @(git -C $root diff --name-only --diff-filter=ACMR "$Since...HEAD" -- include src tests)
    }
    if (-not $PSBoundParameters.ContainsKey('Files') -and -not $Since) {
        $targets = @($units | Where-Object { & $inScope $_ })
    } else {
        $targets = @($Files | ForEach-Object {
            $full = [System.IO.Path]::GetFullPath($(if ([System.IO.Path]::IsPathRooted($_)) { $_ } else { Join-Path $root $_ }))
            if ($full -match '[\\/]include[\\/](rex[\\/].+\.h)$') { Join-Path $headerUnits "$($Matches[1]).cpp" }
            elseif ($full -match '\.(cpp|c)$') { $full }
        } | ForEach-Object { [System.IO.Path]::GetFullPath($_) } |
            Where-Object { $units -contains $_ } | Sort-Object -Unique)
    }

    Write-Host "=== Running clang-tidy on $($targets.Count) file(s) ==="
    $failed = 0
    foreach ($file in $targets) {
        $output = & clang-tidy --quiet -p $lintDir $file 2>&1
        if ($LASTEXITCODE -ne 0 -or ($output -match 'warning:|error:')) {
            $output | Write-Host
            $failed++
        }
    }
    Write-Host "Done. $failed file(s) with findings."
    if ($failed -gt 0) { $global:LASTEXITCODE = 1 } else { $global:LASTEXITCODE = 0 }
}
