# Linux build dependencies

The native module targets Win64 unconditionally (`CMakeLists.txt` refuses
to configure on a non-Windows `CMAKE_SYSTEM_NAME` without the mingw-w64
toolchain file) because ARK: Survival Evolved's `ShooterGame.exe` is a
Windows binary run under Proton — there is no Linux target to build for
natively. `scripts/build-linux.sh` cross-compiles it from any distro via
mingw-w64.

Windows itself is unaffected by anything below: `build.ps1` +
`scripts/build-loader.ps1` (win-x64) still build with MSVC/pwsh exactly as
before.

## One-shot dependency check

`scripts/build-linux.sh` and `scripts/build-loader.sh` detect your distro's
package manager (`/etc/os-release`) and print the exact install command if
something is missing; pass `--install` to have them run it via `sudo`
instead of just printing it. The table below is the reference if you'd
rather install by hand, or your distro isn't auto-detected.

| Tool | Debian/Ubuntu (apt) | Fedora/RHEL (dnf) | Arch (pacman) | openSUSE (zypper) | Alpine (apk) |
|---|---|---|---|---|---|
| CMake | `cmake` | `cmake` | `cmake` | `cmake` | `cmake` |
| Ninja | `ninja-build` | `ninja-build` | `ninja` | `ninja` | `samurai` |
| mingw-w64 C++ | `g++-mingw-w64-x86-64` | `mingw64-gcc-c++` | `mingw-w64-gcc` | `mingw64-cross-gcc-c++` | `mingw-w64-gcc` |

## .NET SDK (loader, net10.0)

Distro packages for a specific .NET major version lag and vary in name
(and Alpine doesn't ship one for every version at all), so the reliable
path on any distro is Microsoft's own installer, which needs no root and
no package manager:

```bash
curl -sSL https://dot.net/v1/dotnet-install.sh | bash -s -- --channel 10.0
export PATH="$HOME/.dotnet:$PATH"   # add to your shell profile to persist
```

`scripts/build-loader.sh` checks for this automatically and prints the
same command if `dotnet --list-sdks` doesn't show a major version >= 10.

## NixOS

Not covered by the table above since Nix has no "install this system-wide"
step; scripts detect `nixos` in `/etc/os-release` and print the equivalent
`nix profile install` line instead of an apt/dnf/pacman command.

## Running against the game (Proton)

`scripts/launch-proton.sh` auto-detects `PROTON` and `STEAM_COMPAT_DATA_PATH`
by walking Steam's library folders (native `~/.local/share/Steam`,
`~/.steam/steam`, Flatpak `~/.var/app/com.valvesoftware.Steam/...`, and
Snap), so nothing needs to be exported by hand on a standard install. It
picks `Proton - Experimental` if installed, otherwise the
highest-versioned `Proton *` it finds. Set `PROTON`,
`STEAM_COMPAT_DATA_PATH`, or `KOPT_STEAM_ROOT` explicitly to override
auto-detection for a non-standard setup (Bottles, a manually placed Proton
build, a second Steam library on another disk that isn't registered in
`libraryfolders.vdf`, etc).

| Symptom | Cause |
|---|---|
| `Missing required tools: ...` from `build-linux.sh` | mingw-w64/cmake/ninja not installed — run the printed command or re-run with `--install` |
| `Missing .NET 10.0+ SDK` from `build-loader.sh` | no `dotnet`, or an SDK older than 10.0 installed — use the `dotnet-install.sh` line above |
| `win-x64 bundle needs native artifacts in .../build-mingw/dist` | ran `build-loader.sh --runtime win-x64` before `build-linux.sh` |
| `build-loader.sh` win-x64 self-test skipped | expected — Windows native assets can't run under Linux's `dotnet` host; self-test that bundle on Windows or under Proton |
| `Could not auto-detect Proton` / compatdata | ARK or Proton isn't installed in any Steam library `launch-proton.sh` can see, or it's in a nonstandard path — set `PROTON`/`STEAM_COMPAT_DATA_PATH`/`KOPT_STEAM_ROOT` |
