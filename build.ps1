param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$PayloadName = 'kopt_payload.dll'
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio Installer (vswhere.exe) was not found.'
}
$visualStudio = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $visualStudio) {
    throw 'Visual Studio C++ Build Tools were not found.'
}
$devCmd = Join-Path $visualStudio 'Common7\Tools\VsDevCmd.bat'
$build = Join-Path $projectRoot 'build-msvc'
$dist = Join-Path $build 'dist'
$objects = Join-Path $build 'obj'
New-Item -ItemType Directory -Force -Path $dist, $objects | Out-Null
$optimization = if ($Configuration -eq 'Release') { '/O2 /DNDEBUG' } else { '/Od /Zi /DDEBUG' }
$common = '/nologo /std:c++20 /EHsc /MT /W4 /permissive- /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DKOPT_PAYLOAD_EXPORTS /I"' + (Join-Path $projectRoot 'include') + '" ' + $optimization
$compileConfig = 'cl ' + $common + ' /c "' + (Join-Path $projectRoot 'src\config.cpp') + '" /Fo"' + (Join-Path $objects 'config.obj') + '"'
$compileOverlay = 'cl ' + $common + ' /c "' + (Join-Path $projectRoot 'src\overlay.cpp') + '" /Fo"' + (Join-Path $objects 'overlay.obj') + '"'
$compilePayload = 'cl ' + $common + ' /c "' + (Join-Path $projectRoot 'src\payload.cpp') + '" /Fo"' + (Join-Path $objects 'payload.obj') + '"'
$compileRuntime = 'cl ' + $common + ' /c "' + (Join-Path $projectRoot 'src\runtime.cpp') + '" /Fo"' + (Join-Path $objects 'runtime.obj') + '"'
$payloadOutput = Join-Path $dist $PayloadName
$linkPayload = 'link /nologo /dll /out:"' + $payloadOutput + '" /implib:"' + (Join-Path $build 'kopt_payload.lib') + '" "' + (Join-Path $objects 'config.obj') + '" "' + (Join-Path $objects 'overlay.obj') + '" "' + (Join-Path $objects 'payload.obj') + '" "' + (Join-Path $objects 'runtime.obj') + '" d3d11.lib d3dcompiler.lib dxgi.lib gdi32.lib user32.lib winmm.lib ole32.lib dbghelp.lib'
$buildInjector = 'cl ' + $common + ' "' + (Join-Path $projectRoot 'src\injector.cpp') + '" /Fe:"' + (Join-Path $dist 'kopt_injector.exe') + '" /Fo"' + (Join-Path $objects 'injector.obj') + '" /link /pdb:"' + (Join-Path $build 'kopt_injector.pdb') + '" advapi32.lib psapi.lib bcrypt.lib'
$nativeCommand = '"' + $devCmd + '" -no_logo -arch=x64 -host_arch=x64 && ' + $compileConfig + ' && ' + $compileOverlay + ' && ' + $compilePayload + ' && ' + $compileRuntime + ' && ' + $linkPayload + ' && ' + $buildInjector
& cmd.exe /d /s /c $nativeCommand
if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
& (Join-Path $build 'dist\kopt_injector.exe') --self-test --dll $payloadOutput
if ($LASTEXITCODE -ne 0) { throw 'Injector/payload self-test failed.' }
Write-Host "Built KOPT Internal in $(Join-Path $build 'dist')"
