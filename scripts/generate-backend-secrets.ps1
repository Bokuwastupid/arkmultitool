param(
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
$pepper = [Convert]::ToBase64String([Security.Cryptography.RandomNumberGenerator]::GetBytes(32))
$databasePassword = [Convert]::ToBase64String([Security.Cryptography.RandomNumberGenerator]::GetBytes(36))
$adminPassword = [Convert]::ToBase64String([Security.Cryptography.RandomNumberGenerator]::GetBytes(36)) + '!aA1'
$signer = [Security.Cryptography.ECDsa]::Create()
$signer.GenerateKey([Security.Cryptography.ECCurve]::CreateFromFriendlyName('nistP256'))
try {
    $privateKey = [Convert]::ToBase64String($signer.ExportPkcs8PrivateKey())
    $publicKey = [Convert]::ToBase64String($signer.ExportSubjectPublicKeyInfo())
}
finally { if ($null -ne $signer) { $signer.Dispose() } }

$content = @"
KOPT_POSTGRES_PASSWORD=$databasePassword
KOPT_HASH_PEPPER=$pepper
KOPT_CAPABILITY_PRIVATE_KEY_PKCS8=$privateKey
KOPT_CAPABILITY_PUBLIC_KEY_SPKI=$publicKey
KOPT_BOOTSTRAP_ADMIN_EMAIL=admin@example.invalid
KOPT_BOOTSTRAP_ADMIN_PASSWORD=$adminPassword
"@

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $content
    Write-Warning 'Secrets were printed once. Store them in a secret manager, not in source control.'
    exit 0
}

$resolved = [IO.Path]::GetFullPath($OutputPath)
[IO.File]::WriteAllText($resolved, $content, [Text.UTF8Encoding]::new($false))
Write-Host "Wrote $resolved"
Write-Warning 'Move this file into a secret manager and delete the plaintext copy after deployment.'
