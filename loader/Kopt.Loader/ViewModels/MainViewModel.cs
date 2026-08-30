using System.Security.Cryptography;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Kopt.Loader.Models;
using Kopt.Loader.Services;

namespace Kopt.Loader.ViewModels;

public partial class MainViewModel : ViewModelBase
{
    private readonly ControlPlaneClient controlPlane = new();
    private readonly GameCoordinator gameCoordinator = new();
    private GameStatus game = new(false, null, "Not detected", "—", false, "Run a scan.");

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsLoginVisible))]
    [NotifyPropertyChangedFor(nameof(IsShellVisible))]
    public partial bool IsAuthenticated { get; set; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsDashboard))]
    [NotifyPropertyChangedFor(nameof(IsDiagnostics))]
    [NotifyPropertyChangedFor(nameof(IsSettings))]
    public partial string ActivePage { get; set; } = "Dashboard";

    [ObservableProperty] public partial string Email { get; set; } = string.Empty;
    [ObservableProperty] public partial string Password { get; set; } = string.Empty;
    [ObservableProperty] public partial bool IsBusy { get; set; }
    [ObservableProperty] public partial string BusyText { get; set; } = string.Empty;
    [ObservableProperty] public partial string LoginMessage { get; set; } = string.Empty;
    [ObservableProperty] public partial string OperationMessage { get; set; } = "Ready for validation.";
    [ObservableProperty] public partial string DiagnosticsId { get; set; } = "—";
    [ObservableProperty] public partial string AccountLabel { get; set; } = "Signed out";
    [ObservableProperty] public partial string SubscriptionLabel { get; set; } = "No entitlement loaded";
    [ObservableProperty] public partial string SubscriptionDetail { get; set; } = "Sign in to refresh the subscription.";
    [ObservableProperty] public partial string ProductState { get; set; } = "UNKNOWN";
    [ObservableProperty] public partial string GameState { get; set; } = "NOT DETECTED";
    [ObservableProperty] public partial string GameDetail { get; set; } = "Run target scan.";
    [ObservableProperty] public partial string GamePath { get; set; } = "—";
    [ObservableProperty] public partial string GameHash { get; set; } = "—";
    [ObservableProperty] public partial string LoaderVersion { get; set; } = "0.1.0-dev";
    [ObservableProperty] public partial bool ReducedMotion { get; set; }
    [ObservableProperty] public partial double UiScale { get; set; } = 1.0;

    public bool IsLoginVisible => !IsAuthenticated;
    public bool IsShellVisible => IsAuthenticated;
    public bool IsDashboard => ActivePage == "Dashboard";
    public bool IsDiagnostics => ActivePage == "Diagnostics";
    public bool IsSettings => ActivePage == "Settings";
    public string ApiAddress => controlPlane.BaseAddress.ToString().TrimEnd('/');

    [RelayCommand]
    private async Task LoginAsync()
    {
        if (IsBusy) return;
        if (string.IsNullOrWhiteSpace(Email) || string.IsNullOrWhiteSpace(Password))
        {
            LoginMessage = "Email and password are required.";
            return;
        }
        await RunBusy("Authorizing device…", async cancellationToken =>
        {
            await controlPlane.LoginAsync(Email.Trim(), Password, cancellationToken);
            Password = string.Empty;
            AccountLabel = Email.Trim();
            IsAuthenticated = true;
            LoginMessage = string.Empty;
            await RefreshCore(cancellationToken);
        }, error => LoginMessage = error);
    }

    [RelayCommand]
    private void Logout()
    {
        controlPlane.Logout();
        IsAuthenticated = false;
        Password = string.Empty;
        AccountLabel = "Signed out";
        SubscriptionLabel = "No entitlement loaded";
        ProductState = "UNKNOWN";
        OperationMessage = "Session credentials cleared from memory.";
    }

    [RelayCommand]
    private void Navigate(string page) => ActivePage = page;

    [RelayCommand]
    private async Task RefreshAsync()
    {
        if (IsBusy) return;
        await RunBusy("Refreshing product state…", RefreshCore,
            error => OperationMessage = error);
    }

    [RelayCommand]
    private async Task ScanGameAsync()
    {
        if (IsBusy) return;
        await RunBusy("Hashing target build…", async cancellationToken =>
        {
            game = await gameCoordinator.ScanAsync(cancellationToken);
            ApplyGame(game);
        }, error => OperationMessage = error);
    }

    [RelayCommand]
    private async Task LaunchAsync()
    {
        if (IsBusy) return;
        await RunBusy("Validating entitlement and target…", async cancellationToken =>
        {
            var entitlements = await controlPlane.GetEntitlementsAsync(cancellationToken);
            _ = entitlements.FirstOrDefault(x => x.Slug == "ark-ase" && x.Active && x.ProductEnabled)
                ?? throw new InvalidOperationException("An active ARK entitlement is required.");
            game = await gameCoordinator.ScanAsync(cancellationToken);
            ApplyGame(game);
            if (!game.Running || !game.Supported)
                throw new InvalidOperationException(game.Detail);
            BusyText = "Acquiring signed device lease…";
            var sessionId = Token(24);
            var nonce = Token(32);
            var lease = await controlPlane.AcquireLeaseAsync(DeviceIdentity.Fingerprint(),
                DeviceIdentity.DisplayName, sessionId, nonce, cancellationToken);
            if (lease.ExpiresAt <= DateTimeOffset.UtcNow)
                throw new InvalidOperationException("Control plane returned an expired lease.");
            BusyText = "Starting version-pinned injector…";
            var result = await gameCoordinator.InjectAsync(game, cancellationToken);
            DiagnosticsId = result.DiagnosticsId;
            OperationMessage = result.Message;
            if (!result.Success) throw new InvalidOperationException(result.Message);
        }, error => OperationMessage = error);
    }

    private async Task RefreshCore(CancellationToken cancellationToken)
    {
        var entitlements = await controlPlane.GetEntitlementsAsync(cancellationToken);
        var entitlement = entitlements.FirstOrDefault(x => x.Slug == "ark-ase");
        if (entitlement is null)
        {
            SubscriptionLabel = "No ARK subscription";
            SubscriptionDetail = "Activate a license key in your account.";
            ProductState = "NO ACCESS";
        }
        else
        {
            SubscriptionLabel = entitlement.Active ? "Subscription active" : "Subscription inactive";
            SubscriptionDetail = $"Until {entitlement.EndsAt.LocalDateTime:g} · {entitlement.SlotLimit} device slot(s)";
            ProductState = entitlement.Active && entitlement.ProductEnabled ? "OPERATIONAL" : "LOCKED";
        }
        game = await gameCoordinator.ScanAsync(cancellationToken);
        ApplyGame(game);
        OperationMessage = "Product state refreshed.";
    }

    private void ApplyGame(GameStatus status)
    {
        GameState = !status.Running ? "NOT RUNNING" : status.Supported ? "SUPPORTED" : "BLOCKED";
        GameDetail = status.Detail;
        GamePath = status.Path;
        GameHash = status.Hash;
    }

    private async Task RunBusy(string label, Func<CancellationToken, Task> operation, Action<string> failure)
    {
        IsBusy = true;
        BusyText = label;
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(45));
        try { await operation(timeout.Token); }
        catch (OperationCanceledException) { failure("Operation timed out. No injection retry was performed."); }
        catch (Exception error) { failure(error.Message); }
        finally { BusyText = string.Empty; IsBusy = false; }
    }

    private static string Token(int bytes) => Convert.ToBase64String(RandomNumberGenerator.GetBytes(bytes))
        .TrimEnd('=').Replace('+', '-').Replace('/', '_');
}
