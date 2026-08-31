using System.Security.Claims;
using System.Text.Encodings.Web;
using Microsoft.AspNetCore.Authentication;
using Microsoft.Extensions.Options;

namespace Kopt.ControlPlane.Security;

// TODO(auth): temporary local-dev bypass so the loader/injector flow can be
// exercised against a running control-plane without a real login. Restore
// the normal Identity bearer flow before this backend is used by anyone but
// the person running it: this stamps every request as an authenticated
// admin with no credential check at all. Wired up (Program.cs) only when
// both ASPNETCORE_ENVIRONMENT=Development AND Security:DevBypassAuth=true --
// never active in Production regardless of misconfiguration, because
// ValidateProductionSecrets already fails startup outside Development.
public static class LocalDevAuth
{
    public const string SchemeName = "LocalDevBypass";

    // Fixed, well-known id so the same dev "account" is used across
    // restarts instead of minting a fresh unlinked user every run.
    public static readonly Guid DevUserId = Guid.Parse("00000000-0000-0000-0000-0000000000de");
    public const string DevUserEmail = "dev-bypass@local";
}

public sealed class LocalDevAuthHandler(
    IOptionsMonitor<AuthenticationSchemeOptions> options,
    ILoggerFactory logger,
    UrlEncoder encoder)
    : AuthenticationHandler<AuthenticationSchemeOptions>(options, logger, encoder)
{
    protected override Task<AuthenticateResult> HandleAuthenticateAsync()
    {
        var claims = new[]
        {
            new Claim(ClaimTypes.NameIdentifier, LocalDevAuth.DevUserId.ToString()),
            new Claim(ClaimTypes.Name, LocalDevAuth.DevUserEmail),
            new Claim(ClaimTypes.Role, "Admin")
        };
        var identity = new ClaimsIdentity(claims, LocalDevAuth.SchemeName);
        var ticket = new AuthenticationTicket(new ClaimsPrincipal(identity), LocalDevAuth.SchemeName);
        return Task.FromResult(AuthenticateResult.Success(ticket));
    }
}
