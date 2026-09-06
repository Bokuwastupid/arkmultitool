using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using System.Security.Cryptography;
using System.Text;
using Microsoft.Win32;

namespace Kopt.Loader.Services;

public static class DeviceIdentity
{
    public static string DisplayName => $"{Environment.MachineName} · {RuntimeInformation.OSDescription}";

    public static string Fingerprint()
    {
        var stableId = OperatingSystem.IsWindows() ? WindowsMachineId() : UnixMachineId();
        var material = $"kopt-device-v1|{stableId}|{RuntimeInformation.OSArchitecture}";
        return Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(material)));
    }

    [SupportedOSPlatform("windows")]
    private static string WindowsMachineId()
    {
        using var key = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Cryptography", false);
        return key?.GetValue("MachineGuid") as string
            ?? throw new InvalidOperationException("Windows MachineGuid is unavailable.");
    }

    private static string UnixMachineId()
    {
        foreach (var path in new[] { "/etc/machine-id", "/var/lib/dbus/machine-id" })
            if (File.Exists(path)) return File.ReadAllText(path).Trim();
        throw new InvalidOperationException("Linux machine-id is unavailable.");
    }
}
