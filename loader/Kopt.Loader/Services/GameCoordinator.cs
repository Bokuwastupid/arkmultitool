using System.Diagnostics;
using System.Security.Cryptography;
using Kopt.Loader.Models;

namespace Kopt.Loader.Services;

public sealed class GameCoordinator
{
    public const string ExpectedHash = "9BC401417A776C5244A1B0B3255DC3AF4A9D73E3F5C1BA96228FBE3FB1A43477";

    public async Task<GameStatus> ScanAsync(CancellationToken cancellationToken)
    {
        var process = Process.GetProcessesByName("ShooterGame").OrderByDescending(x => x.StartTime).FirstOrDefault();
        if (process is null)
            return new(false, null, "Not detected", "—", false, "Start ARK: Survival Evolved without BattlEye.");
        try
        {
            var path = process.MainModule?.FileName ?? string.Empty;
            if (string.IsNullOrWhiteSpace(path) || !File.Exists(path))
                return new(true, process.Id, path, "Unavailable", false, "Target path is inaccessible.");
            await using var stream = new FileStream(path, FileMode.Open, FileAccess.Read,
                FileShare.ReadWrite | FileShare.Delete, 1024 * 1024, true);
            var hash = Convert.ToHexString(await SHA256.HashDataAsync(stream, cancellationToken));
            return new(true, process.Id, path, hash, hash == ExpectedHash,
                hash == ExpectedHash ? "Exact supported ASE build." : "Unsupported executable build.");
        }
        catch (Exception error) when (error is not OperationCanceledException)
        {
            return new(true, process.Id, "Unavailable", "Unavailable", false, error.Message);
        }
        finally { process.Dispose(); }
    }

    public async Task<OperationResult> InjectAsync(GameStatus game, CancellationToken cancellationToken)
    {
        var diagnosticsId = Guid.NewGuid().ToString("N")[..12].ToUpperInvariant();
        if (!OperatingSystem.IsWindows())
            return new(false, "Linux host requires the Proton/Wine backend; native Windows injection was not attempted.", diagnosticsId);
        if (!game.Running || !game.Supported || game.ProcessId is null)
            return new(false, "A supported running ShooterGame.exe is required.", diagnosticsId);

        var injector = ResolveArtifact("KOPT_INJECTOR_PATH", "kopt_injector.exe");
        var payload = ResolveArtifact("KOPT_PAYLOAD_PATH", "kopt_payload_candidate.dll");
        if (!File.Exists(injector) || !File.Exists(payload))
            return new(false, "Signed injector/payload artifacts were not found beside the loader.", diagnosticsId);

        var start = new ProcessStartInfo
        {
            FileName = injector,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };
        start.ArgumentList.Add("--pid");
        start.ArgumentList.Add(game.ProcessId.Value.ToString());
        start.ArgumentList.Add("--dll");
        start.ArgumentList.Add(payload);
        using var process = Process.Start(start) ?? throw new InvalidOperationException("Injector process did not start.");
        var output = process.StandardOutput.ReadToEndAsync(cancellationToken);
        var error = process.StandardError.ReadToEndAsync(cancellationToken);
        await process.WaitForExitAsync(cancellationToken);
        var text = ((await output) + " " + (await error)).Trim();
        return process.ExitCode == 0
            ? new(true, "Payload loaded; waiting for the in-game handshake.", diagnosticsId)
            : new(false, string.IsNullOrWhiteSpace(text) ? $"Injector exit code {process.ExitCode}." : text, diagnosticsId);
    }

    private static string ResolveArtifact(string variable, string fileName)
    {
        var configured = Environment.GetEnvironmentVariable(variable);
        return string.IsNullOrWhiteSpace(configured) ? Path.Combine(AppContext.BaseDirectory, fileName) : configured;
    }
}
