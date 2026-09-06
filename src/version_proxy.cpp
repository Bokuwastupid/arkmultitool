// Auto-load shim for the Linux/Proton deployment path. ShooterGame.exe
// imports VERSION.dll directly (verified against the real binary: it pulls
// VerQueryValueW, GetFileVersionInfoW, GetFileVersionInfoSizeW), and the
// Windows/Wine PE loader always checks the executable's own directory
// before system32. Dropping this DLL there as "version.dll" makes it load
// automatically on every launch -- no CreateRemoteThread, no external
// injector process, no crossing the Steam Linux Runtime sandbox boundary.
//
// version.def forwards every real export to version_orig.dll (the real
// Wine version.dll, copied next to this one by scripts/install-linux-
// proxy.sh) so nothing that actually calls into version.dll behaves any
// differently. This file only adds one thing: load kopt_payload.dll from
// a worker thread on process attach.
//
// Must not touch payload logic from DllMain itself -- LoadLibrary from
// inside DLL_PROCESS_ATTACH while still holding the loader lock is exactly
// the deadlock/reentrancy hazard DisableThreadLibraryCalls + a spawned
// thread exist to avoid; kopt_payload.dll's own DllMain does the same for
// the same reason (see payload.cpp).
//
// version.dll lives in the *prefix's* system32, not the game's own folder,
// so every process in the prefix that imports VERSION.dll loads this --
// confirmed against a real launch: the small in-prefix "steam.exe" helper
// imports it too (via msi.dll), and was loading kopt_payload.dll and
// racing it to probe/hook D3D11 at the same time as the real
// ShooterGame.exe instance, corrupting whichever vtable patch lost the
// race. Payload logic must only ever run inside the actual game process.
#include <windows.h>
#include <wchar.h>

namespace
{
    bool running_inside_shooter_game() noexcept
    {
        wchar_t path[MAX_PATH];
        const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) return false;
        const wchar_t* filename = path;
        for (wchar_t* cursor = path; *cursor != L'\0'; ++cursor)
            if (*cursor == L'\\' || *cursor == L'/') filename = cursor + 1;
        return _wcsicmp(filename, L"ShooterGame.exe") == 0;
    }

    DWORD WINAPI load_payload(LPVOID) noexcept
    {
        if (running_inside_shooter_game())
            LoadLibraryW(L"kopt_payload.dll");
        return 0;
    }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
        const HANDLE thread = CreateThread(nullptr, 0, load_payload, nullptr, 0, nullptr);
        if (thread != nullptr)
            CloseHandle(thread);
    }
    return TRUE;
}
