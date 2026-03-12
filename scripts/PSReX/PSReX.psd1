@{
    RootModule        = 'PSReX.psm1'
    ModuleVersion     = '0.1.0'
    GUID              = 'a1b2c3d4-e5f6-7890-abcd-ef1234567890'
    Author            = 'Tom Clay'
    Description       = 'ReXGlue SDK dev tooling'
    PowerShellVersion = '7.0'
    FunctionsToExport = @(
        'Invoke-ReXConfigure'
        'Invoke-ReXBuild'
        'Invoke-ReXTest'
        'Invoke-ReXInstall'
        'Invoke-ReXFormat'
        'Invoke-ReXLint'
        'Invoke-ReXSetup'
    )
    AliasesToExport   = @(
        'rex-configure'
        'rex-build'
        'rex-test'
        'rex-install'
        'rex-format'
        'rex-lint'
        'rex-setup'
    )
    CmdletsToExport   = @()
    VariablesToExport  = @()
}
