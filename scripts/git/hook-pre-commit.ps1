<#
.SYNOPSIS
    Pre-commit hook - check formatting on staged files.
.NOTES
    Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
    All rights reserved.
    Licensed under the BSD 3-Clause License.
    See LICENSE file in the project root for full license text.
#>
#Requires -Version 7.0

$ErrorActionPreference = "Stop"

$RepoRoot = git rev-parse --show-toplevel
if (-not $RepoRoot) { exit 1 }
$RepoRoot = [System.IO.Path]::GetFullPath($RepoRoot)

# Import PSReX module (initializes VS environment if needed)
Import-Module "$RepoRoot/scripts/PSReX" -Force -WarningAction SilentlyContinue

# -- Collect staged files ------------------------------------------------------
$staged = git diff --cached --name-only --diff-filter=ACM
if (-not $staged) { exit 0 }

$cppFiles = @()
$cmakeFiles = @()
foreach ($file in $staged) {
    switch -Wildcard ($file) {
        "*.cpp"  { $cppFiles += $file }
        "*.hpp"  { $cppFiles += $file }
        "*.c"    { $cppFiles += $file }
        "*.h"    { $cppFiles += $file }
        "*.cc"   { $cppFiles += $file }
        "*.hh"   { $cppFiles += $file }
        "*.cxx"  { $cppFiles += $file }
        "*.hxx"  { $cppFiles += $file }
        "CMakeLists.txt" { $cmakeFiles += $file }
        "*.cmake" { $cmakeFiles += $file }
    }
}

if ($cppFiles.Count -eq 0 -and $cmakeFiles.Count -eq 0) { exit 0 }

# -- Temp directory for staged content -----------------------------------------
$tmpDir = Join-Path ([System.IO.Path]::GetTempPath()) "rexglue-precommit-$PID"
New-Item -ItemType Directory -Path $tmpDir -Force | Out-Null

$failedFiles = @()

function Extract-Staged {
    param([string]$File)
    $dest = Join-Path $tmpDir $File
    $destDir = Split-Path $dest
    if (-not (Test-Path $destDir)) {
        New-Item -ItemType Directory -Path $destDir -Force | Out-Null
    }
    # Rejoin with LF to avoid PowerShell pipeline stripping newlines
    $lines = @(& git show ":$File")
    ($lines -join "`n") + "`n" | Set-Content -Path $dest -NoNewline -Encoding utf8NoBOM
    return $dest
}

try {
    # -- clang-format (C/C++) --------------------------------------------------
    if ($cppFiles.Count -gt 0 -and (Get-Command clang-format -ErrorAction SilentlyContinue)) {
        foreach ($file in $cppFiles) {
            $tmpFile = Extract-Staged $file
            & clang-format --dry-run --Werror `
                --style="file:$RepoRoot/.clang-format" `
                $tmpFile *> $null
            if ($LASTEXITCODE -ne 0) {
                Write-Host "[$file] failed clang-format"
                $failedFiles += $file
            }
        }
    }

    # -- cmake-format (CMake files) --------------------------------------------
    if ($cmakeFiles.Count -gt 0 -and (Get-Command cmake-format -ErrorAction SilentlyContinue)) {
        foreach ($file in $cmakeFiles) {
            $tmpFile = Extract-Staged $file
            & cmake-format --check `
                -c "$RepoRoot/.cmake-format.yaml" `
                $tmpFile *> $null
            if ($LASTEXITCODE -ne 0) {
                Write-Host "[$file] failed cmake-format"
                $failedFiles += $file
            }
        }
    }

    # -- Summary ---------------------------------------------------------------
    if ($failedFiles.Count -gt 0) {
        Write-Host ""
        Write-Host "Pre-commit check FAILED ($($failedFiles.Count) file(s))."
        exit 1
    }

    exit 0
} finally {
    Remove-Item -Recurse -Force $tmpDir -ErrorAction SilentlyContinue
}
