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
            return await InjectLinuxAsync(game, diagnosticsId, cancellationToken);
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

    // "Inject" on Linux means installing the version.dll auto-load proxy --
    // there is no live-attach path here: ARK runs inside the Steam Linux
    // Runtime's bwrap sandbox, which an external process on the host cannot
    // reach without root (confirmed against a real running game; nsenter
    // into its mount namespace fails with EPERM for a normal user). The
    // proxy loads itself on the *next* process start instead.
    //
    // Everything lands in the Wine prefix's own system32
    // (compatdata/<appid>/pfx/drive_c/windows/system32), never in the
    // Steam-tracked game directory: dropping files there once made Steam's
    // integrity check decide the depot had changed and queue a ~50k-chunk
    // "update" that clobbered the install. The prefix is unmanaged user
    // data Steam's verifier never inspects. This also needs a Wine
    // DllOverrides registry entry (WinePrefixOverride) -- Wine may prefer
    // its own builtin version.dll over a same-named file on disk unless
    // told "native,builtin". Matches the proven approach in
    // ark_fun_tools/client/internal/platform/prefix (same version.dll
    // target, same 16-export forwarding, same registry line).
    private static async Task<OperationResult> InjectLinuxAsync(GameStatus game, string diagnosticsId,
        CancellationToken cancellationToken)
    {
        var install = LinuxArkLocator.Locate();
        if (install is null)
            return new(false, "ARK install not found in any Steam library. Set KOPT_STEAM_ROOT if Steam lives somewhere nonstandard.", diagnosticsId);

        var proxyVersion = ResolveArtifact("KOPT_LINUX_VERSION_PROXY_PATH", "version.dll");
        var proxyPayload = ResolveArtifact("KOPT_LINUX_PAYLOAD_PATH", "kopt_payload.dll");
        if (!File.Exists(proxyVersion) || !File.Exists(proxyPayload))
            return new(false,
                "version.dll/kopt_payload.dll were not found beside the loader. Run scripts/build-linux.sh and " +
                "copy build-mingw/dist/{version.dll,kopt_payload.dll} next to Kopt.Loader.dll, or set " +
                "KOPT_LINUX_VERSION_PROXY_PATH / KOPT_LINUX_PAYLOAD_PATH.", diagnosticsId);

        var system32 = Path.Combine(install.CompatDataDir, "pfx", "drive_c", "windows", "system32");
        var userReg = Path.Combine(install.CompatDataDir, "pfx", "user.reg");
        var targetVersion = Path.Combine(system32, "version.dll");
        var targetOrig = Path.Combine(system32, "version_orig.dll");
        var targetPayload = Path.Combine(system32, "kopt_payload.dll");

        try
        {
            await Task.Run(() =>
            {
                if (!Directory.Exists(system32))
                    throw new InvalidOperationException(
                        $"No system32 in the Wine prefix ({system32}). Run ARK at least once so Proton finishes setting it up.");
                if (!File.Exists(targetOrig))
                {
                    // Prefer whatever's already sitting in the prefix (if this is
                    // a fresh, untouched prefix, Wine may have put a real copy
                    // there) but fall back to the Proton installation's own copy
                    // -- confirmed the prefix one isn't reliably present.
                    var source = File.Exists(targetVersion) ? targetVersion : install.ProtonRealVersionDll;
                    if (source is null)
                        throw new InvalidOperationException(
                            $"No real version.dll found in the prefix ({targetVersion}) or any installed Proton's " +
                            "files/lib/wine/x86_64-windows/version.dll.");
                    File.Copy(source, targetOrig);
                }
                File.Copy(proxyVersion, targetVersion, overwrite: true);
                File.Copy(proxyPayload, targetPayload, overwrite: true);
                if (!WinePrefixOverride.HasOverride(userReg))
                    WinePrefixOverride.AddOverride(userReg);
            }, cancellationToken);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidOperationException)
        {
            return new(false, error.Message, diagnosticsId);
        }

        var message = game.Running
            ? "Proxy installed. ShooterGame.exe is already running -- it only loads at process start, so restart ARK for it to take effect."
            : "Proxy installed. It will auto-load the next time ARK is launched through Proton -- no further action needed.";
        return new(true, message, diagnosticsId);
    }

    // Reverses InjectLinuxAsync: restores the real version.dll, removes
    // kopt_payload.dll and the registry override. Windows has nothing
    // persistent to remove -- CreateRemoteThread injection there doesn't
    // touch disk, so there's nothing for an uninstall to undo.
    public async Task<OperationResult> UninstallAsync(CancellationToken cancellationToken)
    {
        var diagnosticsId = Guid.NewGuid().ToString("N")[..12].ToUpperInvariant();
        if (OperatingSystem.IsWindows())
            return new(true, "Nothing to remove on Windows: injection doesn't write anything to disk.", diagnosticsId);

        var install = LinuxArkLocator.Locate();
        if (install is null)
            return new(false, "ARK install not found in any Steam library. Set KOPT_STEAM_ROOT if Steam lives somewhere nonstandard.", diagnosticsId);

        var system32 = Path.Combine(install.CompatDataDir, "pfx", "drive_c", "windows", "system32");
        var userReg = Path.Combine(install.CompatDataDir, "pfx", "user.reg");
        var targetVersion = Path.Combine(system32, "version.dll");
        var targetOrig = Path.Combine(system32, "version_orig.dll");
        var targetPayload = Path.Combine(system32, "kopt_payload.dll");

        try
        {
            await Task.Run(() =>
            {
                if (File.Exists(targetPayload)) File.Delete(targetPayload);
                if (File.Exists(targetOrig))
                {
                    // Overwrite-then-delete, not delete-then-move: a crash between
                    // the two steps must never leave version.dll missing outright.
                    File.Copy(targetOrig, targetVersion, overwrite: true);
                    File.Delete(targetOrig);
                }
                if (WinePrefixOverride.HasOverride(userReg))
                    WinePrefixOverride.RemoveOverride(userReg);
            }, cancellationToken);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            return new(false, error.Message, diagnosticsId);
        }

        var message = Process.GetProcessesByName("ShooterGame").Any()
            ? "Proxy removed. ShooterGame.exe is still running on the old version.dll -- restart ARK to fully revert."
            : "Proxy removed. The prefix is back to vanilla.";
        return new(true, message, diagnosticsId);
    }
}
