param(
    [ValidateSet('win-x64', 'linux-x64')]
    [string]$Runtime = 'win-x64',
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$project = Join-Path $root 'loader\Kopt.Loader\Kopt.Loader.csproj'
$output = Join-Path $root "loader\dist\$Runtime"
$candidate = Join-Path $root 'build-msvc\dist\kopt_payload_candidate.dll'
$injector = Join-Path $root 'build-msvc\dist\kopt_injector.exe'

if ($Runtime -eq 'win-x64') {
    & (Join-Path $PSScriptRoot 'validate-release.ps1') -Payload $candidate
    if ($LASTEXITCODE -ne 0) { throw 'Native release validation failed.' }
}

dotnet publish $project --configuration $Configuration --runtime $Runtime --self-contained false `
    --output $output
if ($LASTEXITCODE -ne 0) { throw 'Loader publish failed.' }

if ($Runtime -eq 'win-x64') {
    Copy-Item -LiteralPath $candidate -Destination (Join-Path $output 'kopt_payload_candidate.dll') -Force
    Copy-Item -LiteralPath $injector -Destination (Join-Path $output 'kopt_injector.exe') -Force
}

$artifacts = Get-ChildItem -LiteralPath $output -File | Sort-Object Name | ForEach-Object {
    [ordered]@{
        name = $_.Name
        size = $_.Length
        sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash
    }
}
$manifest = [ordered]@{
    schema = 1
    runtime = $Runtime
    configuration = $Configuration
    generatedAt = [DateTimeOffset]::UtcNow.ToString('O')
    signed = $false
    channel = 'local-development'
    artifacts = $artifacts
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'release-manifest.local.json') -Encoding UTF8

dotnet (Join-Path $output 'Kopt.Loader.dll') --self-test
if ($LASTEXITCODE -ne 0) { throw 'Published loader self-test failed.' }
Write-Host "[KOPT] Loader bundle built: $output"
