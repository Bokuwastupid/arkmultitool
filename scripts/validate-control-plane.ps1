param(
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$project = Join-Path $root 'backend\Kopt.ControlPlane\Kopt.ControlPlane.csproj'
$script = Join-Path $root 'backend\Kopt.ControlPlane\wwwroot\app.js'
$migrations = Join-Path $root 'backend\Kopt.ControlPlane\Data\Migrations'

& dotnet build $project -c $Configuration --no-restore
if ($LASTEXITCODE -ne 0) { throw 'Control plane build failed.' }

& node --check $script
if ($LASTEXITCODE -ne 0) { throw 'Admin JavaScript syntax check failed.' }

$migrationCount = @(Get-ChildItem -LiteralPath $migrations -Filter '*.cs' |
    Where-Object Name -NotLike '*.Designer.cs' |
    Where-Object Name -NotLike '*ModelSnapshot.cs').Count
if ($migrationCount -lt 1) { throw 'No database migrations were found.' }

$ef = Get-Command dotnet-ef -ErrorAction SilentlyContinue
if ($null -ne $ef) {
    & dotnet ef migrations has-pending-model-changes --project $project --configuration $Configuration --no-build
    if ($LASTEXITCODE -ne 0) { throw 'EF model has changes that are not represented by a migration.' }
}
else {
    Write-Warning 'dotnet-ef is not installed; pending-model validation was skipped.'
}

Write-Host "[KOPT] Control plane validation passed ($Configuration, $migrationCount migrations)."
