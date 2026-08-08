#Requires -Version 5.1
#Requires -Modules @{ ModuleName = 'BuildHelpers'; ModuleVersion = '2.0.1' }
#Requires -Modules @{ ModuleName = 'Pester'; ModuleVersion = '5.2.0' }

Remove-Item Env:CI -ErrorAction Ignore
$pesterConfig = New-PesterConfiguration -Hashtable @{
    Run    = @{
        Path     = "$PSScriptRoot/.."
        PassThru = $true
    }
    Output = @{
        Verbosity = 'Detailed'
    }
}
$result = Invoke-Pester -Configuration $pesterConfig
if ($result.Result -ne 'Passed') {
    exit 1
}
