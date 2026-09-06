@echo off
rem Double-clickable entry point. PowerShell scripts do not run from Explorer
rem under the default execution policy, so this launches the installer with the
rem bypass applied to this one invocation only -- the machine's policy is not
rem changed.
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-KOPT.ps1" %*
if errorlevel 1 (
    echo.
    echo Installation failed. See the message above.
)
echo.
pause
