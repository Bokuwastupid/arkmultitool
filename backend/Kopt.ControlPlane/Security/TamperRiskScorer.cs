namespace Kopt.ControlPlane.Security;

public sealed record TamperRiskDecision(int Score, string[] TrustedSignals, bool ShouldQuarantine);

public static class TamperRiskScorer
{
    private static readonly IReadOnlyDictionary<string, int> Weights = new Dictionary<string, int>(StringComparer.Ordinal)
    {
        ["integrity_mismatch"] = 65,
        ["signer_mismatch"] = 65,
        ["executable_patch"] = 55,
        ["token_replay"] = 80,
        ["lease_replay"] = 70,
        ["injected_module"] = 35,
        ["api_hook"] = 35,
        ["debugger_attached"] = 30,
        ["clock_tamper"] = 25
    };

    public static TamperRiskDecision Evaluate(IEnumerable<string>? signals)
    {
        var trusted = (signals ?? [])
            .Select(x => x?.Trim().ToLowerInvariant())
            .Where(x => x is not null && Weights.ContainsKey(x))
            .Cast<string>()
            .Distinct(StringComparer.Ordinal)
            .Take(16)
            .ToArray();
        var score = Math.Min(100, trusted.Sum(x => Weights[x]));
        return new TamperRiskDecision(score, trusted, score >= 90 && trusted.Length >= 2);
    }
}
