using System.Threading.RateLimiting;
using Kopt.ControlPlane.Data;
using Kopt.ControlPlane.Domain;
using Kopt.ControlPlane.Endpoints;
using Kopt.ControlPlane.Security;
using Microsoft.AspNetCore.Identity;
using Microsoft.AspNetCore.DataProtection;
using Microsoft.EntityFrameworkCore;
using System.Security.Cryptography;

var builder = WebApplication.CreateBuilder(new WebApplicationOptions
{
    Args = args,
    ContentRootPath = AppContext.BaseDirectory,
    WebRootPath = Path.Combine(AppContext.BaseDirectory, "wwwroot")
});
builder.Logging.ClearProviders();
builder.Logging.AddSimpleConsole(options =>
{
    options.SingleLine = true;
    options.TimestampFormat = "HH:mm:ss ";
});
if (!builder.Environment.IsDevelopment())
    ValidateProductionSecrets(builder.Configuration);
var configuredKeyDirectory = builder.Configuration["Security:DataProtectionPath"] ?? "data-protection";
var keyDirectory = Path.IsPathRooted(configuredKeyDirectory)
    ? configuredKeyDirectory
    : Path.Combine(builder.Environment.ContentRootPath, configuredKeyDirectory);
Directory.CreateDirectory(keyDirectory);
builder.Services.AddDataProtection()
    .PersistKeysToFileSystem(new DirectoryInfo(keyDirectory))
    .SetApplicationName("KOPT.ControlPlane");
builder.Services.AddProblemDetails();
var databaseProvider = builder.Configuration["Database:Provider"] ?? "Postgres";
var databaseConnection = builder.Configuration.GetConnectionString("ControlPlane")
    ?? throw new InvalidOperationException("ConnectionStrings:ControlPlane is required.");
builder.Services.AddDbContext<KoptDbContext>(options =>
{
    if (string.Equals(databaseProvider, "Sqlite", StringComparison.OrdinalIgnoreCase))
    {
        if (!builder.Environment.IsDevelopment())
            throw new InvalidOperationException("SQLite is restricted to the local Development profile.");
        options.UseSqlite(databaseConnection);
    }
    else if (string.Equals(databaseProvider, "Postgres", StringComparison.OrdinalIgnoreCase))
    {
        options.UseNpgsql(databaseConnection);
    }
    else
    {
        throw new InvalidOperationException("Database:Provider must be Postgres or Sqlite.");
    }
});
builder.Services.AddIdentityApiEndpoints<AppUser>(options =>
    {
        options.SignIn.RequireConfirmedAccount = false;
        options.Password.RequiredLength = 12;
        options.Password.RequireNonAlphanumeric = true;
        options.Lockout.MaxFailedAccessAttempts = 5;
        options.Lockout.DefaultLockoutTimeSpan = TimeSpan.FromMinutes(15);
    })
    .AddRoles<IdentityRole<Guid>>()
    .AddEntityFrameworkStores<KoptDbContext>();
builder.Services.AddAuthorizationBuilder().AddPolicy("Admin", policy =>
{
    policy.RequireAuthenticatedUser().RequireRole("Admin");
    if (builder.Configuration.GetValue("Security:RequireAdminMfa", false))
        policy.RequireClaim("amr", "mfa");
});
builder.Services.AddRateLimiter(options =>
{
    options.RejectionStatusCode = StatusCodes.Status429TooManyRequests;
    options.AddPolicy("auth", context => RateLimitPartition.GetFixedWindowLimiter(
        context.Connection.RemoteIpAddress?.ToString() ?? "unknown",
        _ => new FixedWindowRateLimiterOptions
        {
            PermitLimit = 20,
            Window = TimeSpan.FromMinutes(1),
            QueueLimit = 0,
            AutoReplenishment = true
        }));
    options.AddPolicy("tamper", context => RateLimitPartition.GetFixedWindowLimiter(
        context.User.Identity?.Name ?? context.Connection.RemoteIpAddress?.ToString() ?? "anonymous",
        _ => new FixedWindowRateLimiterOptions
        {
            PermitLimit = 10,
            Window = TimeSpan.FromMinutes(1),
            QueueLimit = 0,
            AutoReplenishment = true
        }));
});
builder.Services.AddHealthChecks().AddCheck<DatabaseHealthCheck>("database");
builder.Services.AddSingleton(TimeProvider.System);
builder.Services.AddSingleton<SecretHasher>();
builder.Services.AddSingleton<CapabilitySigner>();

var app = builder.Build();
app.UseExceptionHandler();
app.UseHttpsRedirection();
app.UseDefaultFiles();
app.UseStaticFiles();
app.UseRateLimiter();
app.UseAuthentication();
app.UseAuthorization();
app.MapGroup("/api/auth").RequireRateLimiting("auth").MapIdentityApi<AppUser>();
app.MapClientEndpoints();
app.MapAdminEndpoints();
app.MapHealthChecks("/health");
app.MapFallbackToFile("index.html");

if (app.Configuration.GetValue("Database:AutoMigrate", false))
{
    await using var scope = app.Services.CreateAsyncScope();
    var database = scope.ServiceProvider.GetRequiredService<KoptDbContext>();
    if (string.Equals(databaseProvider, "Sqlite", StringComparison.OrdinalIgnoreCase))
        await database.Database.EnsureCreatedAsync();
    else
        await database.Database.MigrateAsync();
    await Bootstrapper.SeedAsync(scope.ServiceProvider, app.Configuration);
}

await app.RunAsync();

static void ValidateProductionSecrets(IConfiguration configuration)
{
    var pepperText = configuration["Security:HashPepper"];
    byte[] pepper;
    try { pepper = Convert.FromBase64String(pepperText ?? string.Empty); }
    catch (FormatException) { throw new InvalidOperationException("Security:HashPepper must be valid base64."); }
    if (pepper.Length < 32)
        throw new InvalidOperationException("Security:HashPepper must contain at least 32 random bytes.");

    var privatePem = configuration["Security:CapabilityPrivateKeyPem"];
    var privatePkcs8 = configuration["Security:CapabilityPrivateKeyPkcs8"];
    try
    {
        using var key = ECDsa.Create();
        if (!string.IsNullOrWhiteSpace(privatePkcs8))
            key.ImportPkcs8PrivateKey(Convert.FromBase64String(privatePkcs8), out _);
        else
            key.ImportFromPem(privatePem ?? string.Empty);
        if (key.KeySize < 256) throw new CryptographicException("Capability key is too small.");
    }
    catch (Exception error) when (error is ArgumentException or FormatException or CryptographicException)
    {
        throw new InvalidOperationException(
            "Security:CapabilityPrivateKeyPkcs8 or CapabilityPrivateKeyPem must be an ECDSA P-256 private key.", error);
    }
}

public partial class Program;

internal static class Bootstrapper
{
    public static async Task SeedAsync(IServiceProvider services, IConfiguration configuration)
    {
        var db = services.GetRequiredService<KoptDbContext>();
        if (!await db.Products.AnyAsync())
        {
            db.Products.Add(new Product { Slug = "ark-ase", Name = "KOPT ARK: Survival Evolved" });
            await db.SaveChangesAsync();
        }
        var roleManager = services.GetRequiredService<RoleManager<IdentityRole<Guid>>>();
        if (!await roleManager.RoleExistsAsync("Admin"))
            IdentityResultGuard(await roleManager.CreateAsync(new IdentityRole<Guid>("Admin")));
        var email = Environment.GetEnvironmentVariable("KOPT_BOOTSTRAP_ADMIN_EMAIL");
        var password = Environment.GetEnvironmentVariable("KOPT_BOOTSTRAP_ADMIN_PASSWORD");
        if (string.IsNullOrWhiteSpace(email) || string.IsNullOrWhiteSpace(password)) return;
        var users = services.GetRequiredService<UserManager<AppUser>>();
        var user = await users.FindByEmailAsync(email);
        if (user is null)
        {
            user = new AppUser { UserName = email, Email = email, EmailConfirmed = true };
            IdentityResultGuard(await users.CreateAsync(user, password));
        }
        if (!await users.IsInRoleAsync(user, "Admin"))
            IdentityResultGuard(await users.AddToRoleAsync(user, "Admin"));

        var subscriptionDays = Math.Clamp(configuration.GetValue("KOPT_BOOTSTRAP_SUBSCRIPTION_DAYS", 0), 0, 3650);
        if (subscriptionDays > 0)
        {
            var product = await db.Products.SingleAsync(x => x.Slug == "ark-ase");
            var subscription = await db.Subscriptions.SingleOrDefaultAsync(x =>
                x.UserId == user.Id && x.ProductId == product.Id);
            if (subscription is null)
            {
                var now = DateTimeOffset.UtcNow;
                db.Subscriptions.Add(new Subscription
                {
                    UserId = user.Id,
                    ProductId = product.Id,
                    StartsAt = now,
                    EndsAt = now.AddDays(subscriptionDays),
                    SlotLimit = 2
                });
                await db.SaveChangesAsync();
            }
        }
    }

    private static void IdentityResultGuard(IdentityResult result)
    {
        if (!result.Succeeded)
            throw new InvalidOperationException(string.Join("; ", result.Errors.Select(x => x.Description)));
    }
}
