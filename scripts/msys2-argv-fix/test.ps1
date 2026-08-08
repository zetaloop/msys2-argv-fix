$root = New-Item (Join-Path $env:TEMP ([guid]::NewGuid())) -ItemType Directory
$dir = $root.FullName
$persist_dir = $dir
$version = '0.0.1'
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
    Copy-Item "$PSScriptRoot\..\..\dist\msys2-argv-fix.dll" $dir
    $json = (Get-Content "$PSScriptRoot\manifest.json.template" -Raw).Replace('@VERSION@', $version).Replace('@HASH@', ('0' * 64))
    $manifest = $json | ConvertFrom-Json

    & ([scriptblock]::Create($manifest.pre_install))
    & ([scriptblock]::Create($manifest.post_install -join "`r`n"))

    $prefix = $dir.Replace('\', '/')
    $prefix = "/proc/cygdrive/$($prefix[0].ToString().ToLower())$($prefix.Substring(2))"
    $entry = "$prefix/preload/msys2-argv-fix-$version.dll"
    if ($stored -ne "/existing/preload.dll:$entry") { throw "Unexpected LD_PRELOAD: $stored" }
    if (!(Test-Path "$dir\preload\msys2-argv-fix-$version.dll")) { throw 'Persisted DLL is missing.' }

    & ([scriptblock]::Create($manifest.uninstaller.script -join "`r`n"))
    if ($stored -ne '/existing/preload.dll') { throw "Unexpected LD_PRELOAD after uninstall: $stored" }
} finally {
    Remove-Item $root -Recurse -Force
}
