#include "kopt/config.hpp"
#include "kopt/com_ptr.hpp"
#include "kopt/overlay.hpp"
#include "kopt/publisher.hpp"
#include "kopt/runtime.hpp"
#include "kopt/share.hpp"
#include "kopt/share_filter.hpp"
#include "kopt/share_remote.hpp"
#if KOPT_ENABLE_SHARE
#include "kopt/http3_publisher.hpp"
#endif

#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <dbghelp.h>

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

using kopt::ComPtr;

namespace
{
    using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
    using ResizeBuffersFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    using CameraUpdateFn = void(__fastcall*)(void*, float);
    using DrawIndexedFn = void(__stdcall*)(ID3D11DeviceContext*, UINT, UINT, INT);
    using DrawIndexedInstancedFn = void(__stdcall*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
    // Skinned meshes rendered through the GPU skin cache (the default on the
    // ShooterGame UE4 version this targets) issue their draws through
    // DrawIndexedInstancedIndirect/DrawInstancedIndirect, not the two slots
    // above. Missing these means the hook never sees the first-person mesh's
    // real draw call at all, so bForceWireframe renders untouched and Solid
    // style can never engage even though the hook itself is installed.
    using DrawIndexedInstancedIndirectFn = void(__stdcall*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
    using DrawInstancedIndirectFn = void(__stdcall*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);

    // Read once, at worker() startup, from the named shared-memory section
    // kopt_injector.exe's --share-token publishes (see its
    // publish_share_token doc comment) -- never written to kopt_internal.ini,
    // same "in memory only" policy the loader already applies to its own
    // tokens. NOT an environment variable: GetEnvironmentVariableW reads
    // THIS process's own environment block, fixed since ShooterGame.exe
    // itself was launched, long before an injector ever runs -- setting an
    // env var anywhere in the injector/loader cannot reach an
    // already-running target process by any means. Empty means "not
    // configured" (no --share-token was given at inject time); the
    // tick-loop refuses to start the publisher with an empty token rather
    // than connecting unauthenticated.
    //
    // Must be read close to process start, not lazily on first use: the
    // injector only holds its side of the mapping open for a few seconds
    // after LoadLibraryW returns (see its wmain) -- by the time a user
    // manually flips the Diagnostics-tab sharing checkbox on, that window
    // has long since closed and the mapping is gone.
    //
    // Shared by both g_share_token ("Kopt_ShareToken_<pid>") and the
    // backend endpoint override ("Kopt_BackendEndpoint_<pid>") -- same
    // shared-memory read, different mapping name prefix (see
    // injector.cpp's publish_string_mapping doc comment for why this
    // exists instead of a real environment variable).
    std::wstring read_named_string(const wchar_t* name_prefix)
    {
        const std::wstring name = name_prefix + std::to_wstring(GetCurrentProcessId());
        const HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
        if (mapping == nullptr) return {}; // not published -- the injector wasn't given this value at inject time
        const wchar_t* view = static_cast<const wchar_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
        if (view == nullptr)
        {
            CloseHandle(mapping);
            return {};
        }
        // The mapping is exactly (value.size() + 1) wchar_t's, written as
        // one contiguous null-terminated buffer -- see
        // publish_string_mapping in injector.cpp. wcsnlen bounds the scan
        // to avoid depending on MapViewOfFile's own view size query (a
        // second syscall); a hard cap here is simpler and both a
        // legitimate JWT and a host:port endpoint are always far under it.
        constexpr std::size_t max_length = 8192;
        const std::size_t length = wcsnlen(view, max_length);
        std::wstring value(view, length);
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        return value;
    }

    std::wstring read_share_token() { return read_named_string(L"Kopt_ShareToken_"); }
    std::wstring read_backend_endpoint() { return read_named_string(L"Kopt_BackendEndpoint_"); }

    HMODULE g_module{};
    kopt::Settings g_settings;
    kopt::ArkRuntime g_runtime;
    // NoopPublisher when the build has no QUIC transport (see
    // KOPT_ENABLE_SHARE in CMakeLists.txt). Everything upstream of the
    // publisher -- sighting building, the change filter, the alert queue --
    // still runs and is still testable; the batches simply go nowhere, and
    // connected() staying false keeps the UI honest about it.
#if KOPT_ENABLE_SHARE
    std::unique_ptr<kopt::Publisher> g_publisher = std::make_unique<kopt::Http3Publisher>();
#else
    std::unique_ptr<kopt::Publisher> g_publisher = std::make_unique<kopt::NoopPublisher>();
#endif
    std::chrono::steady_clock::time_point g_last_share_submit{};
    // Whether g_publisher->start() has actually been called (not just
    // whether the user wants share on) -- lets the start attempt keep
    // quietly retrying, tick after tick, while share_enabled is true but
    // server_ip isn't resolved yet (loading screen/main menu), without
    // re-triggering on every single frame once it succeeds.
    bool g_share_started{};
    bool g_share_missing_token_logged{};
    bool g_share_missing_server_ip_logged{};
    // Populated once at worker() startup via read_share_token() -- see its
    // doc comment for why this can't be read lazily on first use.
    std::wstring g_share_token;
    // Populated once at worker() startup via read_backend_endpoint().
    // Empty means "no --backend override was given"; the tick-loop falls
    // back to kopt_internal.ini's Share.Endpoint in that case (see
    // g_settings.share_endpoint's default in config.hpp). Kept out of
    // g_settings so g_settings.save() on unload never persists a
    // per-launch override into the ini.
    std::wstring g_backend_endpoint;
    // Память о СВОИХ последних отправленных состояниях (не спамить
    // неизменным) -- один экземпляр на весь процесс, пересоздание каждый
    // тик обнулило бы память и превратило фильтр в no-op.
    kopt::share::ChangeFilter g_share_filter;
    // Буфер принятых батчей от других репортёров команды, с протуханием.
    // ttl длиннее самого долгого keyframe-интервала источника (30 с) с
    // запасом -- иначе собственный маячок отправителя будет считаться
    // протухшим раньше, чем он успел повториться.
    kopt::share::RemoteView g_remote_view{std::chrono::milliseconds(45000)};
    // subscribe() зовёт колбэк с фонового read-потока Publisher'а, а
    // g_remote_view читается с потока Present-хука -- без мьютекса это была
    // бы гонка на одной и той же unordered_map.
    std::mutex g_remote_view_mutex;
    kopt::Overlay g_overlay;
    kopt::InputState g_input;
    std::filesystem::path g_settings_path;
    std::filesystem::path g_log_path;
    PresentFn g_original_present{};
    ResizeBuffersFn g_original_resize{};
    void** g_present_slot{};
    void** g_resize_slot{};
    void** g_camera_slot{};
    void** g_draw_indexed_slot{};
    void** g_draw_indexed_instanced_slot{};
    void** g_draw_indexed_instanced_indirect_slot{};
    void** g_draw_instanced_indirect_slot{};
    CameraUpdateFn g_original_camera_update{};
    DrawIndexedFn g_original_draw_indexed{};
    DrawIndexedInstancedFn g_original_draw_indexed_instanced{};
    DrawIndexedInstancedIndirectFn g_original_draw_indexed_instanced_indirect{};
    DrawInstancedIndirectFn g_original_draw_instanced_indirect{};
    HWND g_game_window{};
    IDXGISwapChain* g_game_swap_chain{};
    WNDPROC g_original_wndproc{};
    std::atomic<bool> g_stop{};
    HANDLE g_unload_event{};
    std::atomic<unsigned> g_active_callbacks{};
    std::chrono::steady_clock::time_point g_last_frame{};
    std::atomic<bool> g_first_present_logged{};
    std::atomic<bool> g_overlay_init_logged{};
    std::atomic<bool> g_menu_open{};
    std::atomic<std::uint32_t> g_menu_key{VK_HOME};
    std::atomic<std::uint32_t> g_unload_key{VK_END};
    std::atomic<std::uint32_t> g_freecam_key{VK_F6};
    std::atomic<std::uint32_t> g_esp_toggle_key{VK_F7};
    std::atomic<std::uint32_t> g_panic_key{VK_F12};
    std::atomic<bool> g_freecam_active{};
    std::atomic<bool> g_freecam_pose_valid{};
    std::atomic<std::uint32_t> g_freecam_pose_sequence{};
    std::atomic<float> g_freecam_position_x{};
    std::atomic<float> g_freecam_position_y{};
    std::atomic<float> g_freecam_position_z{};
    std::atomic<float> g_freecam_rotation_x{};
    std::atomic<float> g_freecam_rotation_y{};
    std::atomic<float> g_freecam_rotation_z{};
    std::atomic<bool> g_fov_override_active{};
    std::atomic<float> g_custom_fov{112.5F};
    std::atomic<int> g_camera_hook_idle_frames{};
    std::uintptr_t g_locked_fov_manager{};
    float g_locked_fov_value{};
    std::atomic<int> g_local_chams_draw_mode{-1};
    std::atomic<std::uint32_t> g_local_chams_color{0xA33DFFFFU};
    std::atomic<int> g_freecam_wheel{};
    // Values the unload teardown clobbered, so the config written on the way out
    // reflects what the user had enabled rather than the disabled end state.
    struct UnloadRestore
    {
        bool valid{};
        bool freecam{};
        bool local_chams{};
        bool no_recoil{};
        bool no_sway{};
    } g_unload_restore;
    void restore_settings_intent();
    std::atomic<bool> g_unload_cleanup_requested{};
    std::atomic<bool> g_unload_cleanup_completed{};
    bool g_menu_input_active{};
    std::atomic<bool> g_polled_left_down{};
    std::atomic<bool> g_menu_pointer_armed{};
    std::uint64_t g_logged_world_generation{};
    std::uint32_t g_logged_skeleton_guard_hits{};
    bool g_logged_local_valid{};
    bool g_logged_aim_active{};
    bool g_logged_managarmr_safe_aim{};
    bool g_logged_mounted_safe_mode{};
    std::unordered_map<std::uintptr_t, std::wstring> g_logged_horde_previews;
    std::unordered_map<std::uintptr_t, std::wstring> g_logged_explorer_notes;
    // Players seen this world generation, keyed by pawn address. The value is a
    // signature rather than a flag so that a contact re-logs when something
    // about it actually changed -- in practice when steam_id resolves after the
    // first frames, which is the difference between a usable record and one that
    // just says "someone was here".
    std::unordered_map<std::uintptr_t, std::wstring> g_logged_player_contacts;
    int g_cursor_show_adjustment{};
    std::uint64_t g_game_swap_chain_area{};
    float g_pointer_scale_x{1.0F};
    float g_pointer_scale_y{1.0F};
    std::mutex g_state_mutex;
    ComPtr<ID3D11Device> g_chams_device;
    ComPtr<ID3D11PixelShader> g_chams_pixel_shader;
    ComPtr<ID3D11Buffer> g_chams_color_buffer;
    ComPtr<ID3D11BlendState> g_chams_blend_state;
    ComPtr<ID3D11RasterizerState> g_chams_solid_rasterizer;
    D3D11_RASTERIZER_DESC g_chams_rasterizer_description{};
    bool g_chams_rasterizer_valid{};
    std::uint32_t g_uploaded_chams_color{};
    PVOID g_exception_guard_handle{};
    PVOID g_fatal_exception_logger_handle{};
    LPTOP_LEVEL_EXCEPTION_FILTER g_previous_exception_filter{};
    std::uintptr_t g_game_module_base{};
    alignas(8) std::uint64_t g_none_fname{};
    std::atomic<std::uint32_t> g_skeleton_guard_hits{};
    std::atomic<std::uint64_t> g_camera_tick_lock_skips{};
    std::atomic<std::uint32_t> g_diagnostic_feature_flags{};
    struct PanicState
    {
        bool active{};
        bool menu{};
        bool esp{};
        bool freecam{};
        bool local_chams{};
        bool no_recoil{};
        bool no_sway{};
    } g_panic_state;

    void publish_freecam_pose(const kopt::Vec3& position, const kopt::Vec3& rotation) noexcept
    {
        if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z) ||
            !std::isfinite(rotation.x) || !std::isfinite(rotation.y) || !std::isfinite(rotation.z)) return;
        g_freecam_pose_sequence.fetch_add(1, std::memory_order_acq_rel);
        g_freecam_position_x.store(position.x, std::memory_order_relaxed);
        g_freecam_position_y.store(position.y, std::memory_order_relaxed);
        g_freecam_position_z.store(position.z, std::memory_order_relaxed);
        g_freecam_rotation_x.store(rotation.x, std::memory_order_relaxed);
        g_freecam_rotation_y.store(rotation.y, std::memory_order_relaxed);
        g_freecam_rotation_z.store(rotation.z, std::memory_order_relaxed);
        g_freecam_pose_sequence.fetch_add(1, std::memory_order_release);
        g_freecam_pose_valid.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool load_freecam_pose(kopt::Vec3& position, kopt::Vec3& rotation) noexcept
    {
        if (!g_freecam_pose_valid.load(std::memory_order_acquire)) return false;
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            const std::uint32_t before = g_freecam_pose_sequence.load(std::memory_order_acquire);
            if ((before & 1U) != 0U) continue;
            position = {g_freecam_position_x.load(std::memory_order_relaxed),
                g_freecam_position_y.load(std::memory_order_relaxed),
                g_freecam_position_z.load(std::memory_order_relaxed)};
            rotation = {g_freecam_rotation_x.load(std::memory_order_relaxed),
                g_freecam_rotation_y.load(std::memory_order_relaxed),
                g_freecam_rotation_z.load(std::memory_order_relaxed)};
            const std::uint32_t after = g_freecam_pose_sequence.load(std::memory_order_acquire);
            if (before == after && (after & 1U) == 0U) return true;
        }
        return false;
    }

    void hold_freecam_pose(void* manager) noexcept
    {
        if (manager == nullptr || !g_freecam_active.load(std::memory_order_acquire)) return;
        constexpr std::uintptr_t camera_pov_offset = 0x4D0 + 0x8;
        auto* const pov = reinterpret_cast<std::uint8_t*>(manager) + camera_pov_offset;
        kopt::Vec3 position{};
        kopt::Vec3 rotation{};
        if (!load_freecam_pose(position, rotation))
        {
            SIZE_T position_read{};
            SIZE_T rotation_read{};
            if (ReadProcessMemory(GetCurrentProcess(), pov, &position, sizeof(position), &position_read) == FALSE ||
                ReadProcessMemory(GetCurrentProcess(), pov + 0xC, &rotation, sizeof(rotation), &rotation_read) == FALSE ||
                position_read != sizeof(position) || rotation_read != sizeof(rotation)) return;
            publish_freecam_pose(position, rotation);
        }
        SIZE_T written{};
        WriteProcessMemory(GetCurrentProcess(), pov, &position, sizeof(position), &written);
        WriteProcessMemory(GetCurrentProcess(), pov + 0xC, &rotation, sizeof(rotation), &written);
    }

    void log_line(const std::wstring& message)
    {
        if (g_log_path.empty()) return;
        const HANDLE file = CreateFileW(g_log_path.c_str(), FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        SYSTEMTIME time{};
        GetLocalTime(&time);
        const std::wstring line = std::format(L"{:02}:{:02}:{:02}.{:03} {}\r\n",
            time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, message);
        DWORD written{};
        WriteFile(file, line.data(), static_cast<DWORD>(line.size() * sizeof(wchar_t)), &written, nullptr);
        CloseHandle(file);
    }

    std::filesystem::path write_diagnostics_bundle(EXCEPTION_POINTERS* exception, const bool crash)
    {
        if (g_log_path.empty()) return {};
        SYSTEMTIME time{};
        GetLocalTime(&time);
        const std::wstring stamp = std::format(L"{:04}{:02}{:02}-{:02}{:02}{:02}-{:03}",
            time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
        const std::filesystem::path directory = g_log_path.parent_path() / L"diagnostics" /
            ((crash ? std::wstring(L"crash-") : std::wstring(L"manual-")) + stamp);
        std::error_code directory_error;
        std::filesystem::create_directories(directory, directory_error);
        if (directory_error) return {};

        const std::filesystem::path dump_path = directory / L"ShooterGame.dmp";
        const HANDLE dump = CreateFileW(dump_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (dump != INVALID_HANDLE_VALUE)
        {
            MINIDUMP_EXCEPTION_INFORMATION exception_information{};
            exception_information.ThreadId = GetCurrentThreadId();
            exception_information.ExceptionPointers = exception;
            exception_information.ClientPointers = FALSE;
            const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
                MiniDumpWithThreadInfo | MiniDumpWithProcessThreadData | MiniDumpWithUnloadedModules);
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump, type,
                exception != nullptr ? &exception_information : nullptr, nullptr, nullptr);
            CloseHandle(dump);
        }

        const std::filesystem::path context_path = directory / L"context.txt";
        const HANDLE context = CreateFileW(context_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (context != INVALID_HANDLE_VALUE)
        {
            const auto feature_flags = g_diagnostic_feature_flags.load(std::memory_order_relaxed);
            const auto exception_code = exception != nullptr && exception->ExceptionRecord != nullptr ?
                exception->ExceptionRecord->ExceptionCode : 0UL;
            const auto exception_address = exception != nullptr && exception->ExceptionRecord != nullptr ?
                reinterpret_cast<std::uintptr_t>(exception->ExceptionRecord->ExceptionAddress) : 0ULL;
            const auto exception_operation = exception != nullptr && exception->ExceptionRecord != nullptr &&
                exception->ExceptionRecord->NumberParameters > 0 ?
                exception->ExceptionRecord->ExceptionInformation[0] : 0ULL;
            const auto exception_target = exception != nullptr && exception->ExceptionRecord != nullptr &&
                exception->ExceptionRecord->NumberParameters > 1 ?
                exception->ExceptionRecord->ExceptionInformation[1] : 0ULL;
            const std::wstring payload = std::format(
                L"KOPT diagnostics\r\nKind={}\r\nPID={}\r\nWorldGeneration={}\r\n"
                L"LocalValid={}\r\nAimActive={}\r\nMenuOpen={}\r\nSkeletonGuardHits={}\r\n"
                L"ExceptionCode=0x{:08X}\r\nExceptionAddress=0x{:016X}\r\n"
                L"ExceptionOperation={}\r\nExceptionTarget=0x{:016X}\r\n"
                L"Esp={}\r\nPlayerAim={}\r\nDinoAim={}\r\nFreecam={}\r\n"
                L"LocalChams={}\r\nNoRecoil={}\r\nNoSway={}\r\n",
                crash ? L"crash" : L"manual", GetCurrentProcessId(), g_logged_world_generation,
                g_logged_local_valid, g_logged_aim_active, g_menu_open.load(std::memory_order_relaxed),
                g_skeleton_guard_hits.load(std::memory_order_relaxed), exception_code, exception_address,
                exception_operation, exception_target, (feature_flags & (1U << 0)) != 0,
                (feature_flags & (1U << 1)) != 0, (feature_flags & (1U << 2)) != 0,
                (feature_flags & (1U << 3)) != 0, (feature_flags & (1U << 4)) != 0,
                (feature_flags & (1U << 5)) != 0, (feature_flags & (1U << 6)) != 0);
            const wchar_t bom = 0xFEFF;
            DWORD written{};
            WriteFile(context, &bom, sizeof(bom), &written, nullptr);
            WriteFile(context, payload.data(), static_cast<DWORD>(payload.size() * sizeof(wchar_t)), &written, nullptr);
            CloseHandle(context);
        }
        CopyFileW(g_log_path.c_str(), (directory / L"kopt_internal.log").c_str(), FALSE);
        return directory;
    }

    LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS* exception)
    {
        write_diagnostics_bundle(exception, true);
        if (g_previous_exception_filter != nullptr &&
            g_previous_exception_filter != &unhandled_exception_filter)
            return g_previous_exception_filter(exception);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // fatal_exception_logger is a diagnostic tripwire, not a handler: it
    // NEVER swallows anything (always returns EXCEPTION_CONTINUE_SEARCH),
    // it only appends one log_line before whatever normally happens next
    // happens anyway. Why this exists as a SEPARATE mechanism from
    // unhandled_exception_filter below: a real crash on 2026-09-02 left no
    // diagnostics/crash-* bundle at all -- kopt_internal.log just stops
    // mid-session with no exception ever reaching that top-level filter.
    // SetUnhandledExceptionFilter is a LAST-RESORT callback: Windows (and
    // Wine's translation of a SIGSEGV into an SEH exception) only invokes
    // it if nothing earlier in the frame-based __try/__except chain
    // handled the exception AND the process isn't torn down through some
    // other path (TerminateProcess, __fastfail, a raw signal Wine's
    // page-fault translator failed to convert at all). A vectored handler
    // registered here runs BEFORE any frame-based handler gets a chance to
    // swallow or misreport the exception, so if the same crash happens
    // again, comparing this line's timestamp/code against whether a
    // diagnostics/crash-* bundle showed up afterward tells us which of
    // those two things actually happened -- currently indistinguishable
    // from kopt_internal.log alone.
    std::mutex g_fatal_logger_mutex;
    std::unordered_set<std::uintptr_t> g_logged_fatal_addresses;
    // Caps memory AND (far more importantly) bounds how many times this can
    // ever call log_line's synchronous open+write+close -- see the note
    // below on why an unbounded version of this handler is a real, once-
    // observed-in-practice perf bug, not a hypothetical.
    constexpr std::size_t kMaxLoggedFatalAddresses = 256;

    LONG CALLBACK fatal_exception_logger(EXCEPTION_POINTERS* exception)
    {
        if (exception == nullptr || exception->ExceptionRecord == nullptr)
            return EXCEPTION_CONTINUE_SEARCH;
        const DWORD code = exception->ExceptionRecord->ExceptionCode;
        // Only the codes that mean "this thread is not going to recover
        // normally" -- C++ throw/catch, .NET-style, and Wine's own
        // internal bookkeeping exceptions (e.g. thread naming, debugger
        // probes) raise plenty of harmless first-chance exceptions that
        // would make this log unreadable if logged unconditionally.
        switch (code)
        {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_DATATYPE_MISALIGNMENT:
            break;
        default:
            return EXCEPTION_CONTINUE_SEARCH;
        }
        const auto address = reinterpret_cast<std::uintptr_t>(exception->ExceptionRecord->ExceptionAddress);
        // Log each distinct fault ADDRESS once, not every occurrence.
        // Discovered the hard way: some other, unrelated first-chance
        // ACCESS_VIOLATION-and-recover pattern elsewhere in ShooterGame/its
        // D3D driver fires routinely (not the one specific, already-handled
        // flight-death montage bug -- game_exception_guard resolves that
        // one via EXCEPTION_CONTINUE_EXECUTION before dispatch even reaches
        // this handler, since it's registered later and VEH's "first" list
        // is LIFO). Logging unconditionally meant a synchronous
        // open+append+close file write on every single occurrence -- if
        // that fires many times per frame, THIS diagnostic tool was the
        // periodic multi-second stutter, not the crash it exists to catch.
        {
            const std::lock_guard<std::mutex> lock(g_fatal_logger_mutex);
            if (g_logged_fatal_addresses.size() >= kMaxLoggedFatalAddresses ||
                !g_logged_fatal_addresses.insert(address).second)
                return EXCEPTION_CONTINUE_SEARCH;
        }
        const auto offset = g_game_module_base != 0 && address >= g_game_module_base ?
            address - g_game_module_base : 0;
        log_line(std::format(
            L"FATAL SEH FIRST-CHANCE code=0x{:08X} address=0x{:016X} game_module_offset=0x{:X} "
            L"(if the process dies without a diagnostics/crash-* folder appearing after this line, "
            L"the crash bypassed SetUnhandledExceptionFilter entirely -- see PROTON_LOG launch option)",
            code, address, offset));
        return EXCEPTION_CONTINUE_SEARCH;
    }

    LONG CALLBACK game_exception_guard(EXCEPTION_POINTERS* exception)
    {
#if defined(_M_X64)
        if (exception == nullptr || exception->ExceptionRecord == nullptr ||
            exception->ContextRecord == nullptr || g_game_module_base == 0)
            return EXCEPTION_CONTINUE_SEARCH;
        constexpr std::uintptr_t get_slot_group_name_fault_rva = 0x2D59935;
        const EXCEPTION_RECORD& record = *exception->ExceptionRecord;
        if (record.ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record.NumberParameters < 2 ||
            reinterpret_cast<std::uintptr_t>(record.ExceptionAddress) !=
                g_game_module_base + get_slot_group_name_fault_rva ||
            record.ExceptionInformation[0] != 0 ||
            record.ExceptionInformation[1] != static_cast<ULONG_PTR>(-1))
            return EXCEPTION_CONTINUE_SEARCH;

        // ShooterGame 358.26 can pass FName* == -1 from a flying-dino death montage
        // into USkeleton::GetSlotGroupName. Substitute NAME_None for only this exact,
        // verified fault and let HasValidSlotSetup reject the broken montage normally.
        exception->ContextRecord->R8 = reinterpret_cast<DWORD64>(&g_none_fname);
        g_skeleton_guard_hits.fetch_add(1, std::memory_order_relaxed);
        return EXCEPTION_CONTINUE_EXECUTION;
#else
        (void)exception;
        return EXCEPTION_CONTINUE_SEARCH;
#endif
    }

    bool install_game_exception_guard()
    {
        g_game_module_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (g_game_module_base == 0) return false;
        constexpr std::uintptr_t get_slot_group_name_fault_rva = 0x2D59935;
        static constexpr std::array<std::uint8_t, 6> expected{
            0x4D, 0x8B, 0x00, 0x48, 0x8B, 0xD9};
        if (std::memcmp(reinterpret_cast<const void*>(
                g_game_module_base + get_slot_group_name_fault_rva), expected.data(), expected.size()) != 0)
        {
            log_line(L"Flight-death skeleton guard disabled: ShooterGame symbol mismatch");
            g_game_module_base = 0;
            return false;
        }
        g_exception_guard_handle = AddVectoredExceptionHandler(1, game_exception_guard);
        if (g_exception_guard_handle == nullptr)
        {
            log_line(L"Flight-death skeleton guard registration failed");
            g_game_module_base = 0;
            return false;
        }
        log_line(L"Flight-death skeleton guard installed for ShooterGame 358.26");
        return true;
    }

    void remove_game_exception_guard()
    {
        if (g_exception_guard_handle != nullptr)
            RemoveVectoredExceptionHandler(g_exception_guard_handle);
        g_exception_guard_handle = nullptr;
        g_game_module_base = 0;
    }

    bool patch_slot(void** slot, void* replacement, void** previous)
    {
        if (slot == nullptr || replacement == nullptr)
        {
            log_line(L"patch_slot rejected a null pointer");
            return false;
        }
        DWORD old_protection{};
        if (VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old_protection) == FALSE)
        {
            log_line(std::format(L"VirtualProtect(vtable) failed: {}", GetLastError()));
            return false;
        }
        if (previous != nullptr) *previous = *slot;
        InterlockedExchangePointer(slot, replacement);
        DWORD ignored{};
        VirtualProtect(slot, sizeof(void*), old_protection, &ignored);
        FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
        return true;
    }

    void restore_slot(void** slot, void* original)
    {
        if (slot == nullptr || original == nullptr) return;
        DWORD old_protection{};
        if (VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old_protection) == FALSE) return;
        InterlockedExchangePointer(slot, original);
        DWORD ignored{};
        VirtualProtect(slot, sizeof(void*), old_protection, &ignored);
    }

    bool ensure_solid_chams_pipeline(ID3D11DeviceContext* context, const D3D11_RASTERIZER_DESC& source)
    {
        // Every branch below used to fail silently, so a broken solid-chams
        // pipeline was indistinguishable from "not hooked at all": the caller
        // just falls back to draw() with whatever wireframe state the engine
        // already bound. Each failure now logs once so the real cause shows
        // up in the log instead of just "Solid looks like Wireframe".
        static std::atomic<bool> logged_no_context{};
        static std::atomic<bool> logged_no_device{};
        static std::atomic<bool> logged_compile_failure{};
        static std::atomic<bool> logged_pixel_shader_failure{};
        static std::atomic<bool> logged_buffer_failure{};
        static std::atomic<bool> logged_blend_failure{};
        static std::atomic<bool> logged_rasterizer_failure{};
        if (context == nullptr)
        {
            if (!logged_no_context.exchange(true)) log_line(L"Solid chams: DrawIndexed context is null");
            return false;
        }
        ID3D11Device* raw_device{};
        context->GetDevice(&raw_device);
        ComPtr<ID3D11Device> device(raw_device);
        if (device == nullptr)
        {
            if (!logged_no_device.exchange(true)) log_line(L"Solid chams: ID3D11DeviceContext::GetDevice returned null");
            return false;
        }
        if (g_chams_device.get() != device.get())
        {
            g_chams_solid_rasterizer.reset();
            g_chams_color_buffer.reset();
            g_chams_pixel_shader.reset();
            g_chams_blend_state.reset();
            g_chams_device = device;
            g_chams_rasterizer_valid = false;
            g_uploaded_chams_color = 0;
        }
        if (g_chams_pixel_shader == nullptr)
        {
            static constexpr char source_code[] =
                "cbuffer ChamsTint:register(b13){float4 tint;}"
                "float4 main():SV_TARGET{return tint;}";
            ComPtr<ID3DBlob> shader;
            ComPtr<ID3DBlob> errors;
            const HRESULT compile_result = D3DCompile(source_code, sizeof(source_code) - 1, nullptr, nullptr,
                nullptr, "main", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, shader.put(), errors.put());
            if (FAILED(compile_result))
            {
                if (!logged_compile_failure.exchange(true))
                {
                    std::string narrow_error(errors != nullptr
                        ? static_cast<const char*>(errors->GetBufferPointer()) : "no D3DBlob error text");
                    log_line(std::format(L"Solid chams: D3DCompile failed hr=0x{:08X} ({})",
                        static_cast<std::uint32_t>(compile_result),
                        std::wstring(narrow_error.begin(), narrow_error.end())));
                }
                return false;
            }
            const HRESULT shader_result = device->CreatePixelShader(shader->GetBufferPointer(),
                shader->GetBufferSize(), nullptr, g_chams_pixel_shader.put());
            if (FAILED(shader_result))
            {
                if (!logged_pixel_shader_failure.exchange(true))
                    log_line(std::format(L"Solid chams: CreatePixelShader failed hr=0x{:08X}",
                        static_cast<std::uint32_t>(shader_result)));
                return false;
            }
            D3D11_BUFFER_DESC buffer{};
            buffer.ByteWidth = 16;
            buffer.Usage = D3D11_USAGE_DEFAULT;
            buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            const HRESULT buffer_result = device->CreateBuffer(&buffer, nullptr, g_chams_color_buffer.put());
            if (FAILED(buffer_result))
            {
                if (!logged_buffer_failure.exchange(true))
                    log_line(std::format(L"Solid chams: CreateBuffer(tint cbuffer) failed hr=0x{:08X}",
                        static_cast<std::uint32_t>(buffer_result)));
                return false;
            }
            D3D11_BLEND_DESC blend{};
            blend.RenderTarget[0].BlendEnable = TRUE;
            blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
            blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
            blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            const HRESULT blend_result = device->CreateBlendState(&blend, g_chams_blend_state.put());
            if (FAILED(blend_result))
            {
                if (!logged_blend_failure.exchange(true))
                    log_line(std::format(L"Solid chams: CreateBlendState failed hr=0x{:08X}",
                        static_cast<std::uint32_t>(blend_result)));
                return false;
            }
            log_line(L"Solid chams: pixel shader/cbuffer/blend state created");
        }
        D3D11_RASTERIZER_DESC solid = source;
        solid.FillMode = D3D11_FILL_SOLID;
        if (!g_chams_rasterizer_valid ||
            std::memcmp(&solid, &g_chams_rasterizer_description, sizeof(solid)) != 0)
        {
            g_chams_solid_rasterizer.reset();
            const HRESULT rasterizer_result = device->CreateRasterizerState(&solid, g_chams_solid_rasterizer.put());
            if (FAILED(rasterizer_result))
            {
                if (!logged_rasterizer_failure.exchange(true))
                    log_line(std::format(L"Solid chams: CreateRasterizerState failed hr=0x{:08X} "
                        L"(source CullMode={} DepthClipEnable={} MultisampleEnable={})",
                        static_cast<std::uint32_t>(rasterizer_result),
                        static_cast<int>(source.CullMode), source.DepthClipEnable != 0,
                        source.MultisampleEnable != 0));
                return false;
            }
            g_chams_rasterizer_description = solid;
            g_chams_rasterizer_valid = true;
        }
        return g_chams_solid_rasterizer != nullptr && g_chams_color_buffer != nullptr &&
            g_chams_blend_state != nullptr;
    }

    template <typename Draw>
    void draw_with_local_chams(ID3D11DeviceContext* context, Draw&& draw)
    {
        g_active_callbacks.fetch_add(1, std::memory_order_acq_rel);
        const auto finish = [&]() { g_active_callbacks.fetch_sub(1, std::memory_order_acq_rel); };
        const int mode = g_local_chams_draw_mode.load(std::memory_order_acquire);
        if (g_stop.load(std::memory_order_acquire) || mode < 0 || mode == 1 || context == nullptr)
        {
            draw();
            finish();
            return;
        }
        ID3D11RasterizerState* raw_rasterizer{};
        context->RSGetState(&raw_rasterizer);
        ComPtr<ID3D11RasterizerState> rasterizer(raw_rasterizer);
        D3D11_RASTERIZER_DESC rasterizer_description{};
        if (rasterizer == nullptr || (rasterizer->GetDesc(&rasterizer_description),
            rasterizer_description.FillMode != D3D11_FILL_WIREFRAME))
        {
            draw();
            finish();
            return;
        }
        ID3D11RenderTargetView* raw_render_target{};
        context->OMGetRenderTargets(1, &raw_render_target, nullptr);
        ComPtr<ID3D11RenderTargetView> render_target(raw_render_target);
        if (render_target == nullptr)
        {
            static std::atomic<bool> logged_no_render_target{};
            if (!logged_no_render_target.exchange(true))
                log_line(L"Solid chams: forced-wireframe DrawIndexed had no bound render target, "
                    L"falling back to the engine's own wireframe draw");
            draw();
            finish();
            return;
        }
        if (!ensure_solid_chams_pipeline(context, rasterizer_description))
        {
            draw();
            finish();
            return;
        }
        static std::atomic<bool> logged_engaged{};
        if (!logged_engaged.exchange(true))
            log_line(L"Solid chams: pipeline engaged, overriding a forced-wireframe draw with the tint shader");
        const std::uint32_t packed = g_local_chams_color.load(std::memory_order_acquire);
        if (packed != g_uploaded_chams_color)
        {
            const float tint[4]{
                static_cast<float>((packed >> 24) & 0xFFU) / 255.0F,
                static_cast<float>((packed >> 16) & 0xFFU) / 255.0F,
                static_cast<float>((packed >> 8) & 0xFFU) / 255.0F,
                static_cast<float>(packed & 0xFFU) / 255.0F};
            context->UpdateSubresource(g_chams_color_buffer.get(), 0, nullptr, tint, 0, 0);
            g_uploaded_chams_color = packed;
        }
        ID3D11PixelShader* raw_pixel_shader{};
        context->PSGetShader(&raw_pixel_shader, nullptr, nullptr);
        ComPtr<ID3D11PixelShader> pixel_shader(raw_pixel_shader);
        ID3D11Buffer* raw_constant_buffer{};
        context->PSGetConstantBuffers(13, 1, &raw_constant_buffer);
        ComPtr<ID3D11Buffer> constant_buffer(raw_constant_buffer);
        ID3D11BlendState* raw_blend_state{};
        float blend_factor[4]{};
        UINT sample_mask{};
        context->OMGetBlendState(&raw_blend_state, blend_factor, &sample_mask);
        ComPtr<ID3D11BlendState> blend_state(raw_blend_state);

        context->RSSetState(g_chams_solid_rasterizer.get());
        const float chams_blend_factor[4]{};
        context->OMSetBlendState(g_chams_blend_state.get(), chams_blend_factor, 0xFFFFFFFFU);
        context->PSSetShader(g_chams_pixel_shader.get(), nullptr, 0);
        ID3D11Buffer* tint_buffer = g_chams_color_buffer.get();
        context->PSSetConstantBuffers(13, 1, &tint_buffer);
        draw();

        context->RSSetState(rasterizer.get());
        context->OMSetBlendState(blend_state.get(), blend_factor, sample_mask);
        context->PSSetShader(pixel_shader.get(), nullptr, 0);
        ID3D11Buffer* restore_buffer = constant_buffer.get();
        context->PSSetConstantBuffers(13, 1, &restore_buffer);
        if (mode == 2)
            draw();
        finish();
    }

    void __stdcall hooked_draw_indexed(ID3D11DeviceContext* context, const UINT index_count,
        const UINT start_index, const INT base_vertex)
    {
        draw_with_local_chams(context, [&]() {
            g_original_draw_indexed(context, index_count, start_index, base_vertex);
        });
    }

    void __stdcall hooked_draw_indexed_instanced(ID3D11DeviceContext* context,
        const UINT indices_per_instance, const UINT instance_count, const UINT start_index,
        const INT base_vertex, const UINT start_instance)
    {
        draw_with_local_chams(context, [&]() {
            g_original_draw_indexed_instanced(context, indices_per_instance, instance_count,
                start_index, base_vertex, start_instance);
        });
    }

    void __stdcall hooked_draw_indexed_instanced_indirect(ID3D11DeviceContext* context,
        ID3D11Buffer* buffer_for_args, const UINT aligned_byte_offset_for_args)
    {
        draw_with_local_chams(context, [&]() {
            g_original_draw_indexed_instanced_indirect(context, buffer_for_args, aligned_byte_offset_for_args);
        });
    }

    void __stdcall hooked_draw_instanced_indirect(ID3D11DeviceContext* context,
        ID3D11Buffer* buffer_for_args, const UINT aligned_byte_offset_for_args)
    {
        draw_with_local_chams(context, [&]() {
            g_original_draw_instanced_indirect(context, buffer_for_args, aligned_byte_offset_for_args);
        });
    }

    void ensure_draw_indexed_hook(IDXGISwapChain* swap_chain)
    {
        if ((g_draw_indexed_slot != nullptr && g_draw_indexed_instanced_slot != nullptr &&
            g_draw_indexed_instanced_indirect_slot != nullptr && g_draw_instanced_indirect_slot != nullptr) ||
            swap_chain == nullptr) return;
        ComPtr<ID3D11Device> device;
        if (FAILED(swap_chain->GetDevice(__uuidof(ID3D11Device),
            reinterpret_cast<void**>(device.put()))) || device == nullptr) return;
        ComPtr<ID3D11DeviceContext> context;
        device->GetImmediateContext(context.put());
        if (context == nullptr) return;
        auto** vtable = *reinterpret_cast<void***>(context.get());
        if (vtable == nullptr) return;
        if (g_draw_indexed_slot == nullptr)
        {
            void** slot = &vtable[12];
            if (patch_slot(slot, reinterpret_cast<void*>(&hooked_draw_indexed),
                reinterpret_cast<void**>(&g_original_draw_indexed)))
            {
                g_draw_indexed_slot = slot;
                log_line(L"D3D11 DrawIndexed chams hook installed");
            }
        }
        if (g_draw_indexed_instanced_slot == nullptr)
        {
            void** slot = &vtable[20];
            if (patch_slot(slot, reinterpret_cast<void*>(&hooked_draw_indexed_instanced),
                reinterpret_cast<void**>(&g_original_draw_indexed_instanced)))
            {
                g_draw_indexed_instanced_slot = slot;
                log_line(L"D3D11 DrawIndexedInstanced chams hook installed");
            }
        }
        // Slots 39/40: DrawIndexedInstancedIndirect / DrawInstancedIndirect. This is
        // the GPU-skin-cache draw path; ARK's character meshes go through here, not
        // through the two slots above.
        if (g_draw_indexed_instanced_indirect_slot == nullptr)
        {
            void** slot = &vtable[39];
            if (patch_slot(slot, reinterpret_cast<void*>(&hooked_draw_indexed_instanced_indirect),
                reinterpret_cast<void**>(&g_original_draw_indexed_instanced_indirect)))
            {
                g_draw_indexed_instanced_indirect_slot = slot;
                log_line(L"D3D11 DrawIndexedInstancedIndirect chams hook installed");
            }
        }
        if (g_draw_instanced_indirect_slot == nullptr)
        {
            void** slot = &vtable[40];
            if (patch_slot(slot, reinterpret_cast<void*>(&hooked_draw_instanced_indirect),
                reinterpret_cast<void**>(&g_original_draw_instanced_indirect)))
            {
                g_draw_instanced_indirect_slot = slot;
                log_line(L"D3D11 DrawInstancedIndirect chams hook installed");
            }
        }
    }

    void sync_draw_indexed_hooks(IDXGISwapChain* swap_chain, const bool required)
    {
        if (required)
        {
            ensure_draw_indexed_hook(swap_chain);
            return;
        }
        if (g_draw_indexed_slot == nullptr && g_draw_indexed_instanced_slot == nullptr &&
            g_draw_indexed_instanced_indirect_slot == nullptr && g_draw_instanced_indirect_slot == nullptr) return;
        g_local_chams_draw_mode.store(-1, std::memory_order_release);
        restore_slot(g_draw_indexed_slot, reinterpret_cast<void*>(g_original_draw_indexed));
        restore_slot(g_draw_indexed_instanced_slot,
            reinterpret_cast<void*>(g_original_draw_indexed_instanced));
        restore_slot(g_draw_indexed_instanced_indirect_slot,
            reinterpret_cast<void*>(g_original_draw_indexed_instanced_indirect));
        restore_slot(g_draw_instanced_indirect_slot,
            reinterpret_cast<void*>(g_original_draw_instanced_indirect));
        g_draw_indexed_slot = nullptr;
        g_draw_indexed_instanced_slot = nullptr;
        g_draw_indexed_instanced_indirect_slot = nullptr;
        g_draw_instanced_indirect_slot = nullptr;
        g_chams_solid_rasterizer.reset();
        g_chams_color_buffer.reset();
        g_chams_pixel_shader.reset();
        g_chams_blend_state.reset();
        g_chams_device.reset();
        g_chams_rasterizer_valid = false;
        log_line(L"D3D11 chams hooks removed (feature inactive)");
    }

    void __fastcall hooked_camera_update(void* manager, const float delta_seconds)
    {
        g_active_callbacks.fetch_add(1, std::memory_order_acq_rel);
        g_original_camera_update(manager, delta_seconds);
        // FOV must not depend on the render-thread state mutex. Actor discovery may
        // own that mutex for a camera tick; letting the engine FOV through on that
        // frame creates a visible zoom/unzoom pulse. ADS is still left untouched.
        if (g_fov_override_active.load(std::memory_order_relaxed) &&
            (GetAsyncKeyState(VK_RBUTTON) & 0x8000) == 0)
        {
            const float fov = g_custom_fov.load(std::memory_order_relaxed);
            SIZE_T written{};
            WriteProcessMemory(GetCurrentProcess(),
                reinterpret_cast<std::uint8_t*>(manager) + 0x500, &fov, sizeof(fov), &written);
        }
        if (!g_stop.load(std::memory_order_acquire))
        {
            // UpdateCamera runs on ARK's game thread while Present/update runs on the
            // render thread. Never stall the game thread behind actor discovery: mounted
            // flight (especially IceJumper) consumes the freshly produced camera direction
            // for movement and becomes jerky/stale if this callback waits on the mutex.
            std::unique_lock lock(g_state_mutex, std::try_to_lock);
            if (!lock.owns_lock())
            {
                // UpdateCamera has just restored the player camera. If Present owns
                // the state mutex, keep the last complete freecam pose for this tick
                // instead of exposing that engine pose as a one-frame teleport.
                if (g_freecam_active.load(std::memory_order_acquire))
                    hold_freecam_pose(manager);
                g_camera_tick_lock_skips.fetch_add(1, std::memory_order_relaxed);
                g_active_callbacks.fetch_sub(1, std::memory_order_acq_rel);
                return;
            }
            const int wheel_steps = g_freecam_wheel.exchange(0, std::memory_order_acq_rel);
            if (wheel_steps != 0)
            {
                g_settings.freecam_speed *= std::pow(1.15F, static_cast<float>(wheel_steps));
                g_settings.freecam_speed = std::clamp(g_settings.freecam_speed, 100.0F, 10000.0F);
            }
            g_runtime.on_game_camera_tick(g_settings, reinterpret_cast<std::uintptr_t>(manager), delta_seconds);
            if (g_settings.freecam && g_runtime.snapshot().camera.valid)
                publish_freecam_pose(g_runtime.snapshot().camera.location, g_runtime.snapshot().camera.rotation);
            else
                g_freecam_pose_valid.store(false, std::memory_order_release);
        }
        else if (!g_unload_cleanup_requested.exchange(true, std::memory_order_acq_rel))
        {
            std::scoped_lock lock(g_state_mutex);
            // Turning these off is a teardown step, not a change the user made.
            // g_settings is what gets written to disk on the way out, so without
            // remembering the real values first every unload persisted them as
            // disabled and they came back off on the next inject.
            g_unload_restore = {true, g_settings.freecam, g_settings.local_chams,
                g_settings.no_recoil, g_settings.no_sway};
            g_settings.freecam = false;
            g_freecam_active.store(false, std::memory_order_release);
            g_freecam_pose_valid.store(false, std::memory_order_release);
            g_settings.local_chams = false;
            g_settings.no_recoil = false;
            g_settings.no_sway = false;
            g_runtime.on_game_camera_tick(g_settings, reinterpret_cast<std::uintptr_t>(manager), delta_seconds);
            g_unload_cleanup_completed.store(true, std::memory_order_release);
        }
        g_active_callbacks.fetch_sub(1, std::memory_order_acq_rel);
    }

    void ensure_camera_hook()
    {
        if (g_camera_slot != nullptr || g_runtime.snapshot().camera_manager < 0x10000) return;
        auto** vtable = *reinterpret_cast<void***>(g_runtime.snapshot().camera_manager);
        if (vtable == nullptr) return;
        constexpr std::size_t update_camera_slot = 0xA38 / sizeof(void*);
        void** slot = &vtable[update_camera_slot];
        const auto target = reinterpret_cast<std::uintptr_t>(*slot);
        const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (target < module || target >= module + 0x50000000ULL)
        {
            log_line(L"Camera UpdateCamera vtable target failed module validation");
            return;
        }
        if (patch_slot(slot, reinterpret_cast<void*>(&hooked_camera_update),
            reinterpret_cast<void**>(&g_original_camera_update)))
        {
            g_camera_slot = slot;
            log_line(std::format(L"Camera game-tick hook installed at target 0x{:X}", target));
        }
    }

    void sync_camera_hook(const bool required)
    {
        if (required)
        {
            g_camera_hook_idle_frames.store(0, std::memory_order_relaxed);
            ensure_camera_hook();
            return;
        }
        if (g_camera_slot == nullptr)
        {
            g_camera_hook_idle_frames.store(0, std::memory_order_relaxed);
            return;
        }
        // Keep a short cleanup window so the game-thread callback observes disabled
        // settings and restores any transient recoil/sway/freecam/chams state before
        // the vtable is returned to the engine.
        const int idle_frames = g_camera_hook_idle_frames.fetch_add(1, std::memory_order_relaxed) + 1;
        if (idle_frames < 4) return;
        restore_slot(g_camera_slot, reinterpret_cast<void*>(g_original_camera_update));
        g_camera_slot = nullptr;
        g_camera_hook_idle_frames.store(0, std::memory_order_relaxed);
        log_line(L"Camera game-tick hook removed (all dependent features inactive)");
    }

    bool readable_object(const std::uintptr_t address) noexcept
    {
        if (address < 0x10000) return false;
        MEMORY_BASIC_INFORMATION information{};
        if (VirtualQuery(reinterpret_cast<const void*>(address), &information,
            sizeof(information)) != sizeof(information)) return false;
        return information.State == MEM_COMMIT &&
            (information.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
    }

    void sync_fov_lock(const std::uintptr_t manager, const bool enabled, const float fov)
    {
        // Exact ShooterGame.pdb 358.26 symbols. SetFOV/UnlockFOV update the native
        // bLockedFOV + LockedFOV state consumed by UpdateCamera, avoiding a render-thread
        // tug-of-war with CameraCache.POV.FOV. Unlock during RMB so ADS remains native.
        constexpr std::uintptr_t set_fov_rva = 0x2AB5BD0;
        constexpr std::uintptr_t unlock_fov_rva = 0x2AB5BE0;
        const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (module == 0) return;
        using SetFovFn = void(__fastcall*)(void*, float);
        using UnlockFovFn = void(__fastcall*)(void*);
        const auto set_fov = reinterpret_cast<SetFovFn>(module + set_fov_rva);
        const auto unlock_fov = reinterpret_cast<UnlockFovFn>(module + unlock_fov_rva);

        if ((!enabled || manager != g_locked_fov_manager) &&
            readable_object(g_locked_fov_manager))
            unlock_fov(reinterpret_cast<void*>(g_locked_fov_manager));
        if (!enabled || !readable_object(manager))
        {
            g_locked_fov_manager = 0;
            g_locked_fov_value = 0.0F;
            return;
        }
        if (manager != g_locked_fov_manager || std::abs(fov - g_locked_fov_value) > 0.001F)
        {
            set_fov(reinterpret_cast<void*>(manager), fov);
            g_locked_fov_manager = manager;
            g_locked_fov_value = fov;
        }
    }

    void sync_hotkeys()
    {
        g_menu_key.store(g_settings.menu_key, std::memory_order_release);
        g_unload_key.store(g_settings.unload_key, std::memory_order_release);
        g_freecam_key.store(g_settings.freecam_key, std::memory_order_release);
        g_esp_toggle_key.store(g_settings.esp_toggle_key, std::memory_order_release);
        const bool previous_freecam = g_freecam_active.exchange(g_settings.freecam, std::memory_order_acq_rel);
        if (previous_freecam != g_settings.freecam)
            g_freecam_pose_valid.store(false, std::memory_order_release);
        const std::uint32_t diagnostic_flags =
            (g_settings.esp_enabled ? (1U << 0) : 0U) |
            (g_settings.player_aim ? (1U << 1) : 0U) |
            (g_settings.dino_aim ? (1U << 2) : 0U) |
            (g_settings.freecam ? (1U << 3) : 0U) |
            (g_settings.local_chams ? (1U << 4) : 0U) |
            (g_settings.no_recoil ? (1U << 5) : 0U) |
            (g_settings.no_sway ? (1U << 6) : 0U);
        g_diagnostic_feature_flags.store(diagnostic_flags, std::memory_order_release);
        g_panic_key.store(g_settings.panic_key, std::memory_order_release);
        g_local_chams_draw_mode.store(g_settings.local_chams ? g_settings.local_chams_style : -1,
            std::memory_order_release);
        const auto channel = [](const float value) {
            return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
        };
        g_local_chams_color.store((channel(g_settings.local_chams_color.r) << 24) |
            (channel(g_settings.local_chams_color.g) << 16) |
            (channel(g_settings.local_chams_color.b) << 8) |
            channel(g_settings.local_chams_color.a), std::memory_order_release);
    }

    void set_menu_input_mode(const bool enabled)
    {
        g_menu_open.store(enabled, std::memory_order_release);
        if (enabled == g_menu_input_active)
        {
            if (enabled)
            {
                ClipCursor(nullptr);
                CURSORINFO cursor_info{sizeof(CURSORINFO)};
                if (GetCursorInfo(&cursor_info) == FALSE || (cursor_info.flags & CURSOR_SHOWING) == 0)
                {
                    int result{};
                    do
                    {
                        result = ShowCursor(TRUE);
                        ++g_cursor_show_adjustment;
                    } while (result < 0 && g_cursor_show_adjustment < 64);
                }
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            }
            return;
        }

        g_menu_input_active = enabled;
        ReleaseCapture();
        g_input.reset_pointer();
        if (enabled)
        {
            const bool left_down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            g_polled_left_down.store(left_down, std::memory_order_release);
            g_menu_pointer_armed.store(!left_down, std::memory_order_release);
            ClipCursor(nullptr);
            int result{};
            do
            {
                result = ShowCursor(TRUE);
                ++g_cursor_show_adjustment;
            } while (result < 0 && g_cursor_show_adjustment < 64);
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        }
        else
        {
            g_polled_left_down.store(false, std::memory_order_release);
            g_menu_pointer_armed.store(false, std::memory_order_release);
            while (g_cursor_show_adjustment > 0)
            {
                ShowCursor(FALSE);
                --g_cursor_show_adjustment;
            }
        }
    }

    void store_pointer_position(const int client_x, const int client_y)
    {
        g_input.mouse_x.store(static_cast<int>(std::lround(static_cast<float>(client_x) * g_pointer_scale_x)),
            std::memory_order_relaxed);
        g_input.mouse_y.store(static_cast<int>(std::lround(static_cast<float>(client_y) * g_pointer_scale_y)),
            std::memory_order_relaxed);
    }

    bool capture_binding(int key, bool suppress_key_up);
    void on_remote_batch(kopt::share::RemoteBatch batch);

    int sided_modifier_key(const int key, const LPARAM lparam)
    {
        if (key == VK_SHIFT)
        {
            const UINT scan = (static_cast<UINT>(lparam) >> 16) & 0xFFU;
            const UINT mapped = MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX);
            return mapped == VK_RSHIFT ? VK_RSHIFT : VK_LSHIFT;
        }
        if (key == VK_CONTROL) return (lparam & (1LL << 24)) != 0 ? VK_RCONTROL : VK_LCONTROL;
        if (key == VK_MENU) return (lparam & (1LL << 24)) != 0 ? VK_RMENU : VK_LMENU;
        return key;
    }

    void update_menu_pointer()
    {
        if (!g_menu_open.load(std::memory_order_acquire) || g_game_window == nullptr) return;
        POINT point{};
        if (GetCursorPos(&point) != FALSE && ScreenToClient(g_game_window, &point) != FALSE)
        {
            store_pointer_position(point.x, point.y);
        }
        const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (!g_menu_pointer_armed.load(std::memory_order_acquire))
        {
            g_polled_left_down.store(down, std::memory_order_release);
            g_input.reset_pointer();
            if (!down) g_menu_pointer_armed.store(true, std::memory_order_release);
            return;
        }
        const bool was_down = g_polled_left_down.exchange(down, std::memory_order_acq_rel);
        const bool left_pressed_edge = down && !was_down;
        if (left_pressed_edge) g_input.left_pressed.store(true, std::memory_order_release);
        g_input.left_down.store(down, std::memory_order_release);
        // WM_RBUTTONDOWN/WM_MBUTTONDOWN/WM_XBUTTONDOWN only reach game_wndproc if
        // the game hasn't claimed the mouse for raw/exclusive input, which on this
        // build it effectively always has outside of menu clicks. Track real
        // up-then-down edges for the other mouse buttons every frame (not just
        // while a bind capture is armed) so the edge state is already correct the
        // instant capture arms, exactly like left_pressed_edge above already is.
        static constexpr std::array<int, 4> other_mouse_buttons{
            VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
        static std::array<bool, other_mouse_buttons.size()> other_mouse_was_down{};
        std::array<bool, other_mouse_buttons.size()> other_mouse_edge{};
        for (std::size_t index = 0; index < other_mouse_buttons.size(); ++index)
        {
            const bool button_down = (GetAsyncKeyState(other_mouse_buttons[index]) & 0x8000) != 0;
            other_mouse_edge[index] = button_down && !other_mouse_was_down[index];
            other_mouse_was_down[index] = button_down;
        }
        if (g_input.binding_capture.load(std::memory_order_acquire))
        {
            static constexpr std::array<int, 6> modifiers{
                VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU, VK_LSHIFT, VK_RSHIFT};
            for (const int modifier : modifiers)
            {
                if ((GetAsyncKeyState(modifier) & 0x8000) != 0)
                {
                    capture_binding(modifier, true);
                    break;
                }
            }
            // A naive "is the button down right now" check here captures the tail
            // of the very click that opened the rebind box (it is, by definition,
            // still physically held on the frame capture arms), forcing users to
            // spam-click before a press finally landed on an already-released
            // button. Require a real release-then-press edge instead.
            if (left_pressed_edge) capture_binding(VK_LBUTTON, false);
            else for (std::size_t index = 0; index < other_mouse_buttons.size(); ++index)
            {
                if (other_mouse_edge[index])
                {
                    capture_binding(other_mouse_buttons[index], false);
                    break;
                }
            }
        }
    }

    bool capture_binding(const int key, const bool suppress_key_up)
    {
        if (!g_input.binding_capture.exchange(false, std::memory_order_acq_rel)) return false;
        log_line(std::format(L"Binding capture: key=0x{:02X}", static_cast<unsigned>(key)));
        g_input.captured_key.store(key, std::memory_order_release);
        // WM_LBUTTONDOWN is consumed above, but the polling fallback can still observe the
        // same physical press on the next Present. Quarantine it until a full release so
        // assigning Mouse 1 cannot also activate whichever widget is under the cursor.
        if (key == VK_LBUTTON)
        {
            g_menu_pointer_armed.store(false, std::memory_order_release);
            g_polled_left_down.store(true, std::memory_order_release);
            g_input.reset_pointer();
        }
        if (suppress_key_up) g_input.suppress_key_up.store(key, std::memory_order_release);
        return true;
    }

    LRESULT CALLBACK game_wndproc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == WM_INPUT && !g_menu_open.load(std::memory_order_acquire) &&
            g_freecam_active.load(std::memory_order_acquire))
        {
            RAWINPUT raw{};
            UINT size = sizeof(raw);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, &raw, &size,
                sizeof(RAWINPUTHEADER)) == sizeof(raw) && raw.header.dwType == RIM_TYPEMOUSE)
            {
                g_runtime.queue_freecam_mouse_delta(raw.data.mouse.lLastX, raw.data.mouse.lLastY);
            }
            return 0;
        }
        if (message == WM_MOUSEWHEEL && !g_menu_open.load(std::memory_order_acquire) &&
            g_freecam_active.load(std::memory_order_acquire))
        {
            g_freecam_wheel.fetch_add(GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA, std::memory_order_acq_rel);
            return 0;
        }
        if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) &&
            (lparam & (1LL << 30)) == 0 &&
            capture_binding(sided_modifier_key(static_cast<int>(wparam), lparam), true))
            return 0;
        if (message == WM_LBUTTONDOWN && capture_binding(VK_LBUTTON, false)) return 0;
        if (message == WM_RBUTTONDOWN && capture_binding(VK_RBUTTON, false)) return 0;
        if (message == WM_MBUTTONDOWN && capture_binding(VK_MBUTTON, false)) return 0;
        if (message == WM_XBUTTONDOWN)
        {
            const int key = GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
            if (capture_binding(key, false)) return TRUE;
        }

        if (message == WM_KEYUP || message == WM_SYSKEYUP)
        {
            const int released_key = sided_modifier_key(static_cast<int>(wparam), lparam);
            const int suppressed = g_input.suppress_key_up.load(std::memory_order_acquire);
            if (suppressed != 0 && suppressed == released_key)
            {
                g_input.suppress_key_up.store(0, std::memory_order_release);
                return 0;
            }
            if (released_key == static_cast<int>(g_menu_key.load(std::memory_order_acquire)))
            {
                g_input.toggle_menu_requested.store(true, std::memory_order_release);
                return 0;
            }
            if (released_key == static_cast<int>(g_unload_key.load(std::memory_order_acquire)))
            {
                g_stop.store(true);
                return 0;
            }
            if (released_key == static_cast<int>(g_freecam_key.load(std::memory_order_acquire)))
            {
                g_input.toggle_freecam_requested.store(true, std::memory_order_release);
                return 0;
            }
            if (released_key == static_cast<int>(g_esp_toggle_key.load(std::memory_order_acquire)))
            {
                g_input.toggle_esp_requested.store(true, std::memory_order_release);
                return 0;
            }
            if (released_key == static_cast<int>(g_panic_key.load(std::memory_order_acquire)))
            {
                g_input.toggle_panic_requested.store(true, std::memory_order_release);
                return 0;
            }
        }

        // Freecam consumes the same physical controls as the pawn. Keep release messages
        // flowing to the game so an input that was held while freecam was enabled cannot
        // stick, but isolate every new keyboard/mouse press from gameplay. WM_INPUT and the
        // wheel were already captured above for the free-camera controller.
        if (!g_menu_open.load(std::memory_order_acquire) &&
            g_freecam_active.load(std::memory_order_acquire))
        {
            const bool mouse_message = message >= WM_MOUSEFIRST && message <= WM_MOUSELAST;
            const bool keyboard_message = message >= WM_KEYFIRST && message <= WM_KEYLAST;
            const bool release_message = message == WM_KEYUP || message == WM_SYSKEYUP ||
                message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP ||
                message == WM_XBUTTONUP;
            const bool allow_system_close = message == WM_SYSKEYDOWN && wparam == VK_F4 &&
                (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
            if (release_message)
                return g_original_wndproc != nullptr ?
                    CallWindowProcW(g_original_wndproc, window, message, wparam, lparam) :
                    DefWindowProcW(window, message, wparam, lparam);
            if ((mouse_message || keyboard_message) && !allow_system_close) return 1;
        }

        if (g_menu_open.load(std::memory_order_acquire))
        {
            if (message == WM_CHAR)
            {
                g_input.queue_character(static_cast<wchar_t>(wparam));
                return 0;
            }
            // Windows never synthesizes WM_CHAR for Ctrl+V -- text_input()
            // only ever saw single keystrokes without this. API keys/JWTs
            // are long enough that requiring the player to type one by
            // hand (through a hooked, hidden-cursor overlay) isn't a real
            // option, so the paste is read here, off the clipboard, on the
            // keydown itself.
            if (message == WM_KEYDOWN && wparam == 'V' && (GetKeyState(VK_CONTROL) & 0x8000) != 0)
            {
                if (OpenClipboard(window))
                {
                    if (const HANDLE clipboard_data = GetClipboardData(CF_UNICODETEXT))
                    {
                        if (const auto* text = static_cast<const wchar_t*>(GlobalLock(clipboard_data)))
                        {
                            g_input.queue_paste(std::wstring(text));
                            GlobalUnlock(clipboard_data);
                        }
                    }
                    CloseClipboard();
                }
                return 0;
            }
            switch (message)
            {
            case WM_SETCURSOR:
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            case WM_MOUSEMOVE:
                store_pointer_position(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
                return 0;
            case WM_LBUTTONDOWN:
                store_pointer_position(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
                if (!g_menu_pointer_armed.load(std::memory_order_acquire))
                {
                    g_polled_left_down.store(true, std::memory_order_release);
                    g_input.reset_pointer();
                    return 0;
                }
                g_input.left_down.store(true);
                if (!g_polled_left_down.exchange(true, std::memory_order_acq_rel))
                    g_input.left_pressed.store(true);
                return 0;
            case WM_LBUTTONUP:
                g_input.left_down.store(false);
                g_polled_left_down.store(false, std::memory_order_release);
                g_menu_pointer_armed.store(true, std::memory_order_release);
                return 0;
            case WM_RBUTTONDOWN:
                store_pointer_position(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
                g_input.right_pressed.store(true, std::memory_order_release);
                return 0;
            case WM_RBUTTONUP:
                return 0;
            case WM_MOUSEWHEEL:
                g_input.wheel.fetch_add(GET_WHEEL_DELTA_WPARAM(wparam));
                return 0;
            case WM_INPUT:
            case WM_KEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYDOWN:
            case WM_SYSKEYUP:
            case WM_CHAR:
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:
                return 0;
            case WM_KILLFOCUS:
                g_input.reset_pointer();
                g_polled_left_down.store(false, std::memory_order_release);
                g_menu_pointer_armed.store(false, std::memory_order_release);
                ReleaseCapture();
                break;
            default:
                break;
            }
        }
        return g_original_wndproc != nullptr ?
            CallWindowProcW(g_original_wndproc, window, message, wparam, lparam) :
            DefWindowProcW(window, message, wparam, lparam);
    }

    bool select_game_swap_chain(IDXGISwapChain* swap_chain, const bool refresh_metrics = false)
    {
        if (g_game_swap_chain == swap_chain && !refresh_metrics) return true;
        DXGI_SWAP_CHAIN_DESC description{};
        if (FAILED(swap_chain->GetDesc(&description)) || description.OutputWindow == nullptr) return false;
        ComPtr<ID3D11Texture2D> buffer;
        if (FAILED(swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(buffer.put())))) return false;
        D3D11_TEXTURE2D_DESC buffer_description{};
        buffer->GetDesc(&buffer_description);
        const std::uint64_t area = static_cast<std::uint64_t>(buffer_description.Width) * buffer_description.Height;
        if (g_game_swap_chain != nullptr && g_game_swap_chain != swap_chain && area <= g_game_swap_chain_area)
            return false;

        if (g_game_swap_chain != swap_chain)
        {
            if (g_game_window != nullptr && g_game_window != description.OutputWindow &&
                g_original_wndproc != nullptr && IsWindow(g_game_window))
                SetWindowLongPtrW(g_game_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_original_wndproc));
            if (g_game_window != description.OutputWindow)
            {
                SetLastError(ERROR_SUCCESS);
                const auto previous = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(description.OutputWindow,
                    GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&game_wndproc)));
                if (previous == nullptr && GetLastError() != ERROR_SUCCESS) return false;
                g_game_window = description.OutputWindow;
                g_original_wndproc = previous;
            }
            g_game_swap_chain = swap_chain;
            g_game_swap_chain_area = area;
            g_overlay.invalidate();
            g_overlay_init_logged.store(false, std::memory_order_release);
            log_line(std::format(L"Selected primary swap chain {}x{}", buffer_description.Width,
                buffer_description.Height));
            set_menu_input_mode(g_settings.menu_open);
        }
        g_game_swap_chain_area = area;
        RECT client{};
        if (GetClientRect(description.OutputWindow, &client) != FALSE && client.right > client.left &&
            client.bottom > client.top)
        {
            g_pointer_scale_x = static_cast<float>(buffer_description.Width) /
                static_cast<float>(client.right - client.left);
            g_pointer_scale_y = static_cast<float>(buffer_description.Height) /
                static_cast<float>(client.bottom - client.top);
        }
        return true;
    }

    HRESULT __stdcall hooked_present(IDXGISwapChain* swap_chain, const UINT sync_interval, const UINT flags)
    {
        g_active_callbacks.fetch_add(1);
        if (!g_first_present_logged.exchange(true)) log_line(L"First hooked Present call received");
        if (!g_stop.load())
        {
            const bool primary_swap_chain = select_game_swap_chain(swap_chain);
            const bool overlay_ready = primary_swap_chain && g_overlay.initialize(swap_chain);
            if (!g_overlay_init_logged.exchange(true))
                log_line(overlay_ready ? L"D3D11 overlay initialized" : L"D3D11 overlay initialization failed");
            if (overlay_ready)
            {
                std::scoped_lock state_lock(g_state_mutex);
                if (g_input.toggle_menu_requested.exchange(false, std::memory_order_acq_rel))
                {
                    g_settings.menu_open = !g_settings.menu_open;
                    log_line(g_settings.menu_open ? L"Hotkey: menu opened" : L"Hotkey: menu closed");
                }
                if (g_input.toggle_freecam_requested.exchange(false, std::memory_order_acq_rel))
                {
                    g_settings.freecam = !g_settings.freecam;
                    log_line(g_settings.freecam ? L"Hotkey: freecam enabled" : L"Hotkey: freecam disabled");
                }
                if (g_input.toggle_esp_requested.exchange(false, std::memory_order_acq_rel))
                {
                    g_settings.esp_enabled = !g_settings.esp_enabled;
                    log_line(g_settings.esp_enabled ? L"Hotkey: ESP enabled" : L"Hotkey: ESP disabled");
                }
                if (g_input.toggle_panic_requested.exchange(false, std::memory_order_acq_rel))
                {
                    if (!g_panic_state.active)
                    {
                        g_panic_state = {true, g_settings.menu_open, g_settings.esp_enabled,
                            g_settings.freecam, g_settings.local_chams,
                            g_settings.no_recoil, g_settings.no_sway};
                        g_settings.menu_open = false;
                        g_settings.esp_enabled = false;
                        g_settings.freecam = false;
                        g_settings.local_chams = false;
                        g_settings.no_recoil = false;
                        g_settings.no_sway = false;
                        log_line(L"Hotkey: panic state enabled");
                    }
                    else
                    {
                        g_settings.menu_open = g_panic_state.menu;
                        g_settings.esp_enabled = g_panic_state.esp;
                        g_settings.freecam = g_panic_state.freecam;
                        g_settings.local_chams = g_panic_state.local_chams;
                        g_settings.no_recoil = g_panic_state.no_recoil;
                        g_settings.no_sway = g_panic_state.no_sway;
                        g_panic_state.active = false;
                        log_line(L"Hotkey: panic state restored");
                    }
                }
                set_menu_input_mode(g_settings.menu_open);
                update_menu_pointer();
                const auto now = std::chrono::steady_clock::now();
                float delta_seconds = 1.0F / 60.0F;
                if (g_last_frame.time_since_epoch().count() != 0)
                    delta_seconds = std::chrono::duration<float>(now - g_last_frame).count();
                g_last_frame = now;
                g_settings.normalize();
                g_custom_fov.store(g_settings.camera_fov, std::memory_order_relaxed);
                g_fov_override_active.store(g_settings.fov_override &&
                    !g_runtime.snapshot().local_mounted, std::memory_order_relaxed);
                g_overlay.update_feature_hotkeys(g_settings);
                g_runtime.update(g_settings, delta_seconds);
                const auto& snapshot = g_runtime.snapshot();
                // Pass-through DrawIndexed hooks still intercept every render call and
                // add cross-thread atomics even when chams are disabled. Install them
                // only for the one feature which actually consumes those callbacks.
                const bool mounted_safe_mode = snapshot.local_mounted;
                g_fov_override_active.store(g_settings.fov_override && !mounted_safe_mode,
                    std::memory_order_relaxed);
                if (mounted_safe_mode != g_logged_mounted_safe_mode)
                {
                    g_logged_mounted_safe_mode = mounted_safe_mode;
                    log_line(mounted_safe_mode ?
                        std::format(L"Mounted-safe mode entered: ack={} controllerPawn={} controllerPawnIsDino={} ",
                            snapshot.local_pawn_class_name, snapshot.controller_pawn_class_name,
                            snapshot.controller_reports_riding) +
                            L"passive camera/FOV/chams hooks suspended" :
                        L"Mounted-safe mode exited");
                }
                sync_draw_indexed_hooks(swap_chain, g_settings.local_chams && !mounted_safe_mode);
                const auto aim_tick_requested = [](const bool enabled, const std::uint32_t key,
                    const std::int32_t mode, const bool active) {
                    if (!enabled) return false;
                    const bool down = (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
                    return mode == 0 ? down : mode == 1 ? down || active : true;
                };
                // Player/Dino Aim select target classes, not rider state. Both remain
                // available on foot. While mounted, only Managarmr/Wyvern use a route
                // which avoids Controller::ControlRotation and therefore preserves WASD.
                const bool aim_route_available = !mounted_safe_mode || snapshot.managarmr_safe_aim;
                const bool aim_tick_required = aim_route_available &&
                    (aim_tick_requested(g_settings.player_aim, g_settings.aim_key,
                        g_settings.aim_activation_mode, snapshot.player_aim_active) ||
                     aim_tick_requested(g_settings.dino_aim, g_settings.dino_aim_key,
                        g_settings.dino_aim_activation_mode, snapshot.dino_aim_active));
                const bool passive_game_tick_features = g_settings.no_recoil || g_settings.no_sway ||
                    g_settings.local_chams;
                // Freecam keeps the camera tick even while mounted: it only moves
                // the camera POV, unlike the passive features below, which write
                // character offsets and must stay off a dino pawn.
                const bool camera_tick_required = aim_tick_required || g_settings.freecam ||
                    (!mounted_safe_mode && passive_game_tick_features);
                sync_camera_hook(camera_tick_required);

                // A plain FOV override is a POD camera-cache update and does not justify
                // interposing PlayerCameraManager::UpdateCamera. Keeping the vtable native
                // while only ESP/OSD/FOV is active prevents mounted steering from inheriting
                // callback timing or ABI side effects.
                sync_fov_lock(snapshot.camera_manager,
                    g_settings.fov_override && !mounted_safe_mode &&
                        (GetAsyncKeyState(VK_RBUTTON) & 0x8000) == 0,
                    g_settings.camera_fov);
                // Кнопка Apply на Diagnostics-табе: g_publisher->start()
                // захватывает токен в воркер-поток один раз (см. его
                // собственный комментарий), внутренний reconnect-луп
                // переиспользует именно эту копию вечно -- правка поля API
                // key после того, как Share уже был включён, иначе ни на
                // что не влияет, пока не переключить чекбокс туда-обратно.
                // Сброс g_share_started здесь заставляет блок ниже
                // перезапустить паблишер со свежим g_overlay.share_api_key().
                if (g_overlay.consume_share_reconnect_request()) g_share_started = false;
                // Живой старт/стоп: тумблер в интерфейсе (Diagnostics-таб)
                // меняет g_settings.share_enabled в любой момент, а не
                // только на загрузке payload'а -- без этой проверки
                // включение шера в рантайме молчало бы до перезапуска.
                if (g_settings.share_enabled && !g_share_started)
                {
                    // Diagnostics-таб держит API-ключ только в памяти
                    // оверлея (Overlay::share_api_key_, не Settings -- см.
                    // его doc comment в overlay.hpp), и годится ровно на то
                    // место, куда иначе шёл бы g_share_token: тот же Bearer
                    // проверяется на месте JWT бэкендом (core/account_auth.py
                    // ::get_current_account). g_share_token берёт верх, если
                    // оно есть (--share-token был передан при инжекте) --
                    // ключ из меню существует именно на случай, когда его
                    // не передали.
                    const std::wstring& effective_token = !g_share_token.empty() ?
                        g_share_token : g_overlay.share_api_key();
                    // Same override precedence as the token above: a
                    // --backend passed at inject time (see
                    // publish_backend_endpoint's doc comment in
                    // injector.cpp) beats kopt_internal.ini's Share.Endpoint
                    // -- moving the relay to a VPS is then a launch
                    // argument, not an ini edit or a rebuild.
                    const std::wstring& effective_endpoint = !g_backend_endpoint.empty() ?
                        g_backend_endpoint : g_settings.share_endpoint;
                    if (effective_token.empty())
                    {
                        // g_share_token is fixed for the lifetime of this
                        // process (read once at worker() startup) and the
                        // menu's API key can still change on a later frame --
                        // log once per empty spell, not once ever, so typing
                        // a key in after this fires is still noticed.
                        if (!g_share_missing_token_logged)
                        {
                            g_share_missing_token_logged = true;
                            log_line(L"Share enabled in settings but no --share-token was given at inject time "
                                L"and no API key is set on the Diagnostics tab; not starting publisher");
                        }
                    }
                    // Gated on local_valid, not just "is some server_ip
                    // available": g_settings.share_server_ip (the manual
                    // ini fallback) is non-empty from the moment the ini
                    // loads, long before ArkRuntime ever resolves a world --
                    // checking effective_server_ip.empty() alone would have
                    // used that stale fallback on the very first tick and
                    // never given Snapshot::remote_server_ip (see
                    // ArkRuntime::read_remote_server_ip) a chance to
                    // populate at all. Waiting for local_valid first means
                    // the ini value is only ever a genuine fallback -- used
                    // when the live read actually failed post-spawn, not a
                    // race winner against it. Confirmed live: without this,
                    // the publisher started with the ini's placeholder
                    // 10.99.0.1:7777 instead of the real, freshly-resolved
                    // server address.
                    else if (!snapshot.local_valid)
                    {
                        // Normal transient state during the loading screen/
                        // main menu -- deliberately silent (no log_line
                        // here), or this would write to disk every single
                        // frame until the player spawns in.
                    }
                    else if (snapshot.remote_server_ip.empty() && g_settings.share_server_ip.empty())
                    {
                        // local_valid is true (world resolved) and the live
                        // read still came back empty, with no ini fallback
                        // configured either -- a genuine problem, not a
                        // transient one, so unlike the branch above this is
                        // worth a one-shot log.
                        if (!g_share_missing_server_ip_logged)
                        {
                            g_share_missing_server_ip_logged = true;
                            log_line(L"Share enabled but the server address could not be read from game memory "
                                L"and Share.ServerIp is not set in kopt_internal.ini; not starting publisher");
                        }
                    }
                    else
                    {
                        // Doesn't yet reconnect the publisher on a
                        // mid-session server change (Obelisk travel to a
                        // different server) -- start() only ever fires once
                        // per share_enabled toggle; a real follow-up gap,
                        // not silently ignored.
                        const std::wstring effective_server_ip = !snapshot.remote_server_ip.empty() ?
                            snapshot.remote_server_ip : g_settings.share_server_ip;
                        log_line(std::format(L"Starting share publisher: {} (server={})",
                            effective_endpoint, effective_server_ip));
                        g_publisher->start(effective_endpoint, effective_token, effective_server_ip);
                        g_publisher->subscribe(on_remote_batch);
                        g_share_started = true;
                    }
                }
                else if (!g_settings.share_enabled && g_share_started)
                {
                    g_publisher->stop();
                    g_share_started = false;
                }
                if (g_settings.share_enabled &&
                    std::chrono::duration<float, std::milli>(now - g_last_share_submit).count() >=
                        g_settings.share_interval_ms)
                {
                    g_last_share_submit = now;
                    // build_sightings() -- то же самое множество акторов,
                    // что уже наполняет ESP этот кадр: источник правды здесь,
                    // а не отдельный пересчёт "что показать по сети".
                    std::vector<kopt::share::Sighting> all_sightings =
                        kopt::share::build_sightings(snapshot.actors);
                    // snapshot.actors never contains the local player itself
                    // (ArkRuntime excludes it on purpose -- it's "what I see
                    // around me", and self needs no ESP box); sharing exists
                    // precisely so teammates see the sender's own position,
                    // so it goes in separately here rather than in
                    // build_sightings() itself.
                    //
                    // Gated on share_send_self_position independently of
                    // everything else in this batch -- first instance of
                    // "send" being its own axis from "scan"/"render" (see
                    // Settings::share_send_self_position's own comment):
                    // this flag only ever affects whether self goes out over
                    // the wire, never local ESP (self was never drawn there
                    // to begin with).
                    if (g_settings.share_send_self_position)
                    {
                        if (const auto self = kopt::share::build_self_sighting(snapshot))
                            all_sightings.push_back(*self);
                    }
                    // reporter identity/position -- independent axis from
                    // share_send_self_position above (that one only gates
                    // whether self appears as a drawable entity in this
                    // same batch): dedup metadata for the receiving side's
                    // ReporterFilter, sent whenever known regardless of
                    // that setting. snapshot.local_stable_id/local_position
                    // are already "sticky" (see their own doc comments in
                    // runtime.hpp) -- a momentary !local_valid here just
                    // means submit_sightings gets the last-known values,
                    // not zeros, consistent with that documented behavior.
                    g_publisher->submit_sightings(
                        g_share_filter.filter(all_sightings, now),
                        g_share_filter.collect_vanished(all_sightings),
                        snapshot.local_stable_id, snapshot.local_position);
                    if (!snapshot.alerts.empty())
                    {
                        std::vector<kopt::share::Notification> notifications;
                        notifications.reserve(snapshot.alerts.size());
                        for (const kopt::Alert& alert : snapshot.alerts)
                            notifications.push_back(kopt::share::build_notification(alert));
                        g_publisher->submit_notifications(std::move(notifications));
                    }
                }
                if (snapshot.world_generation != g_logged_world_generation)
                {
                    g_logged_world_generation = snapshot.world_generation;
                    g_logged_horde_previews.clear();
                    g_logged_explorer_notes.clear();
                    g_logged_player_contacts.clear();
                    log_line(std::format(L"World generation changed: {} address=0x{:X}",
                        snapshot.world_generation, snapshot.world_address));
                }
                if (snapshot.local_valid != g_logged_local_valid)
                {
                    g_logged_local_valid = snapshot.local_valid;
                    log_line(snapshot.local_valid ? L"Local player runtime became valid" : L"Local player runtime invalidated");
                }
                if (snapshot.aim_active != g_logged_aim_active)
                {
                    g_logged_aim_active = snapshot.aim_active;
                    log_line(snapshot.aim_active ? L"Aim activation entered fresh active state" : L"Aim activation released");
                }
                if (snapshot.managarmr_safe_aim != g_logged_managarmr_safe_aim)
                {
                    g_logged_managarmr_safe_aim = snapshot.managarmr_safe_aim;
                    log_line(snapshot.managarmr_safe_aim ?
                        std::format(L"Mounted movement-independent aim-offset route active: {}",
                            snapshot.local_pawn_class_name) :
                        L"Mounted movement-independent aim-offset route inactive");
                }
                for (const auto& actor : snapshot.actors)
                {
                    if (actor.kind != kopt::ActorKind::horde_crate && actor.kind != kopt::ActorKind::element_node)
                        continue;
                    std::wstring preview = actor.reward_preview;
                    std::replace(preview.begin(), preview.end(), L'\n', L' ');
                    const std::wstring signature = actor.class_name + L"|" + std::to_wstring(actor.horde_wave) +
                        L"|" + actor.reward_diagnostic + L"|" + preview;
                    if (g_logged_horde_previews[actor.address] == signature) continue;
                    g_logged_horde_previews[actor.address] = signature;
                    log_line(std::format(L"Horde preview 0x{:X}: class={} wave={} exact={} [{}] {}",
                        actor.address, actor.class_name, actor.horde_wave, actor.reward_exact,
                        actor.reward_diagnostic, preview));
                }
                for (const auto& actor : snapshot.actors)
                {
                    if (actor.kind != kopt::ActorKind::explorer_note) continue;
                    const std::wstring signature = actor.class_name + L"|" + actor.name;
                    if (g_logged_explorer_notes[actor.address] == signature) continue;
                    g_logged_explorer_notes[actor.address] = signature;
                    log_line(std::format(L"Explorer Note ESP 0x{:X}: class={} label={}",
                        actor.address, actor.class_name, actor.name));
                }
                // Every player contact goes into the log with its SteamID64, so the
                // record survives the session and the 60-minute in-game journal.
                // This is deliberately not gated on the ESP relation filter: the
                // point is a complete contact history, not what was drawn.
                for (const auto& actor : snapshot.actors)
                {
                    if (actor.kind != kopt::ActorKind::player) continue;
                    const std::wstring signature = actor.name + L"|" + actor.tribe + L"|" +
                        std::to_wstring(actor.steam_id) + L"|" + std::to_wstring(actor.steam_id_stage);
                    const auto known = g_logged_player_contacts.find(actor.address);
                    if (known != g_logged_player_contacts.end() && known->second == signature) continue;
                    const bool update = known != g_logged_player_contacts.end();
                    g_logged_player_contacts[actor.address] = signature;
                    const float to_local_x = actor.position.x - snapshot.local_position.x;
                    const float to_local_y = actor.position.y - snapshot.local_position.y;
                    const float to_local_z = actor.position.z - snapshot.local_position.z;
                    const float distance_m = snapshot.local_valid ?
                        std::sqrt(to_local_x * to_local_x + to_local_y * to_local_y +
                            to_local_z * to_local_z) / 100.0F : 0.0F;
                    log_line(std::format(
                        L"Player {} 0x{:X}: name={} tribe={} steamid64={} team={} pos=({:.0f},{:.0f},{:.0f}) dist={:.0f}m",
                        update ? L"contact updated" : L"contact", actor.address,
                        actor.name.empty() ? L"?" : actor.name,
                        actor.tribe.empty() ? L"?" : actor.tribe,
                        actor.steam_id == 0 ?
                            L"unresolved(stage=" + std::to_wstring(actor.steam_id_stage) + L")" :
                            std::to_wstring(actor.steam_id),
                        actor.team, actor.position.x, actor.position.y, actor.position.z, distance_m));
                }
                const std::uint32_t guard_hits = g_skeleton_guard_hits.load(std::memory_order_relaxed);
                if (guard_hits != g_logged_skeleton_guard_hits)
                {
                    g_logged_skeleton_guard_hits = guard_hits;
                    log_line(std::format(L"Flight-death skeleton guard recovered {} invalid montage slot reference(s)",
                        guard_hits));
                }
                // No unconditional ensure_camera_hook() here any more: the camera
                // tick is now installed and torn down by sync_camera_hook() above,
                // which only keeps it while a feature actually needs it and drops
                // it while mounted. Re-installing it every frame here would undo
                // that teardown immediately.
                g_overlay.set_share_connected(g_publisher->connected());
                // Same override precedence as effective_endpoint above (a
                // --backend at inject time beats kopt_internal.ini's
                // Share.Endpoint) -- shown on the Diagnostics tab every
                // frame, not just while a start attempt is being gated,
                // so the menu always reflects what g_publisher is actually
                // configured for, not the compile-time KOPT_DEFAULT_SHARE_ENDPOINT.
                g_overlay.set_share_endpoint(!g_backend_endpoint.empty() ?
                    g_backend_endpoint : g_settings.share_endpoint);
                {
                    // Copy under the lock, hand the plain vector to the
                    // overlay -- it never touches g_remote_view_mutex
                    // itself, same reasoning as set_share_connected's own
                    // doc comment (overlay stays ignorant of the network/
                    // threading side entirely).
                    std::vector<kopt::share::RemoteBatch> visible_batches;
                    {
                        const std::lock_guard<std::mutex> lock(g_remote_view_mutex);
                        visible_batches = g_remote_view.visible(now);
                    }
                    g_overlay.set_remote_sightings(std::move(visible_batches));
                }
                g_overlay.render(swap_chain, g_settings, g_runtime, g_input, g_settings_path);
                // Not gated on debug_panel. This is the periodic performance record
                // in the log, not part of the on-screen panel, and tying the two
                // together meant closing the panel silently stopped the only
                // measurement that survives the session.
                {
                    std::wstring diagnostics;
                    if (g_overlay.take_diagnostics_line(g_runtime, diagnostics)) log_line(diagnostics);
                }
                if (g_input.diagnostics_bundle_requested.exchange(false, std::memory_order_acq_rel))
                {
                    const auto bundle = write_diagnostics_bundle(nullptr, false);
                    log_line(bundle.empty() ? L"Manual diagnostics bundle failed" :
                        L"Manual diagnostics bundle: " + bundle.wstring());
                }
                sync_hotkeys();
            }
        }
        const HRESULT result = g_original_present(swap_chain, sync_interval, flags);
        g_active_callbacks.fetch_sub(1);
        return result;
    }

    HRESULT __stdcall hooked_resize(IDXGISwapChain* swap_chain, const UINT count,
        const UINT width, const UINT height, const DXGI_FORMAT format, const UINT flags)
    {
        g_active_callbacks.fetch_add(1);
        if (swap_chain == g_game_swap_chain) g_overlay.invalidate();
        const HRESULT result = g_original_resize(swap_chain, count, width, height, format, flags);
        if (swap_chain == g_game_swap_chain && SUCCEEDED(result)) select_game_swap_chain(swap_chain, true);
        g_active_callbacks.fetch_sub(1);
        return result;
    }

    LRESULT CALLBACK dummy_wndproc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        return DefWindowProcW(window, message, wparam, lparam);
    }

    // Confirmed by reproducing against a real launch: touching D3D11 at all
    // -- even just D3D11CreateDeviceAndSwapChain for a throwaway probe
    // device to read the vtable, below -- before Wine's DXVK/vkd3d backend
    // has finished initializing corrupts state badly enough to crash the
    // whole process a few seconds later (bad vtable pointer, garbage
    // instruction at the "Present" slot). The payload loads via version.dll
    // at process start, long before the engine creates its own device, so
    // install_hooks() used to run immediately with no guard at all. d3d11/
    // dxgi being loaded is necessary but not sufficient -- give DXVK a
    // grace period afterward too, same margin ark_fun_tools' own working
    // overlay uses for the same reason on this same game/Wine stack.
    bool wait_for_render_ready()
    {
        for (int waited_ms = 0; !g_stop.load(std::memory_order_acquire); waited_ms += 250)
        {
            if (GetModuleHandleW(L"d3d11.dll") != nullptr && GetModuleHandleW(L"dxgi.dll") != nullptr)
            {
                log_line(L"d3d11/dxgi present; waiting for DXVK to settle before probing");
                Sleep(2000);
                return true;
            }
            if (waited_ms > 0 && waited_ms % 30000 == 0)
                log_line(std::format(L"Still waiting for d3d11/dxgi to load ({} s)", waited_ms / 1000));
            Sleep(250);
        }
        return false;
    }

    bool install_hooks()
    {
        log_line(L"Installing DXGI hooks");
        constexpr wchar_t class_name[] = L"KOPTInternalDummyWindow";
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = dummy_wndproc;
        window_class.hInstance = g_module;
        window_class.lpszClassName = class_name;
        const ATOM atom = RegisterClassExW(&window_class);
        if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            log_line(std::format(L"RegisterClassExW failed: {}", GetLastError()));
            return false;
        }
        const HWND window = CreateWindowExW(0, class_name, L"", WS_OVERLAPPEDWINDOW,
            0, 0, 100, 100, nullptr, nullptr, g_module, nullptr);
        if (window == nullptr)
        {
            log_line(std::format(L"CreateWindowExW failed: {}", GetLastError()));
            return false;
        }

        DXGI_SWAP_CHAIN_DESC description{};
        description.BufferCount = 1;
        description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.OutputWindow = window;
        description.SampleDesc.Count = 1;
        description.Windowed = TRUE;
        description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        ComPtr<IDXGISwapChain> swap_chain;
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL feature_level{};
        HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &description,
            swap_chain.put(), device.put(), &feature_level, context.put());
        if (FAILED(result))
        {
            log_line(std::format(L"Hardware dummy swap chain failed: 0x{:08X}", static_cast<unsigned>(result)));
            result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &description,
                swap_chain.put(), device.put(), &feature_level, context.put());
        }
        if (FAILED(result))
        {
            log_line(std::format(L"WARP dummy swap chain failed: 0x{:08X}", static_cast<unsigned>(result)));
            DestroyWindow(window);
            UnregisterClassW(class_name, g_module);
            return false;
        }

        auto** vtable = *reinterpret_cast<void***>(swap_chain.get());
        log_line(std::format(L"Dummy swap chain created; vtable=0x{:X}", reinterpret_cast<std::uintptr_t>(vtable)));
        g_present_slot = &vtable[8];
        g_resize_slot = &vtable[13];
        const bool present_ok = patch_slot(g_present_slot, reinterpret_cast<void*>(&hooked_present),
            reinterpret_cast<void**>(&g_original_present));
        const bool resize_ok = patch_slot(g_resize_slot, reinterpret_cast<void*>(&hooked_resize),
            reinterpret_cast<void**>(&g_original_resize));
        log_line(std::format(L"Vtable patches: Present={} ResizeBuffers={}", present_ok, resize_ok));
        if (!present_ok || !resize_ok)
        {
            if (present_ok) restore_slot(g_present_slot, reinterpret_cast<void*>(g_original_present));
            if (resize_ok) restore_slot(g_resize_slot, reinterpret_cast<void*>(g_original_resize));
        }
        swap_chain.reset();
        context.reset();
        device.reset();
        DestroyWindow(window);
        UnregisterClassW(class_name, g_module);
        return present_ok && resize_ok;
    }

    void uninstall_hooks()
    {
        g_local_chams_draw_mode.store(-1, std::memory_order_release);
        sync_fov_lock(0, false, 0.0F);
        set_menu_input_mode(false);
        restore_slot(g_camera_slot, reinterpret_cast<void*>(g_original_camera_update));
        g_camera_slot = nullptr;
        g_original_camera_update = nullptr;
        restore_slot(g_draw_indexed_slot, reinterpret_cast<void*>(g_original_draw_indexed));
        g_draw_indexed_slot = nullptr;
        restore_slot(g_draw_indexed_instanced_slot,
            reinterpret_cast<void*>(g_original_draw_indexed_instanced));
        g_draw_indexed_instanced_slot = nullptr;
        restore_slot(g_draw_indexed_instanced_indirect_slot,
            reinterpret_cast<void*>(g_original_draw_indexed_instanced_indirect));
        g_draw_indexed_instanced_indirect_slot = nullptr;
        restore_slot(g_draw_instanced_indirect_slot,
            reinterpret_cast<void*>(g_original_draw_instanced_indirect));
        g_draw_instanced_indirect_slot = nullptr;
        restore_slot(g_resize_slot, reinterpret_cast<void*>(g_original_resize));
        restore_slot(g_present_slot, reinterpret_cast<void*>(g_original_present));
        if (g_game_window != nullptr && g_original_wndproc != nullptr && IsWindow(g_game_window))
        {
            SetWindowLongPtrW(g_game_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_original_wndproc));
        }
        g_game_window = nullptr;
        g_game_swap_chain = nullptr;
        g_game_swap_chain_area = 0;
        g_original_wndproc = nullptr;
        g_overlay.invalidate();
    }

    std::filesystem::path module_directory()
    {
        std::wstring buffer(32768, L'\0');
        const DWORD length = GetModuleFileNameW(g_module, buffer.data(), static_cast<DWORD>(buffer.size()));
        buffer.resize(length);
        return std::filesystem::path(buffer).parent_path();
    }

    // Config and log live in one fixed per-user location instead of beside the
    // DLL. Injecting the same build from a different folder used to mean a
    // different ini, so settings looked like they reset themselves whenever the
    // payload was run from a build output, a copied bundle or a temp directory.
    // Falls back to the module directory when LOCALAPPDATA is unavailable.
    std::filesystem::path user_data_directory()
    {
        const wchar_t* local_app_data = _wgetenv(L"LOCALAPPDATA");
        if (local_app_data == nullptr || *local_app_data == L'\0') return module_directory();
        std::filesystem::path directory = std::filesystem::path(local_app_data) / L"KOPT";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) return module_directory();
        return directory;
    }

    // Приёмный колбэк подписки Publisher::subscribe -- вызывается на
    // фоновом read-потоке реализации транспорта, поэтому: (1) не трогает
    // ничего из hot-path Present-хука напрямую, только g_remote_view под
    // мьютексом; (2) сам дедуплицирует по репортёру через ReporterFilter,
    // не полагаясь на то, что сервер уже отфильтровал.
    //
    // ReporterFilter строится заново на каждый батч, а не кэшируется: оба
    // его параметра (own_stable_id, радиус) могут поменяться между вызовами
    // -- локальный игрок определяется не сразу после входа в игру, а радиус
    // "esp_distance_m" настраивается вручную -- и сам объект тривиален
    // (два поля), пересоздание стоит меньше, чем синхронизация кеша.
    void on_remote_batch(kopt::share::RemoteBatch batch)
    {
        const auto& snapshot = g_runtime.snapshot();
        if (!snapshot.local_valid) return; // своя позиция ещё не известна -- решить "рядом или нет" нечем
        const kopt::share::ReporterFilter filter(snapshot.local_stable_id,
            g_settings.esp_distance_m * 100.0F);
        if (!filter.accept(batch.reporter_stable_id, batch.reporter_position, snapshot.local_position)) return;
        batch.received_at = std::chrono::steady_clock::now();
        const std::lock_guard<std::mutex> lock(g_remote_view_mutex);
        g_remote_view.update(std::move(batch));
    }

    DWORD WINAPI worker(void*)
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const auto directory = user_data_directory();
        g_settings_path = directory / L"kopt_internal.ini";
        g_log_path = directory / L"kopt_internal.log";
        // One-time adoption of a config that still sits next to the DLL, so
        // moving to the shared location does not silently start from defaults.
        if (!std::filesystem::exists(g_settings_path))
        {
            const auto legacy = module_directory() / L"kopt_internal.ini";
            std::error_code copy_error;
            if (legacy != g_settings_path && std::filesystem::exists(legacy, copy_error))
            {
                std::filesystem::copy_file(legacy, g_settings_path,
                    std::filesystem::copy_options::none, copy_error);
                if (!copy_error) log_line(L"Adopted existing configuration from the payload directory");
            }
        }
        log_line(L"Payload worker started");
        log_line(L"Configuration: " + g_settings_path.wstring());
        // Named per-PID so kopt_injector.exe's --unload can find it without
        // already knowing kopt_payload.dll's remote base address. This
        // REPLACES an earlier CreateRemoteThread-into-KoptRequestUnload's-
        // own-address design: that worked for the very first injection of a
        // session but was found to reliably time out (5s, every time) on a
        // long-running Wine/Proton session that had already gone through a
        // few inject/unload cycles -- CreateRemoteThread's raw start-address
        // form is the well-worn, heavily-exercised path when the target is
        // a builtin export (kernel32!LoadLibraryW, used by inject() above,
        // which never showed this problem); jumping directly into an
        // arbitrary address inside a third-party, non-builtin PE module is
        // a far less exercised corner of Wine's thread creation, and this
        // symptom (silent hang, not a crash, not a wrong-result) matches
        // that being the less reliable path rather than a bug in
        // KoptRequestUnload's own two-instruction body. A named event is
        // the standard, boring, universally-supported way to signal into a
        // process without running code at an address the caller had to
        // guess at cross-process -- kernel32's CreateEvent/SetEvent are
        // exactly as "builtin" as LoadLibraryW itself.
        g_unload_event = CreateEventW(nullptr, TRUE, FALSE,
            (L"Kopt_Unload_" + std::to_wstring(GetCurrentProcessId())).c_str());
        // As early as possible -- see read_share_token's doc comment: the
        // injector only holds its side of this mapping open for a few
        // seconds after inject, this has to happen well within that window.
        g_share_token = read_share_token();
        g_backend_endpoint = read_backend_endpoint();
        // Registered before everything else, including
        // SetUnhandledExceptionFilter below -- see fatal_exception_logger's
        // doc comment for why this exists as a second, earlier tripwire.
        g_fatal_exception_logger_handle = AddVectoredExceptionHandler(1, fatal_exception_logger);
        g_previous_exception_filter = SetUnhandledExceptionFilter(&unhandled_exception_filter);
        g_settings.load(g_settings_path);
        g_settings.normalize();
        g_custom_fov.store(g_settings.camera_fov, std::memory_order_relaxed);
        g_fov_override_active.store(g_settings.fov_override, std::memory_order_relaxed);
        install_game_exception_guard();
        sync_hotkeys();
        g_menu_open.store(g_settings.menu_open, std::memory_order_release);
        if (!wait_for_render_ready())
        {
            // Only reachable via KoptRequestUnload firing before the game
            // ever got this far -- not an error, just an early exit.
            log_line(L"Unload requested before render was ready; skipping hook install");
            remove_game_exception_guard();
            RemoveVectoredExceptionHandler(g_fatal_exception_logger_handle);
            SetUnhandledExceptionFilter(g_previous_exception_filter);
            if (g_unload_event != nullptr) CloseHandle(g_unload_event);
            CoUninitialize();
            FreeLibraryAndExitThread(g_module, 0);
        }
        if (!install_hooks())
        {
            log_line(L"DXGI hook installation failed; unloading payload");
            MessageBoxW(nullptr, L"DXGI hook installation failed.", L"KOPT Internal", MB_ICONERROR | MB_OK);
            remove_game_exception_guard();
            RemoveVectoredExceptionHandler(g_fatal_exception_logger_handle);
            SetUnhandledExceptionFilter(g_previous_exception_filter);
            if (g_unload_event != nullptr) CloseHandle(g_unload_event);
            CoUninitialize();
            FreeLibraryAndExitThread(g_module, 1);
        }
        log_line(L"DXGI hooks installed; waiting for Present");
        while (!g_stop.load())
        {
            // g_unload_event may be null if CreateEventW failed (extremely
            // unlikely) -- fall back to the plain poll so END/panic-hotkey
            // local sets of g_stop still work even without it.
            if (g_unload_event != nullptr && WaitForSingleObject(g_unload_event, 50) == WAIT_OBJECT_0)
                g_stop.store(true);
            else if (g_unload_event == nullptr)
                Sleep(50);
        }
        for (int attempt = 0; attempt < 25 && g_camera_slot != nullptr &&
            !g_unload_cleanup_completed.load(std::memory_order_acquire); ++attempt) Sleep(10);
        uninstall_hooks();
        while (g_active_callbacks.load() != 0) Sleep(1);
        g_publisher->stop();
        g_runtime.restore_transient_state();
        remove_game_exception_guard();
        RemoveVectoredExceptionHandler(g_fatal_exception_logger_handle);
        SetUnhandledExceptionFilter(g_previous_exception_filter);
        if (g_unload_event != nullptr) CloseHandle(g_unload_event);
        restore_settings_intent();
        g_settings.save(g_settings_path);
        CoUninitialize();
        FreeLibraryAndExitThread(g_module, 0);
    }
}

namespace
{
    // Both teardown paths (unload cleanup, panic) deliberately switch features
    // off in g_settings to stop them running. g_settings is also what gets
    // written to disk, so saving straight after either one persisted the
    // teardown as if the user had turned everything off themselves. Put the
    // user's actual intent back before the config is written.
    void restore_settings_intent()
    {
        if (g_panic_state.active)
        {
            g_settings.menu_open = g_panic_state.menu;
            g_settings.esp_enabled = g_panic_state.esp;
            g_settings.freecam = g_panic_state.freecam;
            g_settings.local_chams = g_panic_state.local_chams;
            g_settings.no_recoil = g_panic_state.no_recoil;
            g_settings.no_sway = g_panic_state.no_sway;
            g_panic_state.active = false;
        }
        if (g_unload_restore.valid)
        {
            g_settings.freecam = g_unload_restore.freecam;
            g_settings.local_chams = g_unload_restore.local_chams;
            g_settings.no_recoil = g_unload_restore.no_recoil;
            g_settings.no_sway = g_unload_restore.no_sway;
            g_unload_restore.valid = false;
        }
    }
}

extern "C" __declspec(dllexport) void __stdcall KoptRequestUnload()
{
    g_stop.store(true);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
        const HANDLE thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
        if (thread != nullptr) CloseHandle(thread);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        // When the game exits normally it tears down every other thread before
        // DllMain ever sees this, so the worker's own end-of-life save (reached
        // only via the END-key/injector unload path, after g_stop turns true)
        // never runs. Without this, settings changed in a session that ends by
        // just quitting ARK were silently lost. Safe here: plain file I/O, no
        // new threads, no other DLLs touched.
        if (!g_settings_path.empty())
        {
            restore_settings_intent();
            g_settings.save(g_settings_path);
        }
    }
    return TRUE;
}
