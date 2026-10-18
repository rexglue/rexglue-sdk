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
    $cmakeNames = @("CMakeLists.txt", "*.cmake", "*.cmake.in")

    if ($Path) {
        # Explicit files provided
        $resolved = @($Path | ForEach-Object { Resolve-Path $_ -ErrorAction Stop })
        $isCMake = { $name = Split-Path $_ -Leaf; $name -like 'CMakeLists.txt' -or $name -like '*.cmake' -or $name -like '*.cmake.in' }
        $cmakeFiles = @($resolved | Where-Object $isCMake)
        $files = @($resolved | Where-Object { -not (& $isCMake) })
        Write-Host "=== Formatting $($files.Count + $cmakeFiles.Count) specified file(s) ==="
    } elseif ($All) {
        # All source files
        $searchDirs = @("include", "src", "tests") | ForEach-Object { Join-Path $root $_ } | Where-Object { Test-Path $_ }
        $files = @(Get-ChildItem -Path $searchDirs -Recurse -Include $extensions | Select-Object -ExpandProperty FullName)
        $cmakeDirs = @("cmake", "src", "tests") | ForEach-Object { Join-Path $root $_ } | Where-Object { Test-Path $_ }
        $cmakeFiles = @(Get-ChildItem -Path $cmakeDirs -Recurse -Include $cmakeNames | Select-Object -ExpandProperty FullName) +
            @(Join-Path $root "CMakeLists.txt")
        Write-Host "=== Formatting all C/C++ and CMake files ==="
    } else {
        # Default: modified files (staged + unstaged changes)
        $diff = git -C $root diff --name-only HEAD 2>$null
        $untracked = git -C $root ls-files --others --exclude-standard 2>$null
        $candidates = @($diff) + @($untracked) | Where-Object { $_ }
        $files = @($candidates | Where-Object {
            $ext = [System.IO.Path]::GetExtension($_)
            $ext -in @('.cpp', '.h', '.hpp', '.c')
        } | ForEach-Object { Join-Path $root $_ } | Where-Object { Test-Path $_ })
        $cmakeFiles = @($candidates | Where-Object {
            $name = [System.IO.Path]::GetFileName($_)
            $name -like 'CMakeLists.txt' -or $name -like '*.cmake' -or $name -like '*.cmake.in'
        } | ForEach-Object { Join-Path $root $_ } | Where-Object { Test-Path $_ })
        Write-Host "=== Formatting modified C/C++ and CMake files ==="
    }

    if ($files.Count -gt 0) {
        $clangFormat = Get-ReXClangFormat
        $files | ForEach-Object { & $clangFormat -i $_ }
        Write-Host "  Formatted $($files.Count) files"
    } else {
        Write-Host "  No files found"
    }

    if ($cmakeFiles.Count -gt 0) {
        $python = Get-ReXCMakeLang
        $config = Join-Path $root ".cmake-format.yaml"
        $cmakeFiles | ForEach-Object { & $python -m cmakelang.format -i $_ -c $config }
        Write-Host "  Formatted $($cmakeFiles.Count) CMake files"
    }

    Write-Host "Done."
}
