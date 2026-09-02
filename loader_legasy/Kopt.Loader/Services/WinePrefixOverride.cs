namespace Kopt.Loader.Services;

// Wine's DllOverrides registry section decides "native" (a real file on
// disk) vs "builtin" (Wine's own compiled-in implementation) per DLL name.
// Dropping version.dll into the prefix's system32 is not enough on its own:
// Wine may still prefer its own builtin version.dll unless told otherwise.
// Ported from ark_fun_tools' internal/platform/prefix/registry.go, which
// already validated this against a live Proton prefix -- keep the two in
// sync if Wine's user.reg format assumptions ever change.
public static class WinePrefixOverride
{
    private const string OverrideLine = "\"version\"=\"native,builtin\"";
    private const string OverridesSection = "[Software\\\\Wine\\\\DllOverrides]";

    public static bool HasOverride(string userReg)
    {
        if (!File.Exists(userReg)) return false;
        return File.ReadAllText(userReg).Contains(OverrideLine, StringComparison.Ordinal);
    }

    public static void AddOverride(string userReg)
    {
        var text = File.ReadAllText(userReg);
        if (text.Contains(OverrideLine, StringComparison.Ordinal)) return;

        var sectionIndex = text.IndexOf(OverridesSection, StringComparison.Ordinal);
        if (sectionIndex < 0)
            throw new InvalidOperationException($"No {OverridesSection} section in {userReg}.");

        // Insert right after the section header and the "#time=" stamp Wine
        // writes under every section header -- inserting before it would
        // put our line ahead of Wine's own timestamp comment, which Wine's
        // own writer never does and better not to be the first to try.
        var afterHeaderNewline = text.IndexOf('\n', sectionIndex);
        if (afterHeaderNewline < 0)
            throw new InvalidOperationException($"{OverridesSection} section is truncated in {userReg}.");
        var cut = afterHeaderNewline + 1;
        if (text.AsSpan(cut).StartsWith("#time="))
        {
            var afterTimestamp = text.IndexOf('\n', cut);
            if (afterTimestamp >= 0) cut = afterTimestamp + 1;
        }

        var updated = text[..cut] + OverrideLine + "\n" + text[cut..];
        WriteAtomic(userReg, updated);
    }

    public static void RemoveOverride(string userReg)
    {
        if (!File.Exists(userReg)) return;
        var lines = File.ReadAllLines(userReg)
            .Where(line => line.Trim() != OverrideLine);
        WriteAtomic(userReg, string.Join('\n', lines));
    }

    // Wine rewrites user.reg wholesale on every prefix shutdown; losing this
    // file mid-write would leave the prefix registry corrupt and the game
    // unable to start. Write-then-rename makes a torn write impossible.
    private static void WriteAtomic(string path, string content)
    {
        var tmp = path + ".kopt-tmp";
        File.WriteAllText(tmp, content);
        File.Move(tmp, path, overwrite: true);
    }
}
