using System.Security.Cryptography;
using System.Text;

namespace Kopt.ControlPlane.Security;

public sealed class SecretHasher(IConfiguration configuration)
{
    private readonly byte[] _pepper = Convert.FromBase64String(configuration["Security:HashPepper"]
        ?? throw new InvalidOperationException("Security:HashPepper must be a base64 secret."));

    public string Hash(string value)
    {
        using var hmac = new HMACSHA256(_pepper);
        return Convert.ToHexString(hmac.ComputeHash(Encoding.UTF8.GetBytes(value.Trim())));
    }

    public static string NewLicenseCode()
    {
        var bytes = RandomNumberGenerator.GetBytes(24);
        return $"KOPT-{Convert.ToHexString(bytes)[..8]}-{Convert.ToHexString(bytes)[8..16]}-{Convert.ToHexString(bytes)[16..24]}-{Convert.ToHexString(bytes)[24..32]}";
    }
}
