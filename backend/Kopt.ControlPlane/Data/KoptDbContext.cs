using Kopt.ControlPlane.Domain;
using Microsoft.AspNetCore.Identity;
using Microsoft.AspNetCore.Identity.EntityFrameworkCore;
using Microsoft.EntityFrameworkCore;

namespace Kopt.ControlPlane.Data;

public sealed class KoptDbContext(DbContextOptions<KoptDbContext> options)
    : IdentityDbContext<AppUser, IdentityRole<Guid>, Guid>(options)
{
    public DbSet<Product> Products => Set<Product>();
    public DbSet<LicenseKey> LicenseKeys => Set<LicenseKey>();
    public DbSet<Subscription> Subscriptions => Set<Subscription>();
    public DbSet<Device> Devices => Set<Device>();
    public DbSet<Lease> Leases => Set<Lease>();
    public DbSet<ProductRelease> Releases => Set<ProductRelease>();
    public DbSet<TamperIncident> TamperIncidents => Set<TamperIncident>();
    public DbSet<AuditEvent> AuditEvents => Set<AuditEvent>();

    protected override void OnModelCreating(ModelBuilder builder)
    {
        base.OnModelCreating(builder);
        builder.Entity<Product>().HasIndex(x => x.Slug).IsUnique();
        builder.Entity<LicenseKey>().HasIndex(x => x.CodeHash).IsUnique();
        builder.Entity<Subscription>().HasIndex(x => new { x.UserId, x.ProductId }).IsUnique();
        builder.Entity<Device>().HasIndex(x => new { x.UserId, x.ProductId, x.FingerprintHash }).IsUnique();
        builder.Entity<Lease>().HasIndex(x => x.SessionId).IsUnique();
        builder.Entity<ProductRelease>().HasIndex(x => new { x.ProductId, x.Channel, x.Version }).IsUnique();
        builder.Entity<AuditEvent>().Property(x => x.Id).ValueGeneratedOnAdd();
        builder.Entity<Subscription>().Property(x => x.Version).IsConcurrencyToken();

        builder.Entity<LicenseKey>().HasOne<Product>().WithMany().HasForeignKey(x => x.ProductId).OnDelete(DeleteBehavior.Restrict);
        builder.Entity<Subscription>().HasOne<AppUser>().WithMany().HasForeignKey(x => x.UserId).OnDelete(DeleteBehavior.Restrict);
        builder.Entity<Subscription>().HasOne<Product>().WithMany().HasForeignKey(x => x.ProductId).OnDelete(DeleteBehavior.Restrict);
        builder.Entity<Device>().HasOne<AppUser>().WithMany().HasForeignKey(x => x.UserId).OnDelete(DeleteBehavior.Restrict);
        builder.Entity<Device>().HasOne<Product>().WithMany().HasForeignKey(x => x.ProductId).OnDelete(DeleteBehavior.Restrict);
        builder.Entity<Lease>().HasOne<Subscription>().WithMany().HasForeignKey(x => x.SubscriptionId).OnDelete(DeleteBehavior.Restrict);
        builder.Entity<Lease>().HasOne<Device>().WithMany().HasForeignKey(x => x.DeviceId).OnDelete(DeleteBehavior.Restrict);
        builder.Entity<ProductRelease>().HasOne<Product>().WithMany().HasForeignKey(x => x.ProductId).OnDelete(DeleteBehavior.Restrict);
        builder.Entity<TamperIncident>().HasOne<AppUser>().WithMany().HasForeignKey(x => x.UserId).OnDelete(DeleteBehavior.Restrict);
        builder.Entity<TamperIncident>().HasOne<Device>().WithMany().HasForeignKey(x => x.DeviceId).OnDelete(DeleteBehavior.SetNull);

        builder.Entity<LicenseKey>().ToTable(x =>
        {
            x.HasCheckConstraint("CK_LicenseKey_DurationDays", "\"DurationDays\" BETWEEN 1 AND 3650");
            x.HasCheckConstraint("CK_LicenseKey_SlotLimit", "\"SlotLimit\" BETWEEN 1 AND 20");
        });
        builder.Entity<Subscription>().ToTable(x =>
            x.HasCheckConstraint("CK_Subscription_SlotLimit", "\"SlotLimit\" BETWEEN 1 AND 20"));
        builder.Entity<TamperIncident>().ToTable(x =>
            x.HasCheckConstraint("CK_TamperIncident_RiskScore", "\"RiskScore\" BETWEEN 0 AND 100"));
        builder.Entity<ProductRelease>().ToTable(x =>
            x.HasCheckConstraint("CK_ProductRelease_RolloutPercent", "\"RolloutPercent\" BETWEEN 0 AND 100"));

        builder.Entity<TamperIncident>().HasIndex(x => new { x.ResolvedAt, x.CreatedAt });
        builder.Entity<AuditEvent>().HasIndex(x => x.CreatedAt);
    }
}
