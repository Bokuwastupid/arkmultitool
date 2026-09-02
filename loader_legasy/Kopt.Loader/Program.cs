using Avalonia;
using System;

namespace Kopt.Loader;

internal static class Program
{
    // Initialization code. Don't use any Avalonia, third-party APIs or any
    // SynchronizationContext-reliant code before AppMain is called: things aren't initialized
    // yet and stuff might break.
    [STAThread]
    public static int Main(string[] args)
    {
        if (args.Contains("--self-test", StringComparer.Ordinal)) return SelfTest();
        BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
        return 0;
    }

    // Avalonia configuration, don't remove; also used by visual designer.
    public static AppBuilder BuildAvaloniaApp()
        => AppBuilder.Configure<App>()
            .UsePlatformDetect()
#if DEBUG
            .WithDeveloperTools()
#endif
            .WithInterFont()
            .LogToTrace();

    private static int SelfTest()
    {
        try
        {
            using var controlPlane = new Services.ControlPlaneClient();
            var fingerprint = Services.DeviceIdentity.Fingerprint();
            if (fingerprint.Length != 64 || !fingerprint.All(Uri.IsHexDigit))
                throw new InvalidOperationException("Device fingerprint is not SHA-256 hex.");
            if (Services.GameCoordinator.ExpectedHash.Length != 64)
                throw new InvalidOperationException("Target build pin is malformed.");
            Console.WriteLine($"[KOPT] Loader self-test passed; API={controlPlane.BaseAddress}; OS={Environment.OSVersion.Platform}");
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine($"[KOPT] Loader self-test failed: {error.Message}");
            return 1;
        }
    }
}
