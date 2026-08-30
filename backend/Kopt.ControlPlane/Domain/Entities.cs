using Microsoft.AspNetCore.Identity;

namespace Kopt.ControlPlane.Domain;

public sealed class AppUser : IdentityUser<Guid>
{
    public bool Quarantined { get; set; }
    public string? QuarantineReason { get; set; }
}

public sealed class Product
{
    public Guid Id { get; set; } = Guid.NewGuid();
    public required string Slug { get; set; }
    public required string Name { get; set; }
    public bool Enabled { get; set; } = true;
    public bool EmergencyStop { get; set; }
    public string? EmergencyReason { get; set; }
}

public sealed class LicenseKey
{
    public Guid Id { get; set; } = Guid.NewGuid();
    public Guid ProductId { get; set; }
    public required string CodeHash { get; set; }
    public int DurationDays { get; set; }
    public int SlotLimit { get; set; }
    public DateTimeOffset CreatedAt { get; set; }
    public Guid CreatedBy { get; set; }
    public DateTimeOffset? RedeemedAt { get; set; }
    public Guid? RedeemedBy { get; set; }
    public DateTimeOffset? RevokedAt { get; set; }
    public string? RevokeReason { get; set; }
}

public sealed class Subscription
{
    public Guid Id { get; set; } = Guid.NewGuid();
    public Guid UserId { get; set; }
    public Guid ProductId { get; set; }
    public DateTimeOffset StartsAt { get; set; }
    public DateTimeOffset EndsAt { get; set; }
    public int SlotLimit { get; set; }
    public DateTimeOffset? PausedAt { get; set; }
    public DateTimeOffset? RevokedAt { get; set; }
    public string? StateReason { get; set; }
    public uint Version { get; set; }
}

public sealed class Device
{
    public Guid Id { get; set; } = Guid.NewGuid();
    public Guid UserId { get; set; }
    public Guid ProductId { get; set; }
    public required string FingerprintHash { get; set; }
    public required string DisplayName { get; set; }
    public DateTimeOffset EnrolledAt { get; set; }
    public DateTimeOffset LastSeenAt { get; set; }
    public DateTimeOffset? ResetEligibleAt { get; set; }
    public DateTimeOffset? RevokedAt { get; set; }
    public string? RevokeReason { get; set; }
}

public sealed class Lease
{
    public Guid Id { get; set; } = Guid.NewGuid();
    public Guid SubscriptionId { get; set; }
    public Guid DeviceId { get; set; }
    public required string SessionId { get; set; }
    public required string NonceHash { get; set; }
    public DateTimeOffset IssuedAt { get; set; }
    public DateTimeOffset ExpiresAt { get; set; }
    public DateTimeOffset? RevokedAt { get; set; }
    public string? RevokeReason { get; set; }
}

public sealed class ProductRelease
{
    public Guid Id { get; set; } = Guid.NewGuid();
    public Guid ProductId { get; set; }
    public required string Channel { get; set; }
    public required string Version { get; set; }
    public required string ManifestUri { get; set; }
    public required string ManifestSha256 { get; set; }
    public required string ManifestSignature { get; set; }
    public required string MinimumLoaderVersion { get; set; }
    public string? Changelog { get; set; }
    public string? KnownIssues { get; set; }
    public int RolloutPercent { get; set; } = 100;
    public bool Enabled { get; set; } = true;
    public DateTimeOffset PublishedAt { get; set; }
}

public sealed class TamperIncident
{
    public Guid Id { get; set; } = Guid.NewGuid();
    public Guid UserId { get; set; }
    public Guid? DeviceId { get; set; }
    public required string Code { get; set; }
    public required string EvidenceDigest { get; set; }
    public required string SignalsJson { get; set; }
    public int RiskScore { get; set; }
    public DateTimeOffset CreatedAt { get; set; }
    public DateTimeOffset? ResolvedAt { get; set; }
    public Guid? ResolvedBy { get; set; }
    public string? Resolution { get; set; }
}

public sealed class AuditEvent
{
    public long Id { get; set; }
    public DateTimeOffset CreatedAt { get; set; }
    public Guid? ActorUserId { get; set; }
    public required string Action { get; set; }
    public required string TargetType { get; set; }
    public required string TargetId { get; set; }
    public required string Reason { get; set; }
    public string? BeforeJson { get; set; }
    public string? AfterJson { get; set; }
    public string? CorrelationId { get; set; }
}
