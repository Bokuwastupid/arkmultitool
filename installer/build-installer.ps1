# Compiles installer\kopt.iss into a single setup .exe.
#
# Inno Setup is not vendored and is not a build dependency of the project --
# only of this optional packaging step -- so a missing ISCC.exe is reported as
# what to install, not as a build failure with a compiler error in it.

[CmdletBinding()]
param(
    [string]$Version = '0.1.0',
    # Defaults to the tree's own build output.
    [string]$SourceDir
)

$ErrorActionPreference = 'Stop'
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $scriptRoot
if (-not $SourceDir) { $SourceDir = Join-Path $projectRoot 'build-msvc\dist' }

foreach ($file in @('KOPT_Inject.exe', 'kopt_injector.exe', 'kopt_payload.dll')) {
    $path = Join-Path $SourceDir $file
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Missing $path -- run build.ps1 first, or pass -SourceDir."
    }
}

$candidates = @(
    'C:\Program Files (x86)\Inno Setup 6\ISCC.exe',
    'C:\Program Files\Inno Setup 6\ISCC.exe'
)
$iscc = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $iscc) {
    $onPath = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($onPath) { $iscc = $onPath.Source }
}
if (-not $iscc) {
    throw ('Inno Setup (ISCC.exe) was not found. Install it from ' +
        'https://jrsoftware.org/isdl.php, or use installer\Install-KOPT.cmd, ' +
        'which needs no extra tooling.')
}

$outputDir = Join-Path $projectRoot 'build-msvc\installer'
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

& $iscc "/DSourceDir=$SourceDir" "/DAppVersion=$Version" (Join-Path $scriptRoot 'kopt.iss')
if ($LASTEXITCODE -ne 0) { throw 'Inno Setup compilation failed.' }

Write-Host ''
Write-Host ("Installer written to: " + (Join-Path $outputDir ("KOPT-Setup-$Version.exe")))
