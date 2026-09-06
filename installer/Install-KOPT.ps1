# Per-user installer for KOPT.
#
# Deliberately per-user, under %LOCALAPPDATA%: no administrator rights, no
# writes outside the user's own profile, and no system or security settings
# touched. The configuration already lives in %LOCALAPPDATA%\KOPT (see
# user_data_directory() in src/payload.cpp), so an install, upgrade or removal
# never disturbs settings.
#
# Run it from a release bundle (binaries beside this script) or straight from a
# built tree (binaries in build-msvc\dist).

[CmdletBinding()]
param(
    # Where to install. The default needs no elevation.
    [string]$InstallRoot = (Join-Path $env:LOCALAPPDATA 'Programs\KOPT'),
    [switch]$NoDesktopShortcut,
    [switch]$NoStartMenuShortcut
)

$ErrorActionPreference = 'Stop'

$payloadFiles = @('KOPT_Inject.exe', 'kopt_injector.exe', 'kopt_payload.dll')
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

# Beside the script first (that is what a release bundle looks like), then the
# build output, so the same script serves both without being told which.
$sourceCandidates = @(
    $scriptRoot,
    (Join-Path $scriptRoot 'dist'),
    (Join-Path (Split-Path -Parent $scriptRoot) 'build-msvc\dist')
)
$source = $null
foreach ($candidate in $sourceCandidates) {
    if (-not (Test-Path -LiteralPath $candidate)) { continue }
    $missing = $payloadFiles | Where-Object { -not (Test-Path -LiteralPath (Join-Path $candidate $_)) }
    if ($missing.Count -eq 0) { $source = $candidate; break }
}
if (-not $source) {
    throw ("Could not find " + ($payloadFiles -join ', ') + " in any of: " + ($sourceCandidates -join '; ') +
        ". Build first with build.ps1, or run this from a release bundle.")
}
Write-Host "Installing from: $source"

New-Item -ItemType Directory -Force -Path $InstallRoot | Out-Null
foreach ($file in $payloadFiles) {
    Copy-Item -LiteralPath (Join-Path $source $file) -Destination (Join-Path $InstallRoot $file) -Force
}

# Copied rather than generated so the uninstaller stays reviewable and can be
# run on its own.
$uninstaller = Join-Path $scriptRoot 'Uninstall-KOPT.ps1'
if (Test-Path -LiteralPath $uninstaller) {
    Copy-Item -LiteralPath $uninstaller -Destination (Join-Path $InstallRoot 'Uninstall-KOPT.ps1') -Force
}

function New-Shortcut {
    param([string]$LinkPath, [string]$TargetPath, [string]$WorkingDirectory)
    $shell = New-Object -ComObject WScript.Shell
    try {
        $shortcut = $shell.CreateShortcut($LinkPath)
        $shortcut.TargetPath = $TargetPath
        $shortcut.WorkingDirectory = $WorkingDirectory
        $shortcut.Description = 'KOPT'
        $shortcut.Save()
    } finally {
        [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($shell)
    }
}

$injector = Join-Path $InstallRoot 'KOPT_Inject.exe'
if (-not $NoStartMenuShortcut) {
    $startMenu = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs'
    New-Item -ItemType Directory -Force -Path $startMenu | Out-Null
    New-Shortcut -LinkPath (Join-Path $startMenu 'KOPT.lnk') -TargetPath $injector -WorkingDirectory $InstallRoot
    Write-Host 'Start Menu shortcut created.'
}
if (-not $NoDesktopShortcut) {
    $desktop = [Environment]::GetFolderPath('Desktop')
    New-Shortcut -LinkPath (Join-Path $desktop 'KOPT.lnk') -TargetPath $injector -WorkingDirectory $InstallRoot
    Write-Host 'Desktop shortcut created.'
}

# HKCU only -- this is the per-user Apps & Features list, so the install can be
# removed the same way any other application is. Nothing machine-wide is touched.
$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\KOPT'
New-Item -Path $uninstallKey -Force | Out-Null
$uninstallCommand = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "' +
    (Join-Path $InstallRoot 'Uninstall-KOPT.ps1') + '"'
New-ItemProperty -Path $uninstallKey -Name 'DisplayName' -Value 'KOPT' -PropertyType String -Force | Out-Null
New-ItemProperty -Path $uninstallKey -Name 'DisplayIcon' -Value $injector -PropertyType String -Force | Out-Null
New-ItemProperty -Path $uninstallKey -Name 'InstallLocation' -Value $InstallRoot -PropertyType String -Force | Out-Null
New-ItemProperty -Path $uninstallKey -Name 'UninstallString' -Value $uninstallCommand -PropertyType String -Force | Out-Null
New-ItemProperty -Path $uninstallKey -Name 'NoModify' -Value 1 -PropertyType DWord -Force | Out-Null
New-ItemProperty -Path $uninstallKey -Name 'NoRepair' -Value 1 -PropertyType DWord -Force | Out-Null

Write-Host ''
Write-Host "KOPT installed to: $InstallRoot"
Write-Host "Configuration and log stay in: $(Join-Path $env:LOCALAPPDATA 'KOPT')"
Write-Host 'Launch KOPT_Inject.exe with the game already running.'
