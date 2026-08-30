using System.Security.Cryptography;
using System.Text.Json;
using Microsoft.AspNetCore.WebUtilities;

namespace Kopt.ControlPlane.Security;

public sealed record Capability(Guid LeaseId, Guid UserId, Guid ProductId, Guid DeviceId,
    string SessionId, string Channel, DateTimeOffset ExpiresAt);

public sealed class CapabilitySigner(IConfiguration configuration)
{
    private readonly string? _privatePem = configuration["Security:CapabilityPrivateKeyPem"];
    private readonly string? _privatePkcs8 = configuration["Security:CapabilityPrivateKeyPkcs8"];

    public string Sign(Capability capability)
    {
        var payload = JsonSerializer.SerializeToUtf8Bytes(capability);
        using var key = ECDsa.Create();
        if (!string.IsNullOrWhiteSpace(_privatePkcs8))
            key.ImportPkcs8PrivateKey(Convert.FromBase64String(_privatePkcs8), out _);
        else if (!string.IsNullOrWhiteSpace(_privatePem))
            key.ImportFromPem(_privatePem);
        else
            throw new InvalidOperationException("Security:CapabilityPrivateKeyPkcs8 or CapabilityPrivateKeyPem is required.");
        var signature = key.SignData(payload, HashAlgorithmName.SHA256);
        return $"{WebEncoders.Base64UrlEncode(payload)}.{WebEncoders.Base64UrlEncode(signature)}";
    }
}
