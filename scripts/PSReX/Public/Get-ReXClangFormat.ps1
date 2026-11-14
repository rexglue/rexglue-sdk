<#
.SYNOPSIS
    Resolve the pinned clang-format binary from scripts/requirements-dev.txt.
.NOTES
    Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
    All rights reserved.
    Licensed under the BSD 3-Clause License.
    See LICENSE file in the project root for full license text.
#>
function Get-ReXClangFormat {
    [CmdletBinding()]
    param()

    $install = "python -m pip install -r scripts/requirements-dev.txt"

    $requirements = Join-Path (Get-ReXRoot) "scripts/requirements-dev.txt"
    if (-not (Test-Path $requirements)) {
        throw "requirements-dev.txt not found at $requirements"
    }

    $match = Select-String -Path $requirements -Pattern '^\s*clang-format\s*==\s*(\S+)' |
        Select-Object -First 1
    if (-not $match) {
        throw "No 'clang-format==<version>' pin found in $requirements"
    }
    $pinned = $match.Matches[0].Groups[1].Value

    $python = Get-Command python -ErrorAction SilentlyContinue
    if (-not $python) { $python = Get-Command python3 -ErrorAction SilentlyContinue }
    if (-not $python) {
        throw "Python not found on PATH. Install Python and run $install"
    }

    $locator = 'import glob, os, clang_format; ' +
               'print(glob.glob(os.path.join(os.path.dirname(clang_format.__file__), "data", "bin", "clang-format*"))[0])'
    $exe = & $python.Source -c $locator 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $exe -or -not (Test-Path $exe)) {
        throw "clang-format $pinned is not installed. Run $install"
    }

    $reported = (& $exe --version) -replace '^.*clang-format version\s+', '' -replace '\s.*$', ''
    if ($reported -ne $pinned) {
        throw "clang-format found $reported, expected $pinned. Run $install"
    }

    return $exe
}
