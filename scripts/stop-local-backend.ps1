$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$pidPath = Join-Path $root 'backend\local-runtime\control-plane.pid'
if (-not (Test-Path -LiteralPath $pidPath)) { Write-Host '[KOPT] Local backend is not running.'; exit 0 }
$backendPid = [int]([IO.File]::ReadAllText($pidPath).Trim())
$process = Get-Process -Id $backendPid -ErrorAction SilentlyContinue
if ($null -ne $process) {
    $commandLine = (Get-CimInstance Win32_Process -Filter "ProcessId=$backendPid").CommandLine
    if ($process.ProcessName -ne 'dotnet' -or $commandLine -notlike '*Kopt.ControlPlane.dll*') {
        throw "PID $backendPid is not the expected local KOPT backend; refusing to stop it."
    }
    Stop-Process -Id $backendPid
    $process.WaitForExit(5000) | Out-Null
}
Remove-Item -LiteralPath $pidPath -Force
Write-Host "[KOPT] Local backend stopped; PID=$backendPid"
