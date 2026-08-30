using System.Security.Claims;
using System.Data;
using System.Text.Json;
using System.Security.Cryptography;
using System.Text;
using Kopt.ControlPlane.Data;
using Kopt.ControlPlane.Domain;
using Kopt.ControlPlane.Security;
using Microsoft.EntityFrameworkCore;

namespace Kopt.ControlPlane.Endpoints;

public static class ClientEndpoints
{
    public sealed record ActivateRequest(string Code);
    public sealed record AcquireLeaseRequest(string Product, string DeviceFingerprint, string DeviceName,
        string SessionId, string Nonce, string Channel = "stable");
    public sealed record RenewLeaseRequest(string SessionId, string Nonce, string Channel = "stable");
    public sealed record TamperRequest(string Code, string EvidenceDigest, int RiskScore, string[] Signals,
        Guid? DeviceId);

    public static IEndpointRouteBuilder MapClientEndpoints(this IEndpointRouteBuilder routes)
    {
        var group = routes.MapGroup("/api/client").RequireAuthorization();
        group.MapGet("/entitlements", GetEntitlements);
        group.MapPost("/activate", Activate);
        group.MapPost("/leases/acquire", AcquireLease);
        group.MapPost("/leases/renew", RenewLease);
        group.MapPost("/leases/release/{sessionId}", ReleaseLease);
        group.MapPost("/tamper", ReportTamper).RequireRateLimiting("tamper");
        group.MapGet("/releases/{productSlug}/{channel}", GetRelease);
        return routes;
    }

    private static Guid UserId(ClaimsPrincipal user) =>
        Guid.Parse(user.FindFirstValue(ClaimTypes.NameIdentifier)
            ?? throw new InvalidOperationException("Authenticated user has no subject."));

    private static async Task<IResult> GetEntitlements(ClaimsPrincipal user, KoptDbContext db, TimeProvider clock)
    {
        var userId = UserId(user);
        var now = clock.GetUtcNow();
        var result = await db.Subscriptions.AsNoTracking()
            .Where(x => x.UserId == userId)
            .Join(db.Products, subscription => subscription.ProductId, product => product.Id,
                (subscription, product) => new
                {
                    product.Slug,
                    product.Name,
                    ProductEnabled = product.Enabled && !product.EmergencyStop,
                    subscription.StartsAt,
                    subscription.EndsAt,
                    subscription.SlotLimit,
                    Active = subscription.RevokedAt == null && subscription.PausedAt == null &&
                        subscription.StartsAt <= now && subscription.EndsAt > now
                }).ToListAsync();
        return Results.Ok(result);
    }

    private static async Task<IResult> Activate(ActivateRequest request, ClaimsPrincipal principal,
        KoptDbContext db, SecretHasher hasher, TimeProvider clock)
    {
        if (string.IsNullOrWhiteSpace(request.Code) || request.Code.Length > 128)
            return Invalid("code", "A license code up to 128 characters is required.");
        var userId = UserId(principal);
        var user = await db.Users.SingleAsync(x => x.Id == userId);
        if (user.Quarantined) return Results.Problem("Account is quarantined.", statusCode: 423);
        await using var transaction = await db.Database.BeginTransactionAsync(IsolationLevel.Serializable);
        var hash = hasher.Hash(request.Code.Trim());
        var key = await db.LicenseKeys.SingleOrDefaultAsync(x => x.CodeHash == hash);
        if (key is null || key.RevokedAt is not null || key.RedeemedAt is not null)
            return Results.Problem("License key is invalid or already used.", statusCode: 400);
        var now = clock.GetUtcNow();
        var subscription = await db.Subscriptions.SingleOrDefaultAsync(x =>
            x.UserId == userId && x.ProductId == key.ProductId);
        if (subscription is null)
        {
            subscription = new Subscription
            {
                UserId = userId,
                ProductId = key.ProductId,
                StartsAt = now,
                EndsAt = now.AddDays(key.DurationDays),
                SlotLimit = key.SlotLimit
            };
            db.Subscriptions.Add(subscription);
        }
        else
        {
            subscription.EndsAt = (subscription.EndsAt > now ? subscription.EndsAt : now).AddDays(key.DurationDays);
            subscription.SlotLimit = Math.Max(subscription.SlotLimit, key.SlotLimit);
            subscription.RevokedAt = null;
            subscription.PausedAt = null;
            subscription.Version++;
        }
        key.RedeemedAt = now;
        key.RedeemedBy = userId;
        db.AuditEvents.Add(Audit(userId, "license.activate", nameof(LicenseKey), key.Id, "User redemption"));
        await db.SaveChangesAsync();
        await transaction.CommitAsync();
        return Results.Ok(new { subscription.Id, subscription.EndsAt, subscription.SlotLimit });
    }

    private static async Task<IResult> AcquireLease(AcquireLeaseRequest request, ClaimsPrincipal principal,
        KoptDbContext db, SecretHasher hasher, CapabilitySigner signer, TimeProvider clock, IConfiguration config)
    {
        var validation = ValidateLeaseInput(request.Product, request.DeviceFingerprint, request.DeviceName,
            request.SessionId, request.Nonce, request.Channel);
        if (validation is not null) return validation;
        var productSlug = request.Product.Trim().ToLowerInvariant();
        var deviceFingerprint = request.DeviceFingerprint.Trim();
        var deviceName = request.DeviceName.Trim();
        var sessionId = request.SessionId.Trim();
        var nonce = request.Nonce.Trim();
        var channel = request.Channel.Trim().ToLowerInvariant();
        var userId = UserId(principal);
        var now = clock.GetUtcNow();
        await using var transaction = await db.Database.BeginTransactionAsync(IsolationLevel.Serializable);
        var user = await db.Users.SingleAsync(x => x.Id == userId);
        if (user.Quarantined) return Results.Problem("Account is quarantined.", statusCode: 423);
        var product = await db.Products.SingleOrDefaultAsync(x => x.Slug == productSlug);
        if (product is null) return Results.NotFound();
        if (!product.Enabled || product.EmergencyStop)
            return Results.Problem(product.EmergencyReason ?? "Product is unavailable.", statusCode: 503);
        var subscription = await db.Subscriptions.SingleOrDefaultAsync(x => x.UserId == userId && x.ProductId == product.Id);
        if (subscription is null || subscription.RevokedAt is not null || subscription.PausedAt is not null ||
            subscription.StartsAt > now || subscription.EndsAt <= now)
            return Results.Problem("No active entitlement.", statusCode: 403);

        if (await db.Leases.AnyAsync(x => x.SessionId == sessionId))
            return Results.Conflict(new { error = "SessionId has already been used." });
        var fingerprint = hasher.Hash(deviceFingerprint);
        var device = await db.Devices.SingleOrDefaultAsync(x => x.UserId == userId &&
            x.ProductId == product.Id && x.FingerprintHash == fingerprint);
        if (device is null)
        {
            var occupied = await db.Devices.CountAsync(x => x.UserId == userId && x.ProductId == product.Id && x.RevokedAt == null);
            if (occupied >= subscription.SlotLimit)
                return Results.Problem("All device slots are occupied.", statusCode: 409);
            device = new Device
            {
                UserId = userId,
                ProductId = product.Id,
                FingerprintHash = fingerprint,
                DisplayName = deviceName,
                EnrolledAt = now,
                LastSeenAt = now
            };
            db.Devices.Add(device);
        }
        if (device.RevokedAt is not null) return Results.Problem("Device is revoked.", statusCode: 403);
        device.LastSeenAt = now;
        var existing = await db.Leases.Where(x => x.SubscriptionId == subscription.Id && x.DeviceId == device.Id &&
            x.RevokedAt == null && x.ExpiresAt > now).ToListAsync();
        foreach (var old in existing) { old.RevokedAt = now; old.RevokeReason = "Superseded"; }
        var minutes = Math.Clamp(config.GetValue("Lease:LifetimeMinutes", 5), 1, 30);
        var lease = new Lease
        {
            SubscriptionId = subscription.Id,
            DeviceId = device.Id,
            SessionId = sessionId,
            NonceHash = hasher.Hash(nonce),
            IssuedAt = now,
            ExpiresAt = now.AddMinutes(minutes)
        };
        db.Leases.Add(lease);
        await db.SaveChangesAsync();
        await transaction.CommitAsync();
        var token = signer.Sign(new Capability(lease.Id, userId, product.Id, device.Id,
            lease.SessionId, channel, lease.ExpiresAt));
        return Results.Ok(new { capability = token, lease.ExpiresAt, device.Id, subscription.EndsAt });
    }

    private static async Task<IResult> RenewLease(RenewLeaseRequest request, ClaimsPrincipal principal,
        KoptDbContext db, SecretHasher hasher, CapabilitySigner signer, TimeProvider clock, IConfiguration config)
    {
        var validation = ValidateRenewInput(request.SessionId, request.Nonce, request.Channel);
        if (validation is not null) return validation;
        var sessionId = request.SessionId.Trim();
        var nonce = request.Nonce.Trim();
        var channel = request.Channel.Trim().ToLowerInvariant();
        var userId = UserId(principal);
        var now = clock.GetUtcNow();
        var lease = await db.Leases.SingleOrDefaultAsync(x => x.SessionId == sessionId);
        if (lease is null || lease.RevokedAt is not null || lease.NonceHash != hasher.Hash(nonce))
            return Results.Problem("Lease is invalid.", statusCode: 403);
        var subscription = await db.Subscriptions.SingleAsync(x => x.Id == lease.SubscriptionId);
        var device = await db.Devices.SingleAsync(x => x.Id == lease.DeviceId);
        var product = await db.Products.SingleAsync(x => x.Id == subscription.ProductId);
        var appUser = await db.Users.SingleAsync(x => x.Id == userId);
        if (subscription.UserId != userId || appUser.Quarantined || subscription.RevokedAt is not null ||
            subscription.PausedAt is not null || subscription.EndsAt <= now || device.RevokedAt is not null ||
            !product.Enabled || product.EmergencyStop)
            return Results.Problem("Entitlement or device is no longer active.", statusCode: 403);
        lease.ExpiresAt = now.AddMinutes(Math.Clamp(config.GetValue("Lease:LifetimeMinutes", 5), 1, 30));
        device.LastSeenAt = now;
        await db.SaveChangesAsync();
        return Results.Ok(new
        {
            capability = signer.Sign(new Capability(lease.Id, userId, product.Id, device.Id,
                lease.SessionId, channel, lease.ExpiresAt)),
            lease.ExpiresAt
        });
    }

    private static async Task<IResult> ReleaseLease(string sessionId, ClaimsPrincipal principal,
        KoptDbContext db, TimeProvider clock)
    {
        var userId = UserId(principal);
        var lease = await db.Leases.SingleOrDefaultAsync(x => x.SessionId == sessionId);
        if (lease is null) return Results.NoContent();
        var subscription = await db.Subscriptions.SingleAsync(x => x.Id == lease.SubscriptionId);
        if (subscription.UserId != userId) return Results.Forbid();
        lease.RevokedAt = clock.GetUtcNow();
        lease.RevokeReason = "Client release";
        await db.SaveChangesAsync();
        return Results.NoContent();
    }

    private static async Task<IResult> ReportTamper(TamperRequest request, ClaimsPrincipal principal,
        KoptDbContext db, TimeProvider clock)
    {
        if (string.IsNullOrWhiteSpace(request.Code) || request.Code.Length > 80)
            return Invalid("code", "A tamper code up to 80 characters is required.");
        if (string.IsNullOrWhiteSpace(request.EvidenceDigest) || request.EvidenceDigest.Length != 64 ||
            !request.EvidenceDigest.All(Uri.IsHexDigit))
            return Invalid("evidenceDigest", "EvidenceDigest must be a SHA-256 hexadecimal digest.");
        if (request.Signals is null || request.Signals.Length is < 1 or > 32 ||
            request.Signals.Any(x => string.IsNullOrWhiteSpace(x) || x.Length > 64))
            return Invalid("signals", "One to 32 bounded tamper signals are required.");
        var userId = UserId(principal);
        if (request.DeviceId is Guid deviceId &&
            !await db.Devices.AnyAsync(x => x.Id == deviceId && x.UserId == userId))
            return Invalid("deviceId", "Device does not belong to the authenticated account.");
        var now = clock.GetUtcNow();
        var decision = TamperRiskScorer.Evaluate(request.Signals);
        var incident = new TamperIncident
        {
            UserId = userId,
            DeviceId = request.DeviceId,
            Code = request.Code.Trim(),
            EvidenceDigest = request.EvidenceDigest.ToUpperInvariant(),
            SignalsJson = JsonSerializer.Serialize(decision.TrustedSignals),
            RiskScore = decision.Score,
            CreatedAt = now
        };
        db.TamperIncidents.Add(incident);
        if (decision.ShouldQuarantine)
        {
            var user = await db.Users.SingleAsync(x => x.Id == userId);
            user.Quarantined = true;
            user.QuarantineReason = $"Tamper incident {incident.Id}";
            var leases = await db.Leases.Where(x => x.RevokedAt == null &&
                db.Subscriptions.Where(s => s.UserId == userId).Select(s => s.Id).Contains(x.SubscriptionId)).ToListAsync();
            foreach (var lease in leases) { lease.RevokedAt = now; lease.RevokeReason = "Tamper quarantine"; }
        }
        await db.SaveChangesAsync();
        return Results.Accepted(value: new
        {
            incident.Id,
            riskScore = decision.Score,
            trustedSignals = decision.TrustedSignals,
            quarantined = decision.ShouldQuarantine
        });
    }

    private static async Task<IResult> GetRelease(string productSlug, string channel, string sessionId,
        ClaimsPrincipal principal, KoptDbContext db, TimeProvider clock)
    {
        if (!ValidToken(productSlug, 64) || !ValidToken(channel, 32) ||
            string.IsNullOrWhiteSpace(sessionId) || sessionId.Length is < 16 or > 128)
            return Invalid("request", "Product, channel or sessionId is invalid.");
        productSlug = productSlug.Trim().ToLowerInvariant();
        channel = channel.Trim().ToLowerInvariant();
        sessionId = sessionId.Trim();
        var userId = UserId(principal);
        var now = clock.GetUtcNow();
        var product = await db.Products.AsNoTracking().SingleOrDefaultAsync(x => x.Slug == productSlug);
        if (product is null) return Results.NotFound();
        if (!product.Enabled || product.EmergencyStop)
            return Results.Problem(product.EmergencyReason ?? "Product is unavailable.", statusCode: 503);
        var subscription = await db.Subscriptions.AsNoTracking().SingleOrDefaultAsync(x =>
            x.UserId == userId && x.ProductId == product.Id && x.RevokedAt == null && x.PausedAt == null &&
            x.StartsAt <= now && x.EndsAt > now);
        if (subscription is null) return Results.Forbid();
        var activeLease = await db.Leases.AsNoTracking().AnyAsync(x => x.SubscriptionId == subscription.Id &&
            x.SessionId == sessionId && x.RevokedAt == null && x.ExpiresAt > now);
        if (!activeLease) return Results.Problem("An active device lease is required.", statusCode: 403);

        var releases = await db.Releases.AsNoTracking().Where(x => x.ProductId == product.Id &&
            x.Channel == channel && x.Enabled && x.PublishedAt <= now).OrderByDescending(x => x.PublishedAt).ToListAsync();
        var release = releases.FirstOrDefault(x => RolloutBucket(userId, x.Id) < x.RolloutPercent);
        if (release is null) return Results.NoContent();
        return Results.Ok(new
        {
            release.Id,
            product = product.Slug,
            release.Channel,
            release.Version,
            release.MinimumLoaderVersion,
            release.ManifestUri,
            release.ManifestSha256,
            release.ManifestSignature,
            release.Changelog,
            release.KnownIssues,
            release.PublishedAt
        });
    }

    private static int RolloutBucket(Guid userId, Guid releaseId)
    {
        var digest = SHA256.HashData(Encoding.ASCII.GetBytes($"{userId:N}:{releaseId:N}"));
        return BitConverter.ToUInt16(digest, 0) % 100;
    }

    private static IResult? ValidateLeaseInput(string? product, string? fingerprint, string? deviceName,
        string? sessionId, string? nonce, string? channel)
    {
        if (!ValidToken(product, 64)) return Invalid("product", "Product is invalid.");
        if (string.IsNullOrWhiteSpace(fingerprint) || fingerprint.Length is < 16 or > 4096)
            return Invalid("deviceFingerprint", "DeviceFingerprint must contain 16..4096 characters.");
        if (string.IsNullOrWhiteSpace(deviceName) || deviceName.Trim().Length > 80)
            return Invalid("deviceName", "DeviceName must contain 1..80 characters.");
        return ValidateRenewInput(sessionId, nonce, channel);
    }

    private static IResult? ValidateRenewInput(string? sessionId, string? nonce, string? channel)
    {
        if (string.IsNullOrWhiteSpace(sessionId) || sessionId.Length is < 16 or > 128)
            return Invalid("sessionId", "SessionId must contain 16..128 characters.");
        if (string.IsNullOrWhiteSpace(nonce) || nonce.Length is < 16 or > 256)
            return Invalid("nonce", "Nonce must contain 16..256 characters.");
        if (!ValidToken(channel, 32)) return Invalid("channel", "Channel is invalid.");
        return null;
    }

    private static bool ValidToken(string? value, int maximum) =>
        !string.IsNullOrWhiteSpace(value) && value.Length <= maximum &&
        value.All(character => char.IsAsciiLetterOrDigit(character) || character is '-' or '_' or '.');

    private static IResult Invalid(string field, string message) => Results.ValidationProblem(
        new Dictionary<string, string[]> { [field] = [message] });

    private static AuditEvent Audit(Guid? actor, string action, string type, Guid id, string reason) => new()
    {
        CreatedAt = DateTimeOffset.UtcNow,
        ActorUserId = actor,
        Action = action,
        TargetType = type,
        TargetId = id.ToString(),
        Reason = reason
    };
}
