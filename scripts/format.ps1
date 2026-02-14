$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot

Write-Host "=== Formatting C/C++ files ==="
$cppFiles = Get-ChildItem -Path "$RepoRoot\include", "$RepoRoot\src", "$RepoRoot\tests" `
    -Recurse -Include *.cpp, *.h, *.hpp, *.c

if ($cppFiles) {
    $cppFiles | ForEach-Object {
        clang-format -i $_.FullName
    }
    Write-Host "  Formatted $($cppFiles.Count) files"
}

Write-Host "Done."
