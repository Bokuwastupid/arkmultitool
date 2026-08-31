param(
    [string]$Payload = (Join-Path $PSScriptRoot '..\build-msvc\dist\kopt_payload_candidate.dll'),
    [switch]$RuntimeSmoke
)

$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

& (Join-Path $PSScriptRoot 'validate-release.ps1') -Payload $Payload
Assert-True ($LASTEXITCODE -eq 0) 'Release validation failed.'

$overlay = Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'src\overlay.cpp')
$config = Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'src\config.cpp')
$runtime = Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'src\runtime.cpp')
$payloadSource = Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'src\payload.cpp')

$catalogIds = [regex]::Matches($overlay, '\{L"([a-z0-9_.]+)",\s*L"[^\"]+",\s*L"[^\"]+",\s*&S::') |
    ForEach-Object { $_.Groups[1].Value }
$duplicates = $catalogIds | Group-Object | Where-Object Count -gt 1
Assert-True ($duplicates.Count -eq 0) ('Duplicate feature IDs: ' + (($duplicates.Name) -join ', '))

$uiMembers = [regex]::Matches($overlay,
    '(?:checkbox|toggle)\(L"[^\"]+",\s*settings\.([A-Za-z0-9_]+)') |
    ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
$catalogMembers = [regex]::Matches($overlay, '&S::([A-Za-z0-9_]+)') |
    ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
$missing = $uiMembers | Where-Object { $_ -notin $catalogMembers }
Assert-True ($missing.Count -eq 0) ('Bindable UI settings missing from feature catalog: ' + ($missing -join ', '))

foreach ($marker in @(
    'COMMAND PALETTE',
    'BIND CONFLICTS:',
    'compact_navigation',
    'settings.ui_layouts',
    'Record Aim Lab trace',
    'REPLAY SAMPLE',
    'Create diagnostics bundle'
)) {
    Assert-True ($overlay.Contains($marker)) "UI regression marker missing: $marker"
}
Assert-True ($runtime.Contains('aim_trace_count_ < aim_trace_.size()') -and
    $runtime.Contains('aim_trace_head_ = (aim_trace_head_ + 1) % aim_trace_.size()') -and
    $runtime.Contains('target_point(*best, false)')) 'Bounded Aim Lab trace or raw-bone capture is missing.'
Assert-True ($payloadSource.Contains('MiniDumpWriteDump') -and
    $payloadSource.Contains('SetUnhandledExceptionFilter')) 'Crash dump handler is missing.'
Assert-True ($config.Contains('settings_schema_version = 21U') -and
    $config.Contains('L"Favorites"') -and $config.Contains('L"ActiveLayout"')) `
    'Versioned P1 settings persistence is missing.'

if ($RuntimeSmoke) {
    & (Join-Path $PSScriptRoot 'runtime-smoke.ps1') -Payload $Payload
    Assert-True ($LASTEXITCODE -eq 0) 'Runtime smoke failed.'
}

Write-Host ('[KOPT] Regression suite passed: {0}' -f ([IO.Path]::GetFullPath($Payload)))
