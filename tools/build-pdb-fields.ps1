# Builds tools/pdb_fields.cpp with the same VsDevCmd shell that build.ps1 uses.
# Kept as a script rather than a one-liner because the VsDevCmd call and the
# compile have to run in one cmd invocation -- the environment it sets does not
# survive back into PowerShell.
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'vswhere.exe was not found.' }
$visualStudio = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $visualStudio) { throw 'Visual Studio C++ Build Tools were not found.' }
$devCmd = Join-Path $visualStudio 'Common7\Tools\VsDevCmd.bat'
$build = Join-Path $projectRoot 'build-msvc'
$objects = Join-Path $build 'obj'
New-Item -ItemType Directory -Force -Path $build, $objects | Out-Null
$source = Join-Path $projectRoot 'tools\pdb_fields.cpp'
$output = Join-Path $build 'pdb_fields.exe'
$compile = 'cl /nologo /std:c++20 /EHsc /MT /W4 /permissive- /utf-8 /DUNICODE /D_UNICODE /O2 "' + $source + '" /Fe:"' + $output + '" /Fo:"' + (Join-Path $objects 'pdb_fields.obj') + '" /link dbghelp.lib'
& cmd.exe /d /s /c ('"' + $devCmd + '" -no_logo -arch=x64 -host_arch=x64 && ' + $compile)
if ($LASTEXITCODE -ne 0) { throw 'pdb_fields build failed.' }
Write-Output ('Built ' + $output)
