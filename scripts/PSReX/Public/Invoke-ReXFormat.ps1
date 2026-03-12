<#
.SYNOPSIS
    Format C/C++ source files with clang-format.
.NOTES
    Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
    All rights reserved.
    Licensed under the BSD 3-Clause License.
    See LICENSE file in the project root for full license text.
#>
function Invoke-ReXFormat {
    [CmdletBinding(DefaultParameterSetName = 'Modified')]
    param(
        [Parameter(ParameterSetName = 'Files', Position = 0, ValueFromRemainingArguments)]
        [string[]]$Path,

        [Parameter(ParameterSetName = 'Modified')]
        [switch]$Modified,

        [Parameter(ParameterSetName = 'All')]
        [switch]$All
    )

    $root = Get-ReXRoot
    $extensions = @("*.cpp", "*.h", "*.hpp", "*.c")

    if ($Path) {
        # Explicit files provided
        $files = @($Path | ForEach-Object { Resolve-Path $_ -ErrorAction Stop })
        Write-Host "=== Formatting $($files.Count) specified file(s) ==="
    } elseif ($All) {
        # All source files
        $searchDirs = @("include", "src", "tests") | ForEach-Object { Join-Path $root $_ } | Where-Object { Test-Path $_ }
        $files = @(Get-ChildItem -Path $searchDirs -Recurse -Include $extensions | Select-Object -ExpandProperty FullName)
        Write-Host "=== Formatting all C/C++ files ==="
    } else {
        # Default: modified files (staged + unstaged changes)
        $diff = git -C $root diff --name-only HEAD 2>$null
        $untracked = git -C $root ls-files --others --exclude-standard 2>$null
        $candidates = @($diff) + @($untracked) | Where-Object { $_ }
        $files = @($candidates | Where-Object {
            $ext = [System.IO.Path]::GetExtension($_)
            $ext -in @('.cpp', '.h', '.hpp', '.c')
        } | ForEach-Object { Join-Path $root $_ } | Where-Object { Test-Path $_ })
        Write-Host "=== Formatting modified C/C++ files ==="
    }

    if ($files.Count -gt 0) {
        $files | ForEach-Object { clang-format -i $_ }
        Write-Host "  Formatted $($files.Count) files"
    } else {
        Write-Host "  No files found"
    }

    Write-Host "Done."
}
