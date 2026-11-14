<#
.SYNOPSIS
    Resolve the Python that runs the pinned cmakelang from scripts/requirements-dev.txt.
.NOTES
    Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
    All rights reserved.
    Licensed under the BSD 3-Clause License.
    See LICENSE file in the project root for full license text.
#>
function Get-ReXCMakeLang {
    [CmdletBinding()]
    param()

    $install = "python -m pip install -r scripts/requirements-dev.txt"

    $requirements = Join-Path (Get-ReXRoot) "scripts/requirements-dev.txt"
    if (-not (Test-Path $requirements)) {
        throw "requirements-dev.txt not found at $requirements"
    }

    $match = Select-String -Path $requirements -Pattern '^\s*cmakelang\s*==\s*(\S+)' |
        Select-Object -First 1
    if (-not $match) {
        throw "No 'cmakelang==<version>' pin found in $requirements"
    }
    $pinned = $match.Matches[0].Groups[1].Value

    $python = Get-Command python -ErrorAction SilentlyContinue
    if (-not $python) { $python = Get-Command python3 -ErrorAction SilentlyContinue }
    if (-not $python) {
        throw "Python not found on PATH. Install Python and run $install"
    }

    $reported = & $python.Source -c "import importlib.metadata as m; print(m.version('cmakelang'))" 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $reported) {
        throw "cmakelang $pinned is not installed. Run $install"
    }
    if ($reported -ne $pinned) {
        throw "cmakelang found $reported, expected $pinned. Run $install"
    }

    return $python.Source
}
