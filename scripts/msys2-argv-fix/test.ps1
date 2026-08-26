$root = New-Item (Join-Path $env:TEMP ([guid]::NewGuid())) -ItemType Directory
$dir = $root.FullName
$global = $false
$stored = '/existing/preload.dll'

function Get-EnvVar {
    param([string] $Name, [switch] $Global)
    $script:stored
}

function Set-EnvVar {
    param([string] $Name, [string] $Value, [switch] $Global)
    $script:stored = $Value
}

try {
    $manifest = Get-Content "$PSScriptRoot\manifest.json.template" -Raw | ConvertFrom-Json
    & ([scriptblock]::Create($manifest.post_install -join "`r`n"))

    $prefix = $dir.Replace('\', '/')
    $prefix = "/proc/cygdrive/$($prefix[0].ToString().ToLower())$($prefix.Substring(2))"
    $entry = "$prefix/msys2-argv-fix.dll"
    if ($stored -ne "/existing/preload.dll:$entry") { throw "Unexpected LD_PRELOAD: $stored" }

    & ([scriptblock]::Create($manifest.uninstaller.script -join "`r`n"))
    if ($stored -ne '/existing/preload.dll') { throw "Unexpected LD_PRELOAD after uninstall: $stored" }
} finally {
    Remove-Item $root -Recurse -Force
}
