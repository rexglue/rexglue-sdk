$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = if ($args[0]) { $args[0] } else { "$RepoRoot\out\build\win-amd64" }

if (-not (Test-Path "$BuildDir\compile_commands.json")) {
    Write-Error "compile_commands.json not found at $BuildDir"
    Write-Host "Usage: .\lint.ps1 [build-dir]"
    Write-Host "Generate it first: cmake --preset win-amd64"
    exit 1
}

Write-Host "=== Running clang-tidy ==="
$files = Get-ChildItem -Path "$RepoRoot\include", "$RepoRoot\src" `
    -Recurse -Include *.cpp, *.h

$files | ForEach-Object {
    clang-tidy --config-file="$RepoRoot\.clang-tidy" -p $BuildDir $_.FullName
}

Write-Host "Done."
