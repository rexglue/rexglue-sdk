<#
.SYNOPSIS
    Auto-detect the rexglue-sdk repository root.
.NOTES
    Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
    All rights reserved.
    Licensed under the BSD 3-Clause License.
    See LICENSE file in the project root for full license text.
#>
function Get-ReXRoot {
    if ($env:REXGLUE_ROOT) { return $env:REXGLUE_ROOT }
    $root = git rev-parse --show-toplevel 2>$null
    if ($root) { return [System.IO.Path]::GetFullPath($root) }
    # Fallback: module is at <repo>/scripts/PSReX/
    return (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
}
