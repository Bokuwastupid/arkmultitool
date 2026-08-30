param(
    [string]$Payload = (Join-Path $PSScriptRoot '..\build-msvc\dist\kopt_payload_candidate.dll'),
    [int]$WaitForGameSeconds = 0,
    [int]$StartupTimeoutSeconds = 45
)

$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$payloadPath = [IO.Path]::GetFullPath($Payload)
$injector = Join-Path $projectRoot 'build-msvc\dist\kopt_injector.exe'
$logPath = Join-Path ([IO.Path]::GetDirectoryName($payloadPath)) 'kopt_internal.log'

& (Join-Path $PSScriptRoot 'validate-release.ps1') -Payload $payloadPath
if ($LASTEXITCODE -ne 0) { throw 'Static release validation failed.' }

$deadline = [DateTime]::UtcNow.AddSeconds([Math]::Max(0, $WaitForGameSeconds))
do {
    $game = Get-Process -Name ShooterGame -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $game) { break }
    if ([DateTime]::UtcNow -ge $deadline) { throw 'ShooterGame.exe is not running.' }
    Start-Sleep -Milliseconds 500
} while ($true)

$initialLogLength = if (Test-Path -LiteralPath $logPath) { (Get-Item -LiteralPath $logPath).Length } else { 0L }
$injected = $false
try {
    & $injector --pid $game.Id --dll $payloadPath
    if ($LASTEXITCODE -ne 0) { throw "Injection failed with exit code $LASTEXITCODE." }
    $injected = $true

    $markers = @(
        'Payload worker started',
        'D3D11 overlay initialized',
        'Camera game-tick hook installed',
        'World generation changed:',
        'Local player runtime became valid'
    )
    $startupDeadline = [DateTime]::UtcNow.AddSeconds([Math]::Max(5, $StartupTimeoutSeconds))
    $newLog = ''
    do {
        if (Test-Path -LiteralPath $logPath) {
            $bytes = [IO.File]::ReadAllBytes($logPath)
            if ($bytes.Length -gt $initialLogLength) {
                $newLog = [Text.Encoding]::Unicode.GetString(
                    $bytes,
                    [int]$initialLogLength,
                    [int]($bytes.Length - $initialLogLength))
            }
        }
        $missing = @($markers | Where-Object { -not $newLog.Contains($_) })
        if ($missing.Count -eq 0) { break }
        if ($game.HasExited) { throw 'ShooterGame.exe exited during runtime startup smoke.' }
        Start-Sleep -Milliseconds 500
    } while ([DateTime]::UtcNow -lt $startupDeadline)

    $missing = @($markers | Where-Object { -not $newLog.Contains($_) })
    if ($missing.Count -gt 0) {
        throw ('Runtime startup smoke timed out. Missing markers: ' + ($missing -join ', '))
    }
}
catch {
    $failure = $_
    if ($injected -and -not $game.HasExited) {
        & $injector --pid $game.Id --unload
        if ($LASTEXITCODE -ne 0) {
            Write-Warning "Automatic candidate rollback failed with exit code $LASTEXITCODE; use End or --unload before retrying."
        }
    }
    throw $failure
}

Write-Host "[KOPT] Runtime startup smoke passed for PID $($game.Id)."
Write-Host '[KOPT] Manual interaction gate: Home twice, click every widget once, rebind Alt/Ctrl, aim press/release, freecam, reconnect, End unload.'
