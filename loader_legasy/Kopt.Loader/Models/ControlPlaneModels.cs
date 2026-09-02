using System.Text.Json.Serialization;

namespace Kopt.Loader.Models;

public sealed record LoginResponse(
    [property: JsonPropertyName("tokenType")] string TokenType,
    [property: JsonPropertyName("accessToken")] string AccessToken,
    [property: JsonPropertyName("expiresIn")] int ExpiresIn,
    [property: JsonPropertyName("refreshToken")] string RefreshToken);

public sealed record Entitlement(
    string Slug,
    string Name,
    bool ProductEnabled,
    DateTimeOffset StartsAt,
    DateTimeOffset EndsAt,
    int SlotLimit,
    bool Active);

public sealed record LeaseResponse(string Capability, DateTimeOffset ExpiresAt, Guid Id,
    [property: JsonPropertyName("endsAt")] DateTimeOffset SubscriptionEndsAt);

public sealed record GameStatus(bool Running, int? ProcessId, string Path, string Hash,
    bool Supported, string Detail);

public sealed record OperationResult(bool Success, string Message, string DiagnosticsId);
