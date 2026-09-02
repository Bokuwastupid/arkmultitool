# Running the inject on a real Windows machine

This covers getting `kopt_payload.dll` + `kopt_injector.exe` onto a Windows
box and actually injecting into a running `ShooterGame.exe`.

The mingw-built binaries below are **run-verified, not just
build-verified**: `scripts/launch-proton.sh` injected this exact
`kopt_injector.exe`/`kopt_payload.dll` pair into a real `ShooterGame.exe`
session under Proton — DXGI hooks installed, in-game menu opened/closed on
hotkey, freecam toggled, and the QUIC share connection completed its
handshake against a local relay and stayed connected (`build-mingw/dist/
kopt_internal.log` has the full session). Proton runs the identical PE64
binaries against the same Win32/DXGI surface bare Windows exposes, so this
is strong evidence the injector and payload themselves work correctly —
what's specifically untested is the bare-Windows manual-attach flow this
document describes (no Proton translation layer in between). Test that
golden path yourself before relying on it.

## 1. Prerequisites

- Windows 10/11 x64.
- ARK: Survival Evolved (Steam), **the exact build the injector pins**:
  `kopt_injector.exe` refuses to attach to anything whose `ShooterGame.exe`
  doesn't hash to
  `9BC401417A776C5244A1B0B3255DC3AF4A9D73E3F5C1BA96228FBE3FB1A43477`
  (see `validate_target_build` in `src/injector.cpp`). A different game
  build means the injector exits with "Unsupported ShooterGame.exe build",
  by design — never silently attaches to an unverified target.
- Run both the game and `kopt_injector.exe` as the same user; elevation
  isn't required for `OpenProcess`/`CreateRemoteThread` against your own
  game process, but if Windows Defender or another AV quarantines the DLL,
  you'll need to allow it (expected for any DLL injection tool — this one
  is dumping a raw `LoadLibraryW` call into a game process, which looks
  exactly like what it is to a signature scanner).

## 2. Getting the binaries

**Use the already-built ones — don't try to rebuild natively on Windows
yet.** Two real options exist, and only one currently works end-to-end:

| Path | Status |
|---|---|
| Copy the mingw-w64 cross-compiled binaries (built on Linux via `scripts/build-linux.sh`, or manually per `docs/LINUX_BUILD.md`) | **Works.** Verified: valid PE64, links clean, includes the QUIC/sharing transport (`Http3Publisher`). |
| `build.ps1` (native MSVC on Windows) | **Currently broken for sharing.** It only compiles `config/overlay/payload/runtime.cpp` and links no QUIC libs at all — it predates `share.cpp`/`share_filter.cpp`/`share_remote.cpp`/`http3_publisher.cpp` being wired into `kopt_payload` (see `CMakeLists.txt`'s full source list). Running it today produces a DLL that won't even build, since `payload.cpp` now `#include`s `kopt/http3_publisher.hpp` unconditionally. Fixing it needs MSVC-format (`.lib`, not mingw's `.dll.a`) import libraries for ngtcp2 + OpenSSL, which nothing in this repo currently vendors or fetches — `scripts/fetch-quic-deps.sh` only produces the mingw cross-compiled ones. Treat this as a known gap, not something to work around by hand.

So: build with mingw (Linux, or MSYS2's mingw64 shell on Windows itself —
untested here but same toolchain, should work), then copy the output.

From the build machine, run `scripts/build-linux.sh` (auto-detects your
distro's package manager for missing tools, see `docs/LINUX_BUILD.md`),
then copy these to one folder on the Windows box:

```
build-mingw/dist/kopt_payload.dll
build-mingw/dist/kopt_injector.exe
```

(`version.dll` and the two selftest `.exe`s aren't needed for a manual
Windows attach — `version.dll` is the Linux/Proton auto-load proxy path
only.)

## 3. Running it

Open a terminal (cmd or PowerShell) in the folder holding both files, with
ARK already running:

```
kopt_injector.exe --wait
```

`--wait` polls for `ShooterGame.exe` instead of failing immediately if it
isn't up yet. Useful flags:

```
--process <name>        default: ShooterGame.exe
--pid <id>               attach to a specific PID instead of searching
--dll <path>              default: kopt_payload.dll next to the injector
--share-token <jwt>       account JWT for team sharing (see below)
--backend <host:port>     override the relay address for this launch only
--unload                  detach a previously injected payload
--self-test               verify the DLL is a valid PE64 payload, no injection
```

On success:

```
[KOPT] Target PID 12345
[KOPT] Payload loaded. HOME opens the in-game menu; END unloads it.
```

- **HOME** opens the overlay menu (rebindable in Bindings tab once open).
- **END** requests a clean unload (same as `--unload` from outside).

## 4. Team sharing: JWT or API key, and pointing at your own backend

Sharing (`Diagnostics` tab → "Share sightings & alerts with team") needs a
Bearer credential to authenticate to the relay, and it comes from **one of
two places, in this order**:

1. `--share-token <jwt>` passed to `kopt_injector.exe` at inject time.
2. If that wasn't given: whatever's typed into the **API key** field on
   the Diagnostics tab, in-game. Generate a key via the account's
   `POST /v1/accounts/me/api-keys` (self-service, see
   `backend/backend_python/src/routers/v1/api_keys.py`) and paste the
   plaintext token shown once at creation. It's kept in memory for that
   session only — never written to `kopt_internal.ini`.

Neither is a real environment variable — a value set in your shell can't
reach the DLL after it's already injected into `ShooterGame.exe`'s process
(see the doc comment on `read_share_token` in `src/payload.cpp` for why).

The relay address itself (where sharing actually connects) resolves the
same way: `--backend host:port` at inject time wins if given, otherwise
`Share.Endpoint` from `kopt_internal.ini` (default `127.0.0.1:8443`).
Pointing at a VPS deployment (`backend/deploy/deploy.sh`):

```
kopt_injector.exe --wait --share-token <jwt-or-blank> --backend your-vps-host:8443
```

or leave `--backend` off and edit `kopt_internal.ini`'s `[Share] Endpoint=`
once instead — either way works, `--backend` just avoids touching the ini.

## 5. Troubleshooting

| Symptom | Cause |
|---|---|
| `Unsupported ShooterGame.exe build; SHA-256=...` | Game build doesn't match the pinned hash — wrong branch (public vs beta), or the game updated and the pin is now stale. |
| `A KOPT payload is already loaded: ... Unload it before reinjecting.` | Already injected this session — `--unload` first, or just use the in-game END key. |
| `OpenProcess failed (error 5)` | Access denied — game running as a different user/elevated, or an AV is blocking the handle. |
| `Remote LoadLibraryW did not return a module handle` | DLL failed to load in-process (missing dependency, AV quarantined it mid-load, wrong architecture). Check Windows Event Viewer → Application log for a matching fault. |
| Menu never opens (HOME does nothing) | DLL loaded but the render hook never fired — confirm the game actually reached the main menu/a rendered frame, not still on the loading screen. |
| `Share: offline` forever with a key/token set | Check `kopt_internal.log` (next to the DLL) for "not starting publisher" — it logs the specific reason (missing token/key, no group id, server address unresolved) once per empty spell rather than spamming every frame. |
| Sharing connects to the wrong relay | `--backend` wasn't passed and `kopt_internal.ini`'s `[Share] Endpoint` still holds the local dev default `127.0.0.1:8443`. |
