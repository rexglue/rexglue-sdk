<#
.SYNOPSIS
    PSReX module root - dot-sources helpers and public functions, registers aliases.
.NOTES
    Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
    All rights reserved.
    Licensed under the BSD 3-Clause License.
    See LICENSE file in the project root for full license text.
#>
#Requires -Version 7.0

$Private = @(Get-ChildItem -Path "$PSScriptRoot/Private/*.ps1" -ErrorAction SilentlyContinue)
foreach ($file in $Private) { . $file.FullName }

$Public = @(Get-ChildItem -Path "$PSScriptRoot/Public/*.ps1" -ErrorAction SilentlyContinue)
foreach ($file in $Public) { . $file.FullName }

# Ensure VS developer tools are on PATH (no-op if already active or not on Windows)
Initialize-VsEnvironment

# Convenience aliases
New-Alias -Name 'rex-configure' -Value 'Invoke-ReXConfigure'
New-Alias -Name 'rex-build'     -Value 'Invoke-ReXBuild'
New-Alias -Name 'rex-test'      -Value 'Invoke-ReXTest'
New-Alias -Name 'rex-install'   -Value 'Invoke-ReXInstall'
New-Alias -Name 'rex-format'    -Value 'Invoke-ReXFormat'
New-Alias -Name 'rex-lint'      -Value 'Invoke-ReXLint'
New-Alias -Name 'rex-setup'     -Value 'Invoke-ReXSetup'

Export-ModuleMember -Function $Public.BaseName -Alias @(
    'rex-configure', 'rex-build', 'rex-test', 'rex-install',
    'rex-format', 'rex-lint', 'rex-setup'
)
