#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <bcrypt.h>
#include <shellapi.h>

#include <array>
#include <chrono>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

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
        bool quick{};
        bool elevation_attempted{};
        int timeout_seconds{180};
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
            << L"  kopt_injector.exe [--wait] [--process ShooterGame.exe] [--dll path]\n"
            << L"  kopt_injector.exe --pid 1234 --dll path\n"
            << L"  kopt_injector.exe --pid 1234 --unload\n"
            << L"  kopt_injector.exe --self-test\n";
    }

    std::optional<Options> parse(const int argc, wchar_t** argv)
    {
        Options options;
        const auto directory = executable_directory();
        const auto payload = directory / L"kopt_payload.dll";
        const auto candidate = directory / L"kopt_payload_candidate.dll";
        options.dll = std::filesystem::exists(payload) ? payload : candidate;
        options.quick = argc == 1;
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring argument = argv[i];
            if (argument == L"--help" || argument == L"-h") { usage(); return std::nullopt; }
            if (argument == L"--wait") options.wait = true;
            else if (argument == L"--quick") options.quick = true;
            else if (argument == L"--elevated") options.elevation_attempted = true;
            else if (argument == L"--self-test") options.self_test = true;
            else if (argument == L"--unload") options.unload = true;
            else if ((argument == L"--process" || argument == L"--dll" || argument == L"--pid" || argument == L"--timeout") && i + 1 < argc)
            {
                const std::wstring value = argv[++i];
                if (argument == L"--process") options.process = value;
                else if (argument == L"--dll") options.dll = value;
                else if (argument == L"--pid") options.pid = std::wcstoul(value.c_str(), nullptr, 10);
                else options.timeout_seconds = std::max(1, static_cast<int>(std::wcstol(value.c_str(), nullptr, 10)));
            }
            else
            {
                std::wcerr << L"Unknown or incomplete argument: " << argument << L"\n";
                return std::nullopt;
            }
        }
        if (options.quick)
        {
            options.wait = true;
            options.timeout_seconds = 600;
        }
        return options;
    }

    void quick_message(const std::wstring& message, const UINT icon = MB_ICONINFORMATION)
    {
        MessageBoxW(nullptr, message.c_str(), L"KOPT Quick Injector", MB_OK | icon | MB_SETFOREGROUND);
    }

    bool relaunch_elevated()
    {
        std::wstring executable(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (length == 0) return false;
        executable.resize(length);
        const std::wstring directory = executable_directory().wstring();
        SHELLEXECUTEINFOW request{};
        request.cbSize = sizeof(request);
        request.fMask = SEE_MASK_NOCLOSEPROCESS;
        request.lpVerb = L"runas";
        request.lpFile = executable.c_str();
        request.lpParameters = L"--quick --elevated";
        request.lpDirectory = directory.c_str();
        request.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&request) == FALSE) return false;
        if (request.hProcess != nullptr) CloseHandle(request.hProcess);
        return true;
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
        Handle process(OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
            FALSE, pid));
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
        const HMODULE local_image = LoadLibraryExW(loaded->path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
        if (local_image == nullptr)
        {
            error = L"Could not map the loaded payload for export lookup (error " +
                std::to_wstring(GetLastError()) + L")";
            return false;
        }
        const FARPROC local_request = GetProcAddress(local_image, "KoptRequestUnload");
        if (local_request == nullptr)
        {
            FreeLibrary(local_image);
            error = L"Loaded payload has no KoptRequestUnload export";
            return false;
        }
        const auto request_rva = reinterpret_cast<std::uintptr_t>(local_request) -
            reinterpret_cast<std::uintptr_t>(local_image);
        FreeLibrary(local_image);
        Handle thread(CreateRemoteThread(process.value, nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(loaded->base + request_rva), nullptr, 0, nullptr));
        if (!thread)
        {
            error = L"CreateRemoteThread(unload) failed (error " + std::to_wstring(GetLastError()) + L")";
            return false;
        }
        if (WaitForSingleObject(thread.value, 5000) != WAIT_OBJECT_0)
        {
            error = L"KoptRequestUnload call timed out";
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

int run_injector(const int argc, wchar_t** argv)
{
    const auto parsed = parse(argc, argv);
    if (!parsed) return argc > 1 ? 2 : 0;
    Options options = *parsed;
    options.dll = std::filesystem::absolute(options.dll);
    std::wstring error;
    if (!options.unload && !validate_payload(options.dll, error))
    {
        if (options.quick) quick_message(error + L"\n\nKeep KOPT_Inject.exe and kopt_payload.dll in the same folder.", MB_ICONERROR);
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
        if (options.quick)
        {
            const int action = MessageBoxW(nullptr,
                L"ShooterGame.exe is not running.\n\nStart ARK, enter the main menu, then click Retry.",
                L"KOPT Quick Injector", MB_RETRYCANCEL | MB_ICONINFORMATION | MB_SETFOREGROUND);
            if (action != IDRETRY) return 4;
            continue;
        }
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
            if (options.quick) quick_message(L"Unload failed:\n" + error, MB_ICONERROR);
            std::wcerr << L"[KOPT] Unload failed: " << error << L"\n";
            return 6;
        }
        if (options.quick) quick_message(L"Payload unloaded cleanly.");
        std::wcout << L"[KOPT] Payload unloaded cleanly.\n";
        return 0;
    }
    if (!inject(pid, options.dll, error))
    {
        if (options.quick && error.find(L"already loaded") != std::wstring::npos)
        {
            quick_message(L"KOPT is already injected.\n\nHOME opens the menu. END unloads it.");
            return 0;
        }
        if (options.quick && !options.elevation_attempted &&
            error.find(L"OpenProcess failed (error 5)") != std::wstring::npos)
        {
            if (relaunch_elevated()) return 0;
            quick_message(L"Administrator permission was not granted. Injection was cancelled.", MB_ICONERROR);
            return 5;
        }
        if (options.quick) quick_message(L"Injection failed:\n" + error, MB_ICONERROR);
        std::wcerr << L"[KOPT] Injection failed: " << error << L"\n";
        return 5;
    }
    if (options.quick)
        quick_message(L"Injected successfully.\n\nHOME opens the menu. END unloads it.");
    std::wcout << L"[KOPT] Payload loaded. HOME opens the in-game menu; END unloads it.\n";
    return 0;
}

#if defined(KOPT_QUICK_GUI)
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc{};
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) return 2;
    const int result = run_injector(argc, argv);
    LocalFree(argv);
    return result;
}
#else
int wmain(const int argc, wchar_t** argv)
{
    return run_injector(argc, argv);
}
#endif
