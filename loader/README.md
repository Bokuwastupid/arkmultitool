# KOPT Loader

Cross-platform .NET 10/Avalonia desktop shell for Windows and Linux. The current
vertical slice provides Identity bearer login, entitlement refresh, bounded
device-lease acquisition, privacy-preserving machine fingerprinting, exact ASE
target hashing, diagnostics IDs and the version-pinned Windows LoadLibrary
backend.

Tokens are memory-only. The loader never logs credentials, capabilities, HWID
material or bearer tokens. Set `KOPT_API_URL` to the TLS control-plane endpoint;
plain HTTP is accepted only for a loopback development address.

Build the Windows development bundle after the native candidate:

```powershell
.\scripts\build-loader.ps1 -Runtime win-x64
```

Build the Linux shell:

```powershell
.\scripts\build-loader.ps1 -Runtime linux-x64
```

The Linux shell detects product/account state, but deliberately refuses to run
the Windows injection backend. Proton/Wine path translation and handshake remain
a separate backend, so Linux never silently falls through to an incompatible
Windows operation.

Development overrides:

- `KOPT_API_URL` — control-plane base URL;
- `KOPT_INJECTOR_PATH` — exact injector executable;
- `KOPT_PAYLOAD_PATH` — exact candidate payload.

The local manifest created by the build script is explicitly marked unsigned
and must never be treated as a production update manifest.
