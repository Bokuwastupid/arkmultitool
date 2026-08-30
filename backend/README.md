# KOPT Control Plane

ASP.NET Core 10 + PostgreSQL control plane for accounts, license keys,
subscriptions, device slots, renewable leases, tamper incidents and admin audit.

## Local production-shaped start

Generate fresh secrets outside source control:

```powershell
.\scripts\generate-backend-secrets.ps1 -OutputPath .\backend\.env
```

Change the generated bootstrap email/password, then start from `backend`:

```powershell
docker compose --env-file .env up --build
```

The panel is exposed on `http://127.0.0.1:5087`. Put it behind a TLS reverse
proxy before exposing it outside the host. Delete the bootstrap password from
the runtime environment after the first administrator has been created.

The checked-in appsettings file intentionally contains unusable placeholders.
Production startup fails closed unless a 32-byte hash pepper and an ECDSA P-256
private signing key are supplied through environment variables or a secret
provider.

## Database migrations

The container can apply checked-in migrations on startup with
`Database__AutoMigrate=true`. For controlled deployments, disable it and run:

```powershell
dotnet ef database update --project .\Kopt.ControlPlane\Kopt.ControlPlane.csproj
```

## Key handling

- license plaintext is returned once; only an HMAC digest is stored;
- the capability private key stays backend-only;
- embed only `KOPT_CAPABILITY_PUBLIC_KEY_SPKI` in the loader;
- persist Data Protection keys on an encrypted/permission-restricted volume;
- use a cloud secret manager and KMS/HSM-backed signing before public sales.

## Operational rules

- keep admin MFA enabled behind the final identity provider;
- protect the panel with TLS, strict host filtering and a trusted proxy config;
- back up PostgreSQL and test restores;
- retain audit events independently from mutable product data;
- quarantine/revoke on multi-signal tamper; never delete user files.
