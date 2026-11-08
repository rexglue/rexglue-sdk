<#
.SYNOPSIS
    Run clang-tidy through scripts/lint.py.
.DESCRIPTION
    Runs `python scripts/lint.py <build dir>` with the selection given. With no selection it
    checks the files changed since the development branch. The exit code of the script is left
    in $LASTEXITCODE: 1 for a finding, 2 for a setup error, 0 when clean.
.PARAMETER BuildDir
    Configured build directory with compile_commands.json. Defaults to out/build/<preset>.
.PARAMETER Since
    Check the files in <Since>...HEAD. Defaults to development when no other selection is given.
.PARAMETER Files
    Check these repository relative or absolute paths.
.PARAMETER All
    Check every unit below src and tests, and every header check unit.
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
        [string]$Since,
        [string[]]$Files,
        [switch]$All
    )

    $root = Get-ReXRoot
    if (-not $BuildDir) {
        $preset = Get-ReXPreset
        $BuildDir = Join-Path $root "out/build/$preset"
    }

    $selection = @()
    if ($All) {
        $selection = @('--all')
    } elseif ($Files) {
        $selection = @('--files') + $Files
    } else {
        if (-not $Since) { $Since = 'development' }
        $selection = @('--since', $Since)
    }

    $tidyArgs = @()
    $onPath = Get-Command clang-tidy-22, clang-tidy -ErrorAction SilentlyContinue
    if (-not $onPath) {
        $candidates = @()
        $cache = Join-Path $BuildDir 'CMakeCache.txt'
        if (Test-Path $cache) {
            $match = Select-String -Path $cache -Pattern '^CMAKE_CXX_COMPILER:[A-Z]+=(.+)$' | Select-Object -First 1
            if ($match) {
                $candidates += Join-Path (Split-Path $match.Matches[0].Groups[1].Value) 'clang-tidy.exe'
            }
        }
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        if (Test-Path $vswhere) {
            $vs = & $vswhere -latest -property installationPath
            if ($vs) {
                $candidates += Join-Path $vs 'VC/Tools/Llvm/x64/bin/clang-tidy.exe'
            }
        }
        $found = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
        if ($found) {
            $tidyArgs = @('--clang-tidy', $found)
        }
    }

    & python (Join-Path $root 'scripts/lint.py') $BuildDir @selection @tidyArgs
    $global:LASTEXITCODE = $LASTEXITCODE
}
