$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$visualStudio = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
Import-Module "$visualStudio\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $visualStudio -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'

$output = New-Item "$root\dist" -ItemType Directory -Force
cl /nologo /O2 /W4 /WX /MT /LD /std:c17 "$root\msys2-argv-fix.c" "/Fo$output\" /link /brepro "/out:$output\msys2-argv-fix.dll"
if ($LASTEXITCODE) {
    throw "cl exited with code $LASTEXITCODE"
}
