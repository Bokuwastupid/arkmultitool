using System.Security.Claims;
using System.Text.Json;
using Kopt.ControlPlane.Data;
using Kopt.ControlPlane.Domain;
using Kopt.ControlPlane.Security;
using Microsoft.EntityFrameworkCore;

namespace Kopt.ControlPlane.Endpoints;

public static class AdminEndpoints
{
    public sealed record GenerateKeysRequest(Guid ProductId, int Count, int DurationDays, int Slots, string Reason);
    public sealed record AdjustSubscriptionRequest(int DeltaDays, string Reason);
    public sealed record ReasonRequest(string Reason);
    public sealed record ResolveIncidentRequest(string Resolution, bool ReleaseQuarantine, bool RevokeDevice, string Reason);
    public sealed record EmergencyStopRequest(bool Enabled, string Reason);
    public sealed record PublishReleaseRequest(Guid ProductId, string Channel, string Version,
        string MinimumLoaderVersion, string ManifestUri, string ManifestSha256, string ManifestSignature,
        int RolloutPercent, string? Changelog, string? KnownIssues, string Reason);
    public sealed record ReleaseStateRequest(bool Enabled, int RolloutPercent, string Reason);

    public static IEndpointRouteBuilder MapAdminEndpoints(this IEndpointRouteBuilder routes)
    {
        var group = routes.MapGroup("/api/admin").RequireAuthorization("Admin");
        group.MapGet("/dashboard", Dashboard);
        group.MapGet("/products", Products);
        group.MapGet("/users", Users);
        group.MapGet("/users/{id:guid}", UserDetails);
        group.MapPost("/users/{id:guid}/quarantine", SetQuarantine);
        group.MapPost("/users/{id:guid}/logout", ForceLogout);
        group.MapGet("/keys", Keys);
        group.MapPost("/keys/generate", GenerateKeys);
        group.MapPost("/keys/{id:guid}/revoke", RevokeKey);
        group.MapPost("/subscriptions/{id:guid}/adjust", AdjustSubscription);
        group.MapPost("/subscriptions/{id:guid}/revoke", RevokeSubscription);
        group.MapPost("/devices/{id:guid}/reset", ResetDevice);
        group.MapPost("/devices/{id:guid}/revoke", RevokeDevice);
        group.MapGet("/incidents", Incidents);
        group.MapPost("/incidents/{id:guid}/resolve", ResolveIncident);
        group.MapPost("/products/{id:guid}/emergency-stop", EmergencyStop);
        group.MapGet("/releases", Releases);
        group.MapPost("/releases", PublishRelease);
        group.MapPost("/releases/{id:guid}/state", SetReleaseState);
        group.MapGet("/audit", AuditLog);
        return routes;
    }

    private static Guid AdminId(ClaimsPrincipal principal) => Guid.Parse(
        principal.FindFirstValue(ClaimTypes.NameIdentifier) ?? throw new InvalidOperationException("Missing subject."));

    private static async Task<IResult> Dashboard(KoptDbContext db, TimeProvider clock)
    {
        var now = clock.GetUtcNow();
        return Results.Ok(new
        {
            users = await db.Users.CountAsync(),
            quarantined = await db.Users.CountAsync(x => x.Quarantined),
            activeSubscriptions = await db.Subscriptions.CountAsync(x => x.RevokedAt == null && x.PausedAt == null && x.EndsAt > now),
            expiringSevenDays = await db.Subscriptions.CountAsync(x => x.RevokedAt == null && x.EndsAt > now && x.EndsAt <= now.AddDays(7)),
            activeLeases = await db.Leases.CountAsync(x => x.RevokedAt == null && x.ExpiresAt > now),
            openIncidents = await db.TamperIncidents.CountAsync(x => x.ResolvedAt == null),
            emergencyStops = await db.Products.CountAsync(x => x.EmergencyStop)
        });
    }

    private static async Task<IResult> Products(KoptDbContext db) => Results.Ok(await db.Products.AsNoTracking()
        .OrderBy(x => x.Name)
        .Select(x => new { x.Id, x.Slug, x.Name, x.Enabled, x.EmergencyStop, x.EmergencyReason })
        .ToListAsync());

    private static async Task<IResult> Users(string? query, int page, KoptDbContext db)
    {
        var normalizedPage = Math.Max(1, page);
        var users = db.Users.AsNoTracking();
        if (!string.IsNullOrWhiteSpace(query))
            users = users.Where(x => x.Email != null && EF.Functions.ILike(x.Email, $"%{query.Trim()}%"));
        var result = await users.OrderBy(x => x.Email).Skip((normalizedPage - 1) * 50).Take(50)
            .Select(x => new { x.Id, x.Email, x.Quarantined, x.QuarantineReason }).ToListAsync();
        return Results.Ok(result);
    }

    private static async Task<IResult> UserDetails(Guid id, KoptDbContext db, TimeProvider clock)
    {
        var user = await db.Users.AsNoTracking().Where(x => x.Id == id)
            .Select(x => new { x.Id, x.Email, x.Quarantined, x.QuarantineReason }).SingleOrDefaultAsync();
        if (user is null) return Results.NotFound();
        var now = clock.GetUtcNow();
        var subscriptions = await db.Subscriptions.AsNoTracking().Where(x => x.UserId == id)
            .Join(db.Products, x => x.ProductId, x => x.Id, (subscription, product) => new
            {
                subscription.Id,
                subscription.ProductId,
                Product = product.Name,
                subscription.StartsAt,
                subscription.EndsAt,
                subscription.SlotLimit,
                subscription.PausedAt,
                subscription.RevokedAt,
                subscription.StateReason,
                subscription.Version,
                Active = subscription.RevokedAt == null && subscription.PausedAt == null && subscription.EndsAt > now
            }).ToListAsync();
        var devices = await db.Devices.AsNoTracking().Where(x => x.UserId == id)
            .Select(x => new { x.Id, x.ProductId, x.DisplayName, x.EnrolledAt, x.LastSeenAt, x.RevokedAt, x.RevokeReason })
            .ToListAsync();
        var subscriptionIds = db.Subscriptions.Where(x => x.UserId == id).Select(x => x.Id);
        var leases = await db.Leases.AsNoTracking().Where(x => subscriptionIds.Contains(x.SubscriptionId))
            .OrderByDescending(x => x.IssuedAt).Take(25)
            .Select(x => new { x.Id, x.DeviceId, x.SessionId, x.IssuedAt, x.ExpiresAt, x.RevokedAt, x.RevokeReason })
            .ToListAsync();
        return Results.Ok(new { user, subscriptions, devices, leases });
    }

    private static async Task<IResult> SetQuarantine(Guid id, EmergencyStopRequest request,
        ClaimsPrincipal principal, KoptDbContext db, TimeProvider clock)
    {
        var user = await db.Users.FindAsync(id);
        if (user is null) return Results.NotFound();
        var reason = RequiredReason(request.Reason);
        user.Quarantined = request.Enabled;
        user.QuarantineReason = request.Enabled ? reason : null;
        if (request.Enabled)
        {
            var subscriptionIds = db.Subscriptions.Where(x => x.UserId == id).Select(x => x.Id);
            var leases = await db.Leases.Where(x => subscriptionIds.Contains(x.SubscriptionId) && x.RevokedAt == null)
                .ToListAsync();
            foreach (var lease in leases) { lease.RevokedAt = clock.GetUtcNow(); lease.RevokeReason = "User quarantine"; }
        }
        db.AuditEvents.Add(Audit(AdminId(principal), request.Enabled ? "user.quarantine" : "user.release-quarantine",
            nameof(AppUser), id, reason, after: new { user.Quarantined }));
        await db.SaveChangesAsync();
        return Results.Ok(new { user.Quarantined, user.QuarantineReason });
    }

    private static async Task<IResult> ForceLogout(Guid id, ReasonRequest request, ClaimsPrincipal principal,
        KoptDbContext db, TimeProvider clock)
    {
        if (!await db.Users.AnyAsync(x => x.Id == id)) return Results.NotFound();
        var now = clock.GetUtcNow();
        var subscriptionIds = db.Subscriptions.Where(x => x.UserId == id).Select(x => x.Id);
        var leases = await db.Leases.Where(x => subscriptionIds.Contains(x.SubscriptionId) && x.RevokedAt == null)
            .ToListAsync();
        foreach (var lease in leases) { lease.RevokedAt = now; lease.RevokeReason = "Forced logout"; }
        db.AuditEvents.Add(Audit(AdminId(principal), "user.force-logout", nameof(AppUser), id,
            RequiredReason(request.Reason), after: new { RevokedLeases = leases.Count }));
        await db.SaveChangesAsync();
        return Results.Ok(new { revokedLeases = leases.Count });
    }

    private static async Task<IResult> Keys(int page, KoptDbContext db) => Results.Ok(await db.LicenseKeys.AsNoTracking()
        .OrderByDescending(x => x.CreatedAt).Skip((Math.Max(1, page) - 1) * 100).Take(100)
        .Select(x => new { x.Id, x.ProductId, x.DurationDays, x.SlotLimit, x.CreatedAt, x.RedeemedAt, x.RevokedAt, x.RevokeReason })
        .ToListAsync());

    private static async Task<IResult> GenerateKeys(GenerateKeysRequest request, ClaimsPrincipal principal,
        KoptDbContext db, SecretHasher hasher, TimeProvider clock)
    {
        if (request.Count is < 1 or > 100 || request.DurationDays is < 1 or > 3650 || request.Slots is < 1 or > 20 ||
            string.IsNullOrWhiteSpace(request.Reason)) return Results.ValidationProblem(new Dictionary<string, string[]>
            {
                ["request"] = ["Count 1..100, duration 1..3650, slots 1..20 and reason are required."]
            });
        if (!await db.Products.AnyAsync(x => x.Id == request.ProductId)) return Results.NotFound();
        var now = clock.GetUtcNow();
        var admin = AdminId(principal);
        var plaintext = new List<string>(request.Count);
        for (var index = 0; index < request.Count; index++)
        {
            var code = SecretHasher.NewLicenseCode();
            plaintext.Add(code);
            db.LicenseKeys.Add(new LicenseKey
            {
                ProductId = request.ProductId,
                CodeHash = hasher.Hash(code),
                DurationDays = request.DurationDays,
                SlotLimit = request.Slots,
                CreatedAt = now,
                CreatedBy = admin
            });
        }
        db.AuditEvents.Add(Audit(admin, "keys.generate", nameof(Product), request.ProductId, request.Reason,
            new { request.Count, request.DurationDays, request.Slots }));
        await db.SaveChangesAsync();
        return Results.Ok(new { keys = plaintext, warning = "Plaintext is shown once and is not stored." });
    }

    private static async Task<IResult> RevokeKey(Guid id, ReasonRequest request, ClaimsPrincipal principal,
        KoptDbContext db, TimeProvider clock)
    {
        var key = await db.LicenseKeys.FindAsync(id);
        if (key is null) return Results.NotFound();
        var before = new { key.RevokedAt, key.RevokeReason };
        key.RevokedAt = clock.GetUtcNow();
        key.RevokeReason = RequiredReason(request.Reason);
        db.AuditEvents.Add(Audit(AdminId(principal), "key.revoke", nameof(LicenseKey), id, request.Reason, before));
        await db.SaveChangesAsync();
        return Results.NoContent();
    }

    private static async Task<IResult> AdjustSubscription(Guid id, AdjustSubscriptionRequest request,
        ClaimsPrincipal principal, KoptDbContext db, TimeProvider clock)
    {
        if (request.DeltaDays is < -3650 or > 3650 || request.DeltaDays == 0)
            return Results.Problem("DeltaDays must be between -3650 and 3650 and non-zero.", statusCode: 400);
        var subscription = await db.Subscriptions.FindAsync(id);
        if (subscription is null) return Results.NotFound();
        var before = new { subscription.EndsAt, subscription.RevokedAt, subscription.Version };
        subscription.EndsAt = subscription.EndsAt.AddDays(request.DeltaDays);
        if (subscription.EndsAt < clock.GetUtcNow()) subscription.EndsAt = clock.GetUtcNow();
        subscription.Version++;
        db.AuditEvents.Add(Audit(AdminId(principal), "subscription.adjust", nameof(Subscription), id,
            RequiredReason(request.Reason), before, new { subscription.EndsAt, subscription.Version }));
        await RevokeSubscriptionLeases(db, subscription.Id, clock.GetUtcNow(), "Subscription changed");
        await db.SaveChangesAsync();
        return Results.Ok(new { subscription.EndsAt, subscription.Version });
    }

    private static async Task<IResult> RevokeSubscription(Guid id, ReasonRequest request,
        ClaimsPrincipal principal, KoptDbContext db, TimeProvider clock)
    {
        var subscription = await db.Subscriptions.FindAsync(id);
        if (subscription is null) return Results.NotFound();
        subscription.RevokedAt = clock.GetUtcNow();
        subscription.StateReason = RequiredReason(request.Reason);
        subscription.Version++;
        await RevokeSubscriptionLeases(db, id, clock.GetUtcNow(), "Subscription revoked");
        db.AuditEvents.Add(Audit(AdminId(principal), "subscription.revoke", nameof(Subscription), id, request.Reason));
        await db.SaveChangesAsync();
        return Results.NoContent();
    }

    private static async Task<IResult> ResetDevice(Guid id, ReasonRequest request, ClaimsPrincipal principal,
        KoptDbContext db, TimeProvider clock, IConfiguration config)
    {
        var device = await db.Devices.FindAsync(id);
        if (device is null) return Results.NotFound();
        var now = clock.GetUtcNow();
        device.RevokedAt = now;
        device.RevokeReason = "Admin slot reset: " + RequiredReason(request.Reason);
        device.ResetEligibleAt = now.AddHours(Math.Clamp(config.GetValue("Lease:DeviceResetCooldownHours", 72), 0, 720));
        await RevokeDeviceLeases(db, id, now, "Device slot reset");
        db.AuditEvents.Add(Audit(AdminId(principal), "device.reset", nameof(Device), id, request.Reason));
        await db.SaveChangesAsync();
        return Results.NoContent();
    }

    private static async Task<IResult> RevokeDevice(Guid id, ReasonRequest request, ClaimsPrincipal principal,
        KoptDbContext db, TimeProvider clock)
    {
        var device = await db.Devices.FindAsync(id);
        if (device is null) return Results.NotFound();
        var now = clock.GetUtcNow();
        device.RevokedAt = now;
        device.RevokeReason = RequiredReason(request.Reason);
        await RevokeDeviceLeases(db, id, now, "Device revoked");
        db.AuditEvents.Add(Audit(AdminId(principal), "device.revoke", nameof(Device), id, request.Reason));
        await db.SaveChangesAsync();
        return Results.NoContent();
    }

    private static async Task<IResult> Incidents(string state, KoptDbContext db)
    {
        var query = db.TamperIncidents.AsNoTracking();
        if (string.Equals(state, "open", StringComparison.OrdinalIgnoreCase)) query = query.Where(x => x.ResolvedAt == null);
        return Results.Ok(await query.OrderByDescending(x => x.CreatedAt).Take(200).ToListAsync());
    }

    private static async Task<IResult> ResolveIncident(Guid id, ResolveIncidentRequest request,
        ClaimsPrincipal principal, KoptDbContext db, TimeProvider clock)
    {
        if (string.IsNullOrWhiteSpace(request.Resolution) || request.Resolution.Trim().Length > 256)
            return Results.ValidationProblem(new Dictionary<string, string[]>
            {
                ["resolution"] = ["Resolution must contain 1..256 characters."]
            });
        var reason = RequiredReason(request.Reason);
        var incident = await db.TamperIncidents.FindAsync(id);
        if (incident is null) return Results.NotFound();
        var now = clock.GetUtcNow();
        var admin = AdminId(principal);
        incident.ResolvedAt = now;
        incident.ResolvedBy = admin;
        incident.Resolution = request.Resolution.Trim();
        var user = await db.Users.FindAsync(incident.UserId);
        if (user is not null && request.ReleaseQuarantine)
        {
            user.Quarantined = false;
            user.QuarantineReason = null;
        }
        if (request.RevokeDevice && incident.DeviceId is Guid deviceId)
        {
            var device = await db.Devices.FindAsync(deviceId);
            if (device is not null) { device.RevokedAt = now; device.RevokeReason = reason; }
            await RevokeDeviceLeases(db, deviceId, now, "Incident resolution");
        }
        db.AuditEvents.Add(Audit(admin, "incident.resolve", nameof(TamperIncident), id, reason));
        await db.SaveChangesAsync();
        return Results.NoContent();
    }

    private static async Task<IResult> EmergencyStop(Guid id, EmergencyStopRequest request,
        ClaimsPrincipal principal, KoptDbContext db, TimeProvider clock)
    {
        var product = await db.Products.FindAsync(id);
        if (product is null) return Results.NotFound();
        product.EmergencyStop = request.Enabled;
        product.EmergencyReason = RequiredReason(request.Reason);
        if (request.Enabled)
        {
            var subscriptionIds = db.Subscriptions.Where(x => x.ProductId == id).Select(x => x.Id);
            var leases = await db.Leases.Where(x => subscriptionIds.Contains(x.SubscriptionId) && x.RevokedAt == null).ToListAsync();
            foreach (var lease in leases) { lease.RevokedAt = clock.GetUtcNow(); lease.RevokeReason = "Product emergency stop"; }
        }
        db.AuditEvents.Add(Audit(AdminId(principal), "product.emergency-stop", nameof(Product), id, request.Reason,
            after: new { product.EmergencyStop }));
        await db.SaveChangesAsync();
        return Results.Ok(new { product.EmergencyStop, product.EmergencyReason });
    }

    private static async Task<IResult> Releases(Guid? productId, KoptDbContext db)
    {
        var query = db.Releases.AsNoTracking();
        if (productId is Guid id) query = query.Where(x => x.ProductId == id);
        return Results.Ok(await query.OrderByDescending(x => x.PublishedAt).Take(200).ToListAsync());
    }

    private static async Task<IResult> PublishRelease(PublishReleaseRequest request, ClaimsPrincipal principal,
        KoptDbContext db, TimeProvider clock)
    {
        var reason = RequiredReason(request.Reason);
        if (!await db.Products.AnyAsync(x => x.Id == request.ProductId)) return Results.NotFound();
        if (!ValidToken(request.Channel, 32) ||
            string.IsNullOrWhiteSpace(request.Version) || request.Version.Length > 64 ||
            string.IsNullOrWhiteSpace(request.MinimumLoaderVersion) || request.MinimumLoaderVersion.Length > 64 ||
            request.RolloutPercent is < 0 or > 100 ||
            string.IsNullOrWhiteSpace(request.ManifestSha256) || request.ManifestSha256.Length != 64 ||
            !request.ManifestSha256.All(Uri.IsHexDigit) ||
            string.IsNullOrWhiteSpace(request.ManifestUri) || request.ManifestUri.Length > 2048 ||
            !Uri.TryCreate(request.ManifestUri, UriKind.Absolute, out var manifestUri) || manifestUri.Scheme != Uri.UriSchemeHttps)
            return Results.Problem("Invalid channel, version, rollout, SHA-256 or HTTPS manifest URI.", statusCode: 400);
        if (string.IsNullOrWhiteSpace(request.ManifestSignature) || request.ManifestSignature.Length > 4096)
            return Results.Problem("Manifest signature is required and must be bounded.", statusCode: 400);
        try
        {
            if (Convert.FromBase64String(request.ManifestSignature).Length < 48)
                return Results.Problem("Manifest signature is too short.", statusCode: 400);
        }
        catch (FormatException) { return Results.Problem("Manifest signature must be base64.", statusCode: 400); }

        var release = new ProductRelease
        {
            ProductId = request.ProductId,
            Channel = request.Channel.Trim().ToLowerInvariant(),
            Version = request.Version.Trim(),
            MinimumLoaderVersion = request.MinimumLoaderVersion.Trim(),
            ManifestUri = manifestUri.AbsoluteUri,
            ManifestSha256 = request.ManifestSha256.ToUpperInvariant(),
            ManifestSignature = request.ManifestSignature,
            RolloutPercent = request.RolloutPercent,
            Changelog = TrimOptional(request.Changelog, 4000),
            KnownIssues = TrimOptional(request.KnownIssues, 4000),
            PublishedAt = clock.GetUtcNow()
        };
        db.Releases.Add(release);
        db.AuditEvents.Add(Audit(AdminId(principal), "release.publish", nameof(ProductRelease), release.Id,
            reason, after: new { release.ProductId, release.Channel, release.Version, release.RolloutPercent }));
        await db.SaveChangesAsync();
        return Results.Created($"/api/admin/releases/{release.Id}", release);
    }

    private static async Task<IResult> SetReleaseState(Guid id, ReleaseStateRequest request,
        ClaimsPrincipal principal, KoptDbContext db)
    {
        if (request.RolloutPercent is < 0 or > 100) return Results.Problem("RolloutPercent must be 0..100.", statusCode: 400);
        var release = await db.Releases.FindAsync(id);
        if (release is null) return Results.NotFound();
        var before = new { release.Enabled, release.RolloutPercent };
        release.Enabled = request.Enabled;
        release.RolloutPercent = request.RolloutPercent;
        db.AuditEvents.Add(Audit(AdminId(principal), "release.state", nameof(ProductRelease), id,
            RequiredReason(request.Reason), before, new { release.Enabled, release.RolloutPercent }));
        await db.SaveChangesAsync();
        return Results.Ok(new { release.Enabled, release.RolloutPercent });
    }

    private static async Task<IResult> AuditLog(int page, KoptDbContext db) => Results.Ok(await db.AuditEvents.AsNoTracking()
        .OrderByDescending(x => x.CreatedAt).Skip((Math.Max(1, page) - 1) * 100).Take(100).ToListAsync());

    private static string RequiredReason(string reason) => !string.IsNullOrWhiteSpace(reason)
        ? reason.Trim()[..Math.Min(256, reason.Trim().Length)]
        : throw new BadHttpRequestException("A reason is required.");

    private static string? TrimOptional(string? value, int maximum) => string.IsNullOrWhiteSpace(value)
        ? null : value.Trim()[..Math.Min(maximum, value.Trim().Length)];

    private static bool ValidToken(string? value, int maximum) =>
        !string.IsNullOrWhiteSpace(value) && value.Length <= maximum &&
        value.All(character => char.IsAsciiLetterOrDigit(character) || character is '-' or '_' or '.');

    private static async Task RevokeSubscriptionLeases(KoptDbContext db, Guid subscriptionId,
        DateTimeOffset now, string reason)
    {
        var leases = await db.Leases.Where(x => x.SubscriptionId == subscriptionId && x.RevokedAt == null).ToListAsync();
        foreach (var lease in leases) { lease.RevokedAt = now; lease.RevokeReason = reason; }
    }

    private static async Task RevokeDeviceLeases(KoptDbContext db, Guid deviceId, DateTimeOffset now, string reason)
    {
        var leases = await db.Leases.Where(x => x.DeviceId == deviceId && x.RevokedAt == null).ToListAsync();
        foreach (var lease in leases) { lease.RevokedAt = now; lease.RevokeReason = reason; }
    }

    private static AuditEvent Audit(Guid actor, string action, string type, Guid id, string reason,
        object? before = null, object? after = null) => new()
        {
            CreatedAt = DateTimeOffset.UtcNow,
            ActorUserId = actor,
            Action = action,
            TargetType = type,
            TargetId = id.ToString(),
            Reason = RequiredReason(reason),
            BeforeJson = before is null ? null : JsonSerializer.Serialize(before),
            AfterJson = after is null ? null : JsonSerializer.Serialize(after)
        };
}
