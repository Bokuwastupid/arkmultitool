#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <bcrypt.h>

#include <array>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <utility>

namespace
{
    struct Handle
    {
        HANDLE value{INVALID_HANDLE_VALUE};
        ~Handle() { if (value != nullptr && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
        Handle() = default;
        explicit Handle(HANDLE handle) : value(handle) {}
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
        Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
        Handle& operator=(Handle&& other) noexcept
        {
            if (this == &other) return *this;
            if (value != nullptr && value != INVALID_HANDLE_VALUE) CloseHandle(value);
            value = std::exchange(other.value, INVALID_HANDLE_VALUE);
            return *this;
        }
        [[nodiscard]] explicit operator bool() const { return value != nullptr && value != INVALID_HANDLE_VALUE; }
    };

    struct Options
    {
        std::wstring process{L"ShooterGame.exe"};
        std::filesystem::path dll;
        DWORD pid{};
        bool wait{};
        bool self_test{};
        bool unload{};
        int timeout_seconds{180};
        // Empty -- kopt::share stays disconnected until a real account/
        // login flow exists (deliberately out of scope for now, see
        // publish_share_token's doc comment for how this gets to the
        // payload at all).
        std::wstring share_token;
        // Empty means "no override" -- the payload falls back to
        // kopt_internal.ini's Share.Endpoint (itself defaulting to
        // 127.0.0.1:8443, see config.hpp). Lets moving the relay to a VPS
        // be a launch-argument change instead of an ini edit or a rebuild:
        // same shared-memory delivery as share_token, for the same reason
        // (see publish_string_mapping's doc comment) -- a real environment
        // variable can't reach an already-running ShooterGame.exe.
        std::wstring backend_endpoint;
    };

    std::filesystem::path executable_directory()
    {
        std::wstring value(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
        value.resize(length);
        return std::filesystem::path(value).parent_path();
    }

    void usage()
    {
        std::wcout << L"KOPT Win64 / Proton injector\n\n"
            << L"  kopt_injector.exe [--wait] [--process ShooterGame.exe] [--dll path] "
               L"[--share-token <jwt>] [--backend <host:port>]\n"
            << L"  kopt_injector.exe --pid 1234 --dll path [--share-token <jwt>] [--backend <host:port>]\n"
            << L"  kopt_injector.exe --pid 1234 --unload\n"
            << L"  kopt_injector.exe --self-test\n\n"
            << L"  --backend overrides kopt_internal.ini's Share.Endpoint for this session only\n"
            << L"  (not written back to disk) -- point it at a VPS without touching the ini.\n";
    }

    std::optional<Options> parse(const int argc, wchar_t** argv)
    {
        Options options;
        const auto directory = executable_directory();
        const auto candidate = directory / L"kopt_payload_candidate.dll";
        // The loader bundle deliberately ships the validated candidate name. Make the
        // command-line backend useful when launched directly as well, while retaining the
        // legacy payload name for developer build directories.
        options.dll = std::filesystem::exists(candidate) ? candidate : directory / L"kopt_payload.dll";
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring argument = argv[i];
            if (argument == L"--help" || argument == L"-h") { usage(); return std::nullopt; }
            if (argument == L"--wait") options.wait = true;
            else if (argument == L"--self-test") options.self_test = true;
            else if (argument == L"--unload") options.unload = true;
            else if ((argument == L"--process" || argument == L"--dll" || argument == L"--pid" ||
                    argument == L"--timeout" || argument == L"--share-token" ||
                    argument == L"--backend") && i + 1 < argc)
            {
                const std::wstring value = argv[++i];
                if (argument == L"--process") options.process = value;
                else if (argument == L"--dll") options.dll = value;
                else if (argument == L"--pid") options.pid = std::wcstoul(value.c_str(), nullptr, 10);
                else if (argument == L"--share-token") options.share_token = value;
                else if (argument == L"--backend") options.backend_endpoint = value;
                else options.timeout_seconds = std::max(1, static_cast<int>(std::wcstol(value.c_str(), nullptr, 10)));
            }
            else
            {
                std::wcerr << L"Unknown or incomplete argument: " << argument << L"\n";
                return std::nullopt;
            }
        }
        return options;
    }

    DWORD find_process(const std::wstring& name)
    {
        Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (!snapshot) return 0;
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot.value, &entry) == FALSE) return 0;
        do
        {
            if (_wcsicmp(entry.szExeFile, name.c_str()) == 0) return entry.th32ProcessID;
        } while (Process32NextW(snapshot.value, &entry));
        return 0;
    }

    std::uintptr_t remote_module(const DWORD pid, const std::wstring& module_name)
    {
        Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
        if (!snapshot) return 0;
        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Module32FirstW(snapshot.value, &entry) == FALSE) return 0;
        do
        {
            if (_wcsicmp(entry.szModule, module_name.c_str()) == 0)
                return reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
        } while (Module32NextW(snapshot.value, &entry));
        return 0;
    }

    struct LoadedPayload
    {
        std::wstring name;
        std::filesystem::path path;
        std::uintptr_t base{};
    };

    std::optional<LoadedPayload> loaded_payload(const DWORD pid)
    {
        Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
        if (!snapshot) return std::nullopt;
        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Module32FirstW(snapshot.value, &entry) == FALSE) return std::nullopt;
        do
        {
            constexpr wchar_t prefix[] = L"kopt_payload";
            if (_wcsnicmp(entry.szModule, prefix, std::size(prefix) - 1) == 0)
                return LoadedPayload{entry.szModule, entry.szExePath,
                    reinterpret_cast<std::uintptr_t>(entry.modBaseAddr)};
        } while (Module32NextW(snapshot.value, &entry));
        return std::nullopt;
    }

    std::uintptr_t remote_module(HANDLE process, const std::wstring& module_name)
    {
        std::vector<HMODULE> modules(256);
        DWORD needed{};
        if (EnumProcessModulesEx(process, modules.data(),
                static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &needed, LIST_MODULES_ALL) == FALSE)
            return 0;
        if (needed > modules.size() * sizeof(HMODULE))
        {
            modules.resize((needed + sizeof(HMODULE) - 1) / sizeof(HMODULE));
            if (EnumProcessModulesEx(process, modules.data(),
                    static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &needed, LIST_MODULES_ALL) == FALSE)
                return 0;
        }
        const std::size_t count = std::min(modules.size(), static_cast<std::size_t>(needed / sizeof(HMODULE)));
        for (std::size_t i = 0; i < count; ++i)
        {
            wchar_t name[MAX_PATH]{};
            if (GetModuleBaseNameW(process, modules[i], name, static_cast<DWORD>(std::size(name))) != 0 &&
                _wcsicmp(name, module_name.c_str()) == 0)
                return reinterpret_cast<std::uintptr_t>(modules[i]);
        }
        return 0;
    }

    bool enable_debug_privilege()
    {
        HANDLE raw_token{};
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &raw_token) == FALSE) return false;
        Handle token(raw_token);
        TOKEN_PRIVILEGES privileges{};
        privileges.PrivilegeCount = 1;
        if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &privileges.Privileges[0].Luid) == FALSE) return false;
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        return AdjustTokenPrivileges(token.value, FALSE, &privileges, sizeof(privileges), nullptr, nullptr) != FALSE;
    }

    bool validate_payload(const std::filesystem::path& dll, std::wstring& error)
    {
        Handle file(CreateFileW(dll.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file) { error = L"Payload does not exist: " + dll.wstring(); return false; }
        IMAGE_DOS_HEADER dos{};
        DWORD bytes{};
        if (ReadFile(file.value, &dos, sizeof(dos), &bytes, nullptr) == FALSE ||
            bytes != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE)
        {
            error = L"Payload has no valid DOS header";
            return false;
        }
        SetFilePointer(file.value, dos.e_lfanew, nullptr, FILE_BEGIN);
        DWORD signature{};
        IMAGE_FILE_HEADER header{};
        if (ReadFile(file.value, &signature, sizeof(signature), &bytes, nullptr) == FALSE || signature != IMAGE_NT_SIGNATURE ||
            ReadFile(file.value, &header, sizeof(header), &bytes, nullptr) == FALSE || header.Machine != IMAGE_FILE_MACHINE_AMD64)
        {
            error = L"Payload is not a valid PE64 AMD64 DLL";
            return false;
        }
        return true;
    }

    bool target_is_64_bit(HANDLE process)
    {
        using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
        const auto function = reinterpret_cast<IsWow64Process2Fn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
        if (function != nullptr)
        {
            USHORT process_machine{};
            USHORT native_machine{};
            return function(process, &process_machine, &native_machine) != FALSE && process_machine == IMAGE_FILE_MACHINE_UNKNOWN;
        }
        BOOL wow64{};
        return IsWow64Process(process, &wow64) != FALSE && wow64 == FALSE;
    }

    std::optional<std::wstring> sha256_file(const std::filesystem::path& path)
    {
        BCRYPT_ALG_HANDLE algorithm{};
        BCRYPT_HASH_HANDLE hash{};
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return std::nullopt;
        DWORD object_size{}, digest_size{}, returned{};
        if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size),
                sizeof(object_size), &returned, 0) < 0 ||
            BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&digest_size),
                sizeof(digest_size), &returned, 0) < 0)
        {
            BCryptCloseAlgorithmProvider(algorithm, 0);
            return std::nullopt;
        }
        std::vector<UCHAR> object(object_size);
        std::vector<UCHAR> digest(digest_size);
        if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) < 0)
        {
            BCryptCloseAlgorithmProvider(algorithm, 0);
            return std::nullopt;
        }
        Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        bool valid = static_cast<bool>(file);
        std::vector<UCHAR> buffer(1024 * 1024);
        while (valid)
        {
            DWORD bytes{};
            if (ReadFile(file.value, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr) == FALSE)
            {
                valid = false;
                break;
            }
            if (bytes == 0) break;
            if (BCryptHashData(hash, buffer.data(), bytes, 0) < 0)
            {
                valid = false;
                break;
            }
        }
        if (valid) valid = BCryptFinishHash(hash, digest.data(), digest_size, 0) >= 0;
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        if (!valid) return std::nullopt;
        constexpr wchar_t hex[] = L"0123456789ABCDEF";
        std::wstring result;
        result.reserve(digest.size() * 2);
        for (const UCHAR byte : digest)
        {
            result.push_back(hex[byte >> 4]);
            result.push_back(hex[byte & 0x0F]);
        }
        return result;
    }

    bool validate_target_build(HANDLE process, std::wstring& error)
    {
        std::wstring path(32768, L'\0');
        DWORD length = static_cast<DWORD>(path.size());
        if (QueryFullProcessImageNameW(process, 0, path.data(), &length) == FALSE || length == 0)
        {
            error = L"Could not resolve target executable path";
            return false;
        }
        path.resize(length);
        const auto digest = sha256_file(path);
        if (!digest)
        {
            error = L"Could not hash target executable: " + path;
            return false;
        }
        constexpr wchar_t expected[] = L"9BC401417A776C5244A1B0B3255DC3AF4A9D73E3F5C1BA96228FBE3FB1A43477";
        if (*digest != expected)
        {
            error = L"Unsupported ShooterGame.exe build; SHA-256=" + *digest;
            return false;
        }
        return true;
    }

    // Delivers a string into the target process without executing any code
    // there -- a named, PID-scoped, pagefile-backed shared memory section,
    // mirroring g_unload_event's naming scheme in payload.cpp. Deliberately
    // NOT CreateRemoteThread-into-an-exported-setter: that pattern (a raw
    // start address inside the already-loaded, non-builtin kopt_payload.dll)
    // was found to reliably hang under Proton/Wine once a session had
    // cycled through a few inject/unload rounds -- see KoptRequestUnload's
    // own history, now fixed the same way. Shared memory + kernel32's
    // CreateFileMapping/OpenFileMapping needs no code execution in the
    // target at all. Also NOT a real environment variable: this process's
    // own env block (however it were set) could never reach an
    // already-running ShooterGame.exe -- fixed since that process started,
    // long before kopt_injector.exe ever runs.
    //
    // Shared by publish_share_token (the JWT) and publish_backend_endpoint
    // (the relay address override) -- same delivery mechanism, different
    // mapping name so the payload can tell them apart.
    //
    // Caller must keep the returned Handle alive until the payload has had
    // a chance to read it (see kMappingHoldDuration in wmain below) --
    // a pagefile-backed section is destroyed the moment its last handle
    // anywhere closes, and kopt_injector.exe is a short-lived CLI process
    // that would otherwise tear it down before the payload's worker
    // thread even starts.
    Handle publish_string_mapping(const std::wstring& name, const std::wstring& value, std::wstring& error)
    {
        const SIZE_T size = (value.size() + 1) * sizeof(wchar_t);
        Handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, static_cast<DWORD>(size), name.c_str()));
        if (!mapping)
        {
            error = L"CreateFileMappingW(" + name + L") failed (error " + std::to_wstring(GetLastError()) + L")";
            return {};
        }
        void* view = MapViewOfFile(mapping.value, FILE_MAP_WRITE, 0, 0, size);
        if (view == nullptr)
        {
            error = L"MapViewOfFile(" + name + L") failed (error " + std::to_wstring(GetLastError()) + L")";
            return {};
        }
        memcpy(view, value.c_str(), size);
        UnmapViewOfFile(view);
        return mapping;
    }

    Handle publish_share_token(const DWORD pid, const std::wstring& token, std::wstring& error)
    {
        return publish_string_mapping(L"Kopt_ShareToken_" + std::to_wstring(pid), token, error);
    }

    Handle publish_backend_endpoint(const DWORD pid, const std::wstring& endpoint, std::wstring& error)
    {
        return publish_string_mapping(L"Kopt_BackendEndpoint_" + std::to_wstring(pid), endpoint, error);
    }

    bool inject(const DWORD pid, const std::filesystem::path& dll, std::wstring& error)
    {
        Handle process(OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
            PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid));
        if (!process)
        {
            error = L"OpenProcess failed (error " + std::to_wstring(GetLastError()) + L")";
            return false;
        }
        if (!target_is_64_bit(process.value))
        {
            error = L"Target process is not Win64";
            return false;
        }
        if (!validate_target_build(process.value, error)) return false;
        const auto loaded = loaded_payload(pid);
        if (loaded)
        {
            error = L"A KOPT payload is already loaded: " + loaded->name + L". Unload it before reinjecting.";
            return false;
        }

        const HMODULE local_kernel = GetModuleHandleW(L"kernel32.dll");
        const auto local_load_library = reinterpret_cast<std::uintptr_t>(GetProcAddress(local_kernel, "LoadLibraryW"));
        auto remote_kernel = remote_module(pid, L"kernel32.dll");
        if (remote_kernel == 0) remote_kernel = remote_module(process.value, L"kernel32.dll");
        if (local_kernel == nullptr || local_load_library == 0)
        {
            error = L"Could not resolve local kernel32!LoadLibraryW";
            return false;
        }
        const auto load_library_rva = local_load_library - reinterpret_cast<std::uintptr_t>(local_kernel);
        // Windows maps system DLLs at a boot-session-wide base. Wine/Proton also
        // keeps builtin kernel32 addresses consistent inside one prefix/wineserver.
        // Prefer a separately enumerated remote base, but retain the local address
        // when protected process enumeration hides the module list.
        const auto remote_load_library = remote_kernel != 0 ? remote_kernel + load_library_rva : local_load_library;

        const std::wstring absolute = std::filesystem::absolute(dll).wstring();
        const SIZE_T allocation_size = (absolute.size() + 1) * sizeof(wchar_t);
        void* remote_path = VirtualAllocEx(process.value, nullptr, allocation_size,
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (remote_path == nullptr)
        {
            error = L"VirtualAllocEx failed (error " + std::to_wstring(GetLastError()) + L")";
            return false;
        }
        SIZE_T written{};
        if (WriteProcessMemory(process.value, remote_path, absolute.c_str(), allocation_size, &written) == FALSE ||
            written != allocation_size)
        {
            error = L"WriteProcessMemory failed (error " + std::to_wstring(GetLastError()) + L")";
            VirtualFreeEx(process.value, remote_path, 0, MEM_RELEASE);
            return false;
        }
        Handle thread(CreateRemoteThread(process.value, nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_load_library), remote_path, 0, nullptr));
        if (!thread)
        {
            error = L"CreateRemoteThread failed (error " + std::to_wstring(GetLastError()) + L")";
            VirtualFreeEx(process.value, remote_path, 0, MEM_RELEASE);
            return false;
        }
        const DWORD wait = WaitForSingleObject(thread.value, 15000);
        DWORD exit_code{};
        GetExitCodeThread(thread.value, &exit_code);
        VirtualFreeEx(process.value, remote_path, 0, MEM_RELEASE);
        if (wait != WAIT_OBJECT_0 || exit_code == 0)
        {
            error = L"Remote LoadLibraryW did not return a module handle";
            return false;
        }
        return true;
    }

    bool request_unload(const DWORD pid, std::wstring& error)
    {
        Handle process(OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid));
        if (!process)
        {
            error = L"OpenProcess failed (error " + std::to_wstring(GetLastError()) + L")";
            return false;
        }
        if (!target_is_64_bit(process.value))
        {
            error = L"Target process is not Win64";
            return false;
        }
        const auto loaded = loaded_payload(pid);
        if (!loaded)
        {
            error = L"No KOPT payload is loaded";
            return false;
        }
        // Signals a named event the payload waits on (see payload.cpp's
        // worker(), g_unload_event) instead of CreateRemoteThread'ing
        // directly into KoptRequestUnload's address inside the target's
        // copy of kopt_payload.dll. That used to work but was found to
        // reliably hang (5s timeout, every time, on an otherwise perfectly
        // alive/responsive target process) after a session had already
        // gone through a few inject/unload cycles under Proton/Wine --
        // CreateRemoteThread landing on an arbitrary address inside a
        // third-party, non-builtin PE module is a far less exercised Wine
        // code path than landing on a builtin export (kernel32!LoadLibraryW,
        // used by inject() above, which never showed this problem).
        // CreateEventW/SetEvent are exactly as "builtin" as LoadLibraryW.
        const std::wstring event_name = L"Kopt_Unload_" + std::to_wstring(pid);
        Handle event(OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name.c_str()));
        if (!event)
        {
            error = L"OpenEventW failed (error " + std::to_wstring(GetLastError()) + L")";
            return false;
        }
        if (SetEvent(event.value) == FALSE)
        {
            error = L"SetEvent failed (error " + std::to_wstring(GetLastError()) + L")";
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (remote_module(pid, loaded->name) == 0) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        error = L"Payload accepted the unload request but remained mapped";
        return false;
    }
}

int wmain(const int argc, wchar_t** argv)
{
    const auto parsed = parse(argc, argv);
    if (!parsed) return argc > 1 ? 2 : 0;
    Options options = *parsed;
    options.dll = std::filesystem::absolute(options.dll);
    std::wstring error;
    if (!options.unload && !validate_payload(options.dll, error))
    {
        std::wcerr << L"[KOPT] " << error << L"\n";
        return 3;
    }
    if (options.self_test && options.unload)
    {
        std::wcerr << L"[KOPT] --self-test and --unload cannot be combined\n";
        return 2;
    }
    if (options.self_test)
    {
        std::wcout << L"[KOPT] Self-test passed: PE64 payload is valid at " << options.dll << L"\n";
        return 0;
    }
    enable_debug_privilege();

    DWORD pid = options.pid;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(options.timeout_seconds);
    while (pid == 0)
    {
        pid = find_process(options.process);
        if (pid != 0) break;
        if (!options.wait || std::chrono::steady_clock::now() >= deadline)
        {
            std::wcerr << L"[KOPT] Process not found: " << options.process << L"\n";
            return 4;
        }
        std::wcout << L"[KOPT] Waiting for " << options.process << L"...\r" << std::flush;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    std::wcout << L"\n[KOPT] Target PID " << pid << L"\n";
    if (options.unload)
    {
        if (!request_unload(pid, error))
        {
            std::wcerr << L"[KOPT] Unload failed: " << error << L"\n";
            return 6;
        }
        std::wcout << L"[KOPT] Payload unloaded cleanly.\n";
        return 0;
    }
    // Published BEFORE inject() -- LoadLibraryW below starts the payload's
    // worker thread almost immediately, which reads this at startup; the
    // mapping has to already exist by then, not race to catch up after.
    Handle share_token_mapping;
    if (!options.share_token.empty())
    {
        share_token_mapping = publish_share_token(pid, options.share_token, error);
        if (!share_token_mapping)
        {
            std::wcerr << L"[KOPT] Publishing share token failed: " << error << L"\n";
            return 7;
        }
    }
    Handle backend_endpoint_mapping;
    if (!options.backend_endpoint.empty())
    {
        backend_endpoint_mapping = publish_backend_endpoint(pid, options.backend_endpoint, error);
        if (!backend_endpoint_mapping)
        {
            std::wcerr << L"[KOPT] Publishing backend endpoint failed: " << error << L"\n";
            return 8;
        }
    }
    if (!inject(pid, options.dll, error))
    {
        std::wcerr << L"[KOPT] Injection failed: " << error << L"\n";
        return 5;
    }
    if (share_token_mapping || backend_endpoint_mapping)
    {
        // Hold the mapping(s) open past LoadLibraryW returning -- the
        // payload's worker thread still has DXVK/render-ready waits ahead
        // of it (observed 2-5s in practice) before it gets to reading
        // these. A pagefile-backed section dies the instant every handle
        // to it closes; kopt_injector.exe exiting right after inject()
        // succeeds would otherwise race the payload's own startup.
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    std::wcout << L"[KOPT] Payload loaded. HOME opens the in-game menu; END unloads it.\n";
    return 0;
}
