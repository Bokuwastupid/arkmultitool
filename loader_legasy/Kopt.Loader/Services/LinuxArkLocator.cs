using System.Text.RegularExpressions;

namespace Kopt.Loader.Services;

// Mirrors scripts/lib/steam.sh's discovery logic in-process so the loader
// doesn't need to shell out to bash for the Linux inject path. Keep the two
// in sync if the Steam layout assumptions change.
public sealed record ArkInstall(string GameDir, string Win64Dir, string AppId, string CompatDataDir, string? ProtonRealVersionDll);

public static class LinuxArkLocator
{
    public static ArkInstall? Locate()
    {
        foreach (var root in SteamRoots())
        foreach (var library in LibraryPaths(root))
        {
            var steamapps = Path.Combine(library, "steamapps");
            if (!Directory.Exists(steamapps)) continue;
            var gameDir = FindGameDir(steamapps);
            if (gameDir is null) continue;
            var appId = FindAppId(steamapps, Path.GetFileName(gameDir));
            if (appId is null) continue;
            var win64Dir = Path.Combine(gameDir, "ShooterGame", "Binaries", "Win64");
            var compatData = Path.Combine(steamapps, "compatdata", appId);
            var realVersionDll = FindProtonRealVersionDll(steamapps);
            return new ArkInstall(gameDir, win64Dir, appId, compatData, realVersionDll);
        }
        return null;
    }

    // The authoritative "real" version.dll doesn't reliably exist inside a
    // per-game prefix's own system32 -- Wine only materializes it there if
    // something already copied it in, and a fresh or cleaned-up prefix can
    // run the whole game fine on Wine's internal builtin with no file on
    // disk at all (confirmed: removing it here didn't make Wine recreate
    // it, and ARK kept running). The one place a real copy is guaranteed is
    // the Proton installation itself, which ships its builtin DLLs as real
    // PE files. Matches ark_fun_tools' internal/install/deploy.protonSystemDLL.
    private static string? FindProtonRealVersionDll(string steamapps)
    {
        var common = Path.Combine(steamapps, "common");
        if (!Directory.Exists(common)) return null;
        string? newest = null;
        var newestWrite = DateTime.MinValue;
        foreach (var dir in Directory.EnumerateDirectories(common, "Proton*"))
        {
            var candidate = Path.Combine(dir, "files", "lib", "wine", "x86_64-windows", "version.dll");
            if (!File.Exists(candidate)) continue;
            var writeTime = File.GetLastWriteTimeUtc(candidate);
            if (newest is null || writeTime > newestWrite) { newest = candidate; newestWrite = writeTime; }
        }
        return newest;
    }

    private static IEnumerable<string> SteamRoots()
    {
        var overrideRoot = Environment.GetEnvironmentVariable("KOPT_STEAM_ROOT");
        if (!string.IsNullOrWhiteSpace(overrideRoot))
        {
            yield return overrideRoot;
            yield break;
        }
        var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        string[] candidates =
        [
            Path.Combine(home, ".local", "share", "Steam"),
            Path.Combine(home, ".steam", "steam"),
            Path.Combine(home, ".steam", "root"),
            Path.Combine(home, ".var", "app", "com.valvesoftware.Steam", ".local", "share", "Steam"),
            Path.Combine(home, "snap", "steam", "common", ".local", "share", "Steam"),
        ];
        foreach (var candidate in candidates)
            if (Directory.Exists(Path.Combine(candidate, "steamapps")))
                yield return candidate;
    }

    private static IEnumerable<string> LibraryPaths(string root)
    {
        yield return root;
        var vdf = Path.Combine(root, "steamapps", "libraryfolders.vdf");
        if (!File.Exists(vdf)) yield break;
        var text = File.ReadAllText(vdf);
        foreach (Match match in Regex.Matches(text, "\"path\"\\s*\"([^\"]+)\""))
            yield return match.Groups[1].Value;
    }

    private static string? FindGameDir(string steamapps)
    {
        var common = Path.Combine(steamapps, "common");
        if (!Directory.Exists(common)) return null;
        foreach (var dir in Directory.EnumerateDirectories(common))
        {
            if (!Path.GetFileName(dir).StartsWith("ARK", StringComparison.OrdinalIgnoreCase)) continue;
            var exe = Path.Combine(dir, "ShooterGame", "Binaries", "Win64", "ShooterGame.exe");
            if (File.Exists(exe)) return dir;
        }
        return null;
    }

    private static string? FindAppId(string steamapps, string installDirName)
    {
        foreach (var manifest in Directory.EnumerateFiles(steamapps, "appmanifest_*.acf"))
        {
            var text = File.ReadAllText(manifest);
            if (!Regex.IsMatch(text, $"\"installdir\"\\s*\"{Regex.Escape(installDirName)}\"")) continue;
            var match = Regex.Match(Path.GetFileName(manifest), @"appmanifest_(\d+)\.acf");
            if (match.Success) return match.Groups[1].Value;
        }
        return null;
    }
}
