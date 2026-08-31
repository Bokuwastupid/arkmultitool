#include "kopt/config.hpp"
#include "kopt/com_ptr.hpp"
#include "kopt/overlay.hpp"
#include "kopt/runtime.hpp"

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
#include <mutex>
#include <string>
#include <thread>

using kopt::ComPtr;

namespace
{
    using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
    using ResizeBuffersFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    using CameraUpdateFn = void(__fastcall*)(void*, float);
    using DrawIndexedFn = void(__stdcall*)(ID3D11DeviceContext*, UINT, UINT, INT);
    using DrawIndexedInstancedFn = void(__stdcall*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);

    HMODULE g_module{};
    kopt::Settings g_settings;
    kopt::ArkRuntime g_runtime;
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
    CameraUpdateFn g_original_camera_update{};
    DrawIndexedFn g_original_draw_indexed{};
    DrawIndexedInstancedFn g_original_draw_indexed_instanced{};
    HWND g_game_window{};
    IDXGISwapChain* g_game_swap_chain{};
    WNDPROC g_original_wndproc{};
    std::atomic<bool> g_stop{};
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
    std::atomic<int> g_local_chams_draw_mode{-1};
    std::atomic<std::uint32_t> g_local_chams_color{0xA33DFFFFU};
    std::atomic<int> g_freecam_wheel{};
    std::atomic<bool> g_unload_cleanup_requested{};
    std::atomic<bool> g_unload_cleanup_completed{};
    bool g_menu_input_active{};
    std::atomic<bool> g_polled_left_down{};
    std::atomic<bool> g_menu_pointer_armed{};
    std::uint64_t g_logged_world_generation{};
    std::uint32_t g_logged_skeleton_guard_hits{};
    bool g_logged_local_valid{};
    bool g_logged_aim_active{};
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
    LPTOP_LEVEL_EXCEPTION_FILTER g_previous_exception_filter{};
    std::uintptr_t g_game_module_base{};
    alignas(8) std::uint64_t g_none_fname{};
    std::atomic<std::uint32_t> g_skeleton_guard_hits{};
    struct PanicState
    {
        bool active{};
        bool menu{};
        bool esp{};
        bool freecam{};
        bool local_chams{};
        bool enemy_chams{};
        bool no_recoil{};
        bool no_sway{};
    } g_panic_state;

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
            const std::wstring payload = std::format(
                L"KOPT diagnostics\r\nKind={}\r\nPID={}\r\nWorldGeneration={}\r\n"
                L"LocalValid={}\r\nAimActive={}\r\nMenuOpen={}\r\nSkeletonGuardHits={}\r\n",
                crash ? L"crash" : L"manual", GetCurrentProcessId(), g_logged_world_generation,
                g_logged_local_valid, g_logged_aim_active, g_menu_open.load(std::memory_order_relaxed),
                g_skeleton_guard_hits.load(std::memory_order_relaxed));
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
        if (context == nullptr) return false;
        ID3D11Device* raw_device{};
        context->GetDevice(&raw_device);
        ComPtr<ID3D11Device> device(raw_device);
        if (device == nullptr) return false;
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
            if (FAILED(D3DCompile(source_code, sizeof(source_code) - 1, nullptr, nullptr, nullptr,
                "main", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, shader.put(), errors.put())) ||
                FAILED(device->CreatePixelShader(shader->GetBufferPointer(), shader->GetBufferSize(),
                    nullptr, g_chams_pixel_shader.put()))) return false;
            D3D11_BUFFER_DESC buffer{};
            buffer.ByteWidth = 16;
            buffer.Usage = D3D11_USAGE_DEFAULT;
            buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            if (FAILED(device->CreateBuffer(&buffer, nullptr, g_chams_color_buffer.put()))) return false;
            D3D11_BLEND_DESC blend{};
            blend.RenderTarget[0].BlendEnable = TRUE;
            blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
            blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
            blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            if (FAILED(device->CreateBlendState(&blend, g_chams_blend_state.put()))) return false;
        }
        D3D11_RASTERIZER_DESC solid = source;
        solid.FillMode = D3D11_FILL_SOLID;
        if (!g_chams_rasterizer_valid ||
            std::memcmp(&solid, &g_chams_rasterizer_description, sizeof(solid)) != 0)
        {
            g_chams_solid_rasterizer.reset();
            if (FAILED(device->CreateRasterizerState(&solid, g_chams_solid_rasterizer.put()))) return false;
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
        if (render_target == nullptr || !ensure_solid_chams_pipeline(context, rasterizer_description))
        {
            draw();
            finish();
            return;
        }
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

    void ensure_draw_indexed_hook(IDXGISwapChain* swap_chain)
    {
        if ((g_draw_indexed_slot != nullptr && g_draw_indexed_instanced_slot != nullptr) ||
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
    }

    void __fastcall hooked_camera_update(void* manager, const float delta_seconds)
    {
        g_active_callbacks.fetch_add(1, std::memory_order_acq_rel);
        g_original_camera_update(manager, delta_seconds);
        if (!g_stop.load(std::memory_order_acquire))
        {
            std::scoped_lock lock(g_state_mutex);
            const int wheel_steps = g_freecam_wheel.exchange(0, std::memory_order_acq_rel);
            if (wheel_steps != 0)
            {
                g_settings.freecam_speed *= std::pow(1.15F, static_cast<float>(wheel_steps));
                g_settings.freecam_speed = std::clamp(g_settings.freecam_speed, 100.0F, 10000.0F);
            }
            g_runtime.on_game_camera_tick(g_settings, reinterpret_cast<std::uintptr_t>(manager), delta_seconds);
        }
        else if (!g_unload_cleanup_requested.exchange(true, std::memory_order_acq_rel))
        {
            std::scoped_lock lock(g_state_mutex);
            g_settings.freecam = false;
            g_settings.local_chams = false;
            g_settings.enemy_chams = false;
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

    void sync_hotkeys()
    {
        g_menu_key.store(g_settings.menu_key, std::memory_order_release);
        g_unload_key.store(g_settings.unload_key, std::memory_order_release);
        g_freecam_key.store(g_settings.freecam_key, std::memory_order_release);
        g_esp_toggle_key.store(g_settings.esp_toggle_key, std::memory_order_release);
        g_freecam_active.store(g_settings.freecam, std::memory_order_release);
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
        if (down && !was_down) g_input.left_pressed.store(true, std::memory_order_release);
        g_input.left_down.store(down, std::memory_order_release);
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
        }
    }

    bool capture_binding(const int key, const bool suppress_key_up)
    {
        if (!g_input.binding_capture.exchange(false, std::memory_order_acq_rel)) return false;
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
                ensure_draw_indexed_hook(swap_chain);
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
                            g_settings.freecam, g_settings.local_chams, g_settings.enemy_chams,
                            g_settings.no_recoil, g_settings.no_sway};
                        g_settings.menu_open = false;
                        g_settings.esp_enabled = false;
                        g_settings.freecam = false;
                        g_settings.local_chams = false;
                        g_settings.enemy_chams = false;
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
                        g_settings.enemy_chams = g_panic_state.enemy_chams;
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
                g_overlay.update_feature_hotkeys(g_settings);
                g_runtime.update(g_settings, delta_seconds);
                const auto& snapshot = g_runtime.snapshot();
                if (snapshot.world_generation != g_logged_world_generation)
                {
                    g_logged_world_generation = snapshot.world_generation;
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
                const std::uint32_t guard_hits = g_skeleton_guard_hits.load(std::memory_order_relaxed);
                if (guard_hits != g_logged_skeleton_guard_hits)
                {
                    g_logged_skeleton_guard_hits = guard_hits;
                    log_line(std::format(L"Flight-death skeleton guard recovered {} invalid montage slot reference(s)",
                        guard_hits));
                }
                ensure_camera_hook();
                g_overlay.render(swap_chain, g_settings, g_runtime, g_input, g_settings_path);
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
        set_menu_input_mode(false);
        restore_slot(g_camera_slot, reinterpret_cast<void*>(g_original_camera_update));
        g_camera_slot = nullptr;
        g_original_camera_update = nullptr;
        restore_slot(g_draw_indexed_slot, reinterpret_cast<void*>(g_original_draw_indexed));
        g_draw_indexed_slot = nullptr;
        restore_slot(g_draw_indexed_instanced_slot,
            reinterpret_cast<void*>(g_original_draw_indexed_instanced));
        g_draw_indexed_instanced_slot = nullptr;
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

    DWORD WINAPI worker(void*)
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const auto directory = module_directory();
        g_settings_path = directory / L"kopt_internal.ini";
        g_log_path = directory / L"kopt_internal.log";
        log_line(L"Payload worker started");
        g_previous_exception_filter = SetUnhandledExceptionFilter(&unhandled_exception_filter);
        g_settings.load(g_settings_path);
        install_game_exception_guard();
        sync_hotkeys();
        g_menu_open.store(g_settings.menu_open, std::memory_order_release);
        if (!install_hooks())
        {
            log_line(L"DXGI hook installation failed; unloading payload");
            MessageBoxW(nullptr, L"DXGI hook installation failed.", L"KOPT Internal", MB_ICONERROR | MB_OK);
            remove_game_exception_guard();
            SetUnhandledExceptionFilter(g_previous_exception_filter);
            CoUninitialize();
            FreeLibraryAndExitThread(g_module, 1);
        }
        log_line(L"DXGI hooks installed; waiting for Present");
        while (!g_stop.load()) Sleep(50);
        for (int attempt = 0; attempt < 25 && g_camera_slot != nullptr &&
            !g_unload_cleanup_completed.load(std::memory_order_acquire); ++attempt) Sleep(10);
        uninstall_hooks();
        while (g_active_callbacks.load() != 0) Sleep(1);
        g_runtime.restore_transient_state();
        remove_game_exception_guard();
        SetUnhandledExceptionFilter(g_previous_exception_filter);
        g_settings.save(g_settings_path);
        CoUninitialize();
        FreeLibraryAndExitThread(g_module, 0);
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
    return TRUE;
}
