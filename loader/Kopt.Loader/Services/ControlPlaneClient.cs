using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Text.Json;
using Kopt.Loader.Models;

namespace Kopt.Loader.Services;

public sealed class ControlPlaneClient : IDisposable
{
    private static readonly JsonSerializerOptions Json = new(JsonSerializerDefaults.Web);
    private readonly HttpClient client;
    private string? accessToken;

    public ControlPlaneClient()
    {
        var configured = Environment.GetEnvironmentVariable("KOPT_API_URL") ?? "http://127.0.0.1:5087";
        var uri = new Uri(configured, UriKind.Absolute);
        if (uri.Scheme != Uri.UriSchemeHttps && !uri.IsLoopback)
            throw new InvalidOperationException("KOPT_API_URL must use HTTPS outside loopback development.");
        client = new HttpClient { BaseAddress = uri, Timeout = TimeSpan.FromSeconds(15) };
    }

    public Uri BaseAddress => client.BaseAddress!;
    public bool Authenticated => !string.IsNullOrWhiteSpace(accessToken);

    public async Task LoginAsync(string email, string password, CancellationToken cancellationToken)
    {
        using var response = await client.PostAsJsonAsync("/api/auth/login?useCookies=false&useSessionCookies=false",
            new { email, password }, Json, cancellationToken);
        await EnsureSuccess(response, cancellationToken);
        var login = await response.Content.ReadFromJsonAsync<LoginResponse>(Json, cancellationToken)
            ?? throw new InvalidOperationException("Authorization response was empty.");
        accessToken = login.AccessToken;
        client.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue(login.TokenType, login.AccessToken);
    }

    public async Task<IReadOnlyList<Entitlement>> GetEntitlementsAsync(CancellationToken cancellationToken)
    {
        EnsureAuthenticated();
        using var response = await client.GetAsync("/api/client/entitlements", cancellationToken);
        await EnsureSuccess(response, cancellationToken);
        return await response.Content.ReadFromJsonAsync<List<Entitlement>>(Json, cancellationToken) ?? [];
    }

    public async Task<LeaseResponse> AcquireLeaseAsync(string fingerprint, string deviceName,
        string sessionId, string nonce, CancellationToken cancellationToken)
    {
        EnsureAuthenticated();
        using var response = await client.PostAsJsonAsync("/api/client/leases/acquire", new
        {
            product = "ark-ase",
            deviceFingerprint = fingerprint,
            deviceName,
            sessionId,
            nonce,
            channel = "stable"
        }, Json, cancellationToken);
        await EnsureSuccess(response, cancellationToken);
        return await response.Content.ReadFromJsonAsync<LeaseResponse>(Json, cancellationToken)
            ?? throw new InvalidOperationException("Lease response was empty.");
    }

    public void Logout()
    {
        accessToken = null;
        client.DefaultRequestHeaders.Authorization = null;
    }

    private void EnsureAuthenticated()
    {
        if (!Authenticated) throw new InvalidOperationException("Sign in before contacting the control plane.");
    }

    private static async Task EnsureSuccess(HttpResponseMessage response, CancellationToken cancellationToken)
    {
        if (response.IsSuccessStatusCode) return;
        var body = await response.Content.ReadAsStringAsync(cancellationToken);
        try
        {
            using var document = JsonDocument.Parse(body);
            if (document.RootElement.TryGetProperty("detail", out var detail))
                throw new InvalidOperationException(detail.GetString() ?? $"Request failed ({(int)response.StatusCode}).");
            if (document.RootElement.TryGetProperty("title", out var title))
                throw new InvalidOperationException(title.GetString() ?? $"Request failed ({(int)response.StatusCode}).");
        }
        catch (JsonException) { }
        throw new InvalidOperationException($"Control plane request failed ({(int)response.StatusCode}).");
    }

    public void Dispose() => client.Dispose();
}
