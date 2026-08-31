# KOPT Internal for ARK: Survival Evolved

Standalone Win64 payload and injector designed for ARK: Survival Evolved under
Windows or Proton. The UI is rendered inside the game's DXGI swap chain, so it
does not depend on WPF, WinForms, X11 overlays, or a desktop compositor.

This is for the verified ASE build used by the source tracker:

```text
ShooterGame.exe SHA-256: 9BC401417A776C5244A1B0B3255DC3AF4A9D73E3F5C1BA96228FBE3FB1A43477
File size:              80346664 bytes
```

The project does not target ARK: Survival Ascended. Use it only in a local or
private environment with BattlEye disabled.

## Architecture

- `kopt_injector.exe` resolves remote `kernel32!LoadLibraryW` by RVA and loads
  the payload into `ShooterGame.exe`.
- `kopt_payload.dll` hooks the shared DXGI swap-chain vtable (`Present` and
  `ResizeBuffers`) plus the PDB-validated `APlayerCameraManager::UpdateCamera`
  slot for game-thread camera, aim, and CustomDepth updates.
- The internal D3D11 renderer owns the English in-game menu and ESP. It uses a
  self-contained shader/font atlas and does not require D2D interop flags from
  the game's device.
- `ArkRuntime` resolves `GWorld` and `GNames`, traverses UE4 levels/actors, and
  keeps player, dino, structure, turret, drop, and death-cache snapshots.
- Aim and camera writes are isolated behind build offsets. The default
  `AController.ControlRotation` offset is PDB-validated at `0x490`; the legacy
  `0x4A0` value is rejected because it corrupts adjacent controller input state.

## Windows build

Install Visual Studio C++ Build Tools and run:

```powershell
.\build.ps1 -Configuration Release
```

The build performs a PE64 self-test. Output is in `build-msvc\dist`.

Start ARK without BattlEye, enter a map, then run:

```powershell
.\build-msvc\dist\kopt_injector.exe --process ShooterGame.exe --dll .\build-msvc\dist\kopt_payload.dll
```

Add `--wait` to start the injector before the game.

For a controlled payload shutdown from diagnostics or a failed smoke session:

```powershell
.\build-msvc\dist\kopt_injector.exe --process ShooterGame.exe --unload
```

The injector calls the exported cleanup request and waits until the module is no
longer mapped; it does not use `FreeLibrary` behind the payload's worker thread.

## Linux / Proton build

Install CMake, Ninja, and mingw-w64. On Debian/Ubuntu:

```bash
sudo apt install cmake ninja-build mingw-w64
./scripts/build-linux.sh
```

ARK remains a Win64 process under Proton, so both outputs intentionally remain
PE64 `.exe`/`.dll` files. Run the injector through the same Proton installation
and compatdata prefix as ARK:

```bash
export PROTON="$HOME/.steam/steam/steamapps/common/Proton - Experimental/proton"
export STEAM_COMPAT_DATA_PATH="$HOME/.steam/steam/steamapps/compatdata/346110"
./scripts/launch-proton.sh
```

`Steam AppID 346110` is ARK: Survival Evolved. If ARK uses a custom prefix,
point `STEAM_COMPAT_DATA_PATH` at that prefix instead.

## Controls

- `Home` — open/close the menu.
- `End` — restore hooks and unload the payload.
- `RMB` — default player and dino aim activation; each category has an independent key and Hold/Toggle/Always mode.
- Right-click any checkbox or switch to add a keyboard or mouse bind and choose Hold/Toggle mode.
- The `Hotkeys` tab shows every configured feature bind, its current state, list visibility and remove action.
- The compact `ACTIVE HOTKEYS` window displays visible binds only while they are active; its position is configurable.
- `ESP > Search > Structure catalog` accumulates every live structure class (including mod structures), supports name/class search and an exact saved whitelist.
- Local profiles can be deleted from the Runtime profile manager with a protected two-click confirmation; the base configuration is never a deletion target.
- Player corpses remain available to Dead ESP but are excluded from radius alerts, enemy groups, threat/radar counts, summaries and aim eligibility.
- Moving-target prediction can solve the true projectile intercept for lateral and diagonal movement; Angle catch-up increases response only at larger angular errors and preserves the configured smoothing near the target.
- Player ESP can keep Relation/Status colors for recently visible players while using a separate configurable color for players occluded by geometry. The side preview can switch between Visible and Occluded states.
- Drag the lower-right menu grip to resize it; the saved width and height are part of the local configuration.
- `WASD` and `Q/E` or `Ctrl/Space` — move the free camera.
- `Shift` — freecam sprint; mouse wheel changes its live speed.

Settings are persisted beside the DLL in `kopt_internal.ini`. Named local profiles
are stored in the adjacent `profiles` folder. The first load creates the base file automatically.

## Troubleshooting

- `OpenProcess failed (error 5)` or `VirtualAllocEx failed (error 5)` means the
  game is protected or runs at a higher integrity level. If `ShooterGame_BE.exe`
  and `BEService` are active, close that session and launch ARK with BattlEye
  disabled. Do not retry against the protected process.
- Run the injector at the same privilege level as `ShooterGame.exe`.
- `GWorld signature was not found` means the executable does not match the ASE
  build listed above. Update the signature/offset profile before using it.
- If the payload loads but the menu is not visible, use borderless/windowed
  fullscreen and disable third-party overlay layers while testing the DXGI hook.

## Ported feature surface

The draggable in-game menu includes split player/dino aim, Hold/Toggle/Always
activation, mounted-controller aim lock, selectable hit zones and an accurate
FOV circle. ESP covers players, dinosaurs, structures, detailed turrets, drops
with quantity and death caches. It includes real player bones, health/torpor,
manual tribe alliances, relation filters, configurable anchors/styles, all 17
armor and 10 weapon icons from the reference project, and a drag/drop preview
that edits the real layout. Frame-budgeted alerts cover new enemies, rapid
approach, sleep/knockout, death, nearby Noglin, hostile turret targeting and
enemy groups without running a second world scan from Present.

World discovery is round-robin and frame-budgeted. Moving actors, vitals and
bones use a separate fast path, while static actors and equipment use reduced
cadences. Dense bases therefore do not turn a full discovery into one Present
hitch, and no actor is permanently starved.

Game INI and live CVar editing are intentionally excluded from the current scope.
The Visuals tab only owns runtime/overlay settings. The Chams tab uses the matching PDB's native CustomDepth functions,
separate local/enemy stencil values, nearest-component budgeting, stable writes,
and original flag/stencil restoration on disable/unload.

The signature and offsets are build-specific by design. If ARK updates, the
payload stops at the `GWorld`/`GNames` validation instead of traversing an
unrecognized layout.

## Commercial control plane

`backend/Kopt.ControlPlane` contains the ASP.NET Core/PostgreSQL backend and the
purple-black administration panel. It implements account auth, one-time license
keys, subscription time, HWID slots, short-lived signed leases, force logout,
quarantine, multi-signal tamper incidents, emergency stop and audited admin
mutations. See `backend/README.md` for secret generation and deployment.
