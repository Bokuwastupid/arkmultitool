# Removes what Install-KOPT.ps1 created.
#
# Settings and the log are left alone by default: they live in
# %LOCALAPPDATA%\KOPT, they are the user's own data, and a reinstall is
# expected to find them. -RemoveSettings deletes them explicitly.

[CmdletBinding()]
param(
    [string]$InstallRoot = (Join-Path $env:LOCALAPPDATA 'Programs\KOPT'),
    [switch]$RemoveSettings
)

$ErrorActionPreference = 'Stop'

foreach ($shortcut in @(
    (Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\KOPT.lnk'),
    (Join-Path ([Environment]::GetFolderPath('Desktop')) 'KOPT.lnk'))) {
    if (Test-Path -LiteralPath $shortcut) {
        Remove-Item -LiteralPath $shortcut -Force
        Write-Host "Removed shortcut: $shortcut"
    }
}

$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\KOPT'
if (Test-Path -LiteralPath $uninstallKey) {
    Remove-Item -LiteralPath $uninstallKey -Recurse -Force
    Write-Host 'Removed Apps & Features entry.'
}

if (Test-Path -LiteralPath $InstallRoot) {
    # The payload may still be loaded in a running game, which holds the DLL
    # open. Say so plainly instead of failing with a bare access-denied.
    try {
        Remove-Item -LiteralPath $InstallRoot -Recurse -Force
        Write-Host "Removed: $InstallRoot"
    } catch {
        Write-Warning ("Could not remove $InstallRoot -- if the payload is still injected, " +
            'unload it (END in the overlay) or close the game, then run this again.')
        throw
    }
}

$settings = Join-Path $env:LOCALAPPDATA 'KOPT'
if ($RemoveSettings) {
    if (Test-Path -LiteralPath $settings) {
        Remove-Item -LiteralPath $settings -Recurse -Force
        Write-Host "Removed settings: $settings"
    }
} elseif (Test-Path -LiteralPath $settings) {
    Write-Host "Settings kept at: $settings  (re-run with -RemoveSettings to delete them)"
}

Write-Host 'KOPT uninstalled.'
