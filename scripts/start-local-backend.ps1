param(
    [string]$Email = 'loader@local.test',
    [string]$Password = $env:KOPT_LOCAL_ADMIN_PASSWORD,
    [int]$Port = 5087
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($Password)) {
    $Password = [Convert]::ToBase64String(
        [Security.Cryptography.RandomNumberGenerator]::GetBytes(24)) + '!aA1'
}
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$project = Join-Path $root 'backend\Kopt.ControlPlane\Kopt.ControlPlane.csproj'
$runtime = Join-Path $root 'backend\local-runtime'
$database = Join-Path $runtime 'kopt-local.db'
$pidPath = Join-Path $runtime 'control-plane.pid'
$logPath = Join-Path $runtime 'control-plane.log'
$errorPath = Join-Path $runtime 'control-plane.error.log'
$secretsPath = Join-Path $runtime 'local-secrets.json'
$assembly = Join-Path $root 'backend\Kopt.ControlPlane\bin\Release\net10.0\Kopt.ControlPlane.dll'

[IO.Directory]::CreateDirectory($runtime) | Out-Null
if (Test-Path -LiteralPath $pidPath) {
    $existingPid = [int]([IO.File]::ReadAllText($pidPath).Trim())
    $existing = Get-Process -Id $existingPid -ErrorAction SilentlyContinue
    if ($null -ne $existing -and $existing.ProcessName -eq 'dotnet') {
        Write-Host "[KOPT] Local backend is already running; PID=$existingPid"
        exit 0
    }
}

dotnet build $project --configuration Release
if ($LASTEXITCODE -ne 0) { throw 'Backend build failed.' }

if (Test-Path -LiteralPath $secretsPath) {
    $secrets = Get-Content -Raw -LiteralPath $secretsPath | ConvertFrom-Json
    $pepper = $secrets.hashPepper
    $privateKey = $secrets.capabilityPrivateKeyPkcs8
}
else {
    $pepper = [Convert]::ToBase64String([Security.Cryptography.RandomNumberGenerator]::GetBytes(32))
    $signer = [Security.Cryptography.ECDsa]::Create()
    $signer.GenerateKey([Security.Cryptography.ECCurve]::CreateFromFriendlyName('nistP256'))
    try { $privateKey = [Convert]::ToBase64String($signer.ExportPkcs8PrivateKey()) }
    finally { if ($null -ne $signer) { $signer.Dispose() } }
    [IO.File]::WriteAllText($secretsPath, (@{
        hashPepper = $pepper
        capabilityPrivateKeyPkcs8 = $privateKey
    } | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
}

$childEnvironment = [ordered]@{
    ASPNETCORE_ENVIRONMENT = 'Development'
    ASPNETCORE_URLS = "http://127.0.0.1:$Port"
    ConnectionStrings__ControlPlane = "Data Source=$database"
    Database__Provider = 'Sqlite'
    Database__AutoMigrate = 'true'
    Security__HashPepper = $pepper
    Security__CapabilityPrivateKeyPkcs8 = $privateKey
    Security__DataProtectionPath = (Join-Path $runtime 'data-protection')
    KOPT_BOOTSTRAP_ADMIN_EMAIL = $Email
    KOPT_BOOTSTRAP_ADMIN_PASSWORD = $Password
    KOPT_BOOTSTRAP_SUBSCRIPTION_DAYS = '30'
}
$previousEnvironment = @{}
foreach ($entry in $childEnvironment.GetEnumerator()) {
    $previousEnvironment[$entry.Key] = [Environment]::GetEnvironmentVariable($entry.Key, 'Process')
    [Environment]::SetEnvironmentVariable($entry.Key, [string]$entry.Value, 'Process')
}
try {
    $process = Start-Process -FilePath (Get-Command dotnet).Source -ArgumentList @($assembly) `
        -WorkingDirectory (Split-Path -Parent $assembly) -WindowStyle Hidden `
        -RedirectStandardOutput $logPath -RedirectStandardError $errorPath -PassThru
}
finally {
    foreach ($entry in $previousEnvironment.GetEnumerator()) {
        [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, 'Process')
    }
}
[IO.File]::WriteAllText($pidPath, $process.Id.ToString(), [Text.UTF8Encoding]::new($false))

$deadline = [DateTime]::UtcNow.AddSeconds(30)
$health = $null
do {
    if ($process.HasExited) {
        throw "Local backend exited with code $($process.ExitCode). See $errorPath"
    }
    try {
        $health = Invoke-WebRequest -UseBasicParsing "http://127.0.0.1:$Port/health" -TimeoutSec 2
        if ($health.StatusCode -eq 200) { break }
    }
    catch { Start-Sleep -Milliseconds 300 }
} while ([DateTime]::UtcNow -lt $deadline)

if ($null -eq $health -or $health.StatusCode -ne 200) { throw 'Local backend health check timed out.' }
Write-Host "[KOPT] Local backend ready: http://127.0.0.1:$Port"
Write-Host "[KOPT] Loader login: $Email"
Write-Host "[KOPT] Loader password: $Password"
Write-Host "[KOPT] PID=$($process.Id) database=$database"
