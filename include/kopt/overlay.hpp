#pragma once

#include "kopt/config.hpp"
#include "kopt/com_ptr.hpp"
#include "kopt/runtime.hpp"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <atomic>
#include <array>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace kopt
{
    struct InputState
    {
        std::atomic<int> mouse_x{};
        std::atomic<int> mouse_y{};
        std::atomic<int> wheel{};
        std::atomic<bool> left_down{};
        std::atomic<bool> left_pressed{};
        std::atomic<bool> right_pressed{};
        std::atomic<int> captured_key{};
        std::mutex character_mutex;
        std::wstring pending_characters;
        std::atomic<int> suppress_key_up{};
        std::atomic<bool> binding_capture{};
        std::atomic<bool> toggle_menu_requested{};
        std::atomic<bool> toggle_freecam_requested{};
        std::atomic<bool> toggle_esp_requested{};
        std::atomic<bool> toggle_panic_requested{};
        std::atomic<bool> diagnostics_bundle_requested{};

        int frame_mouse_x{};
        int frame_mouse_y{};
        int frame_wheel{};
        bool frame_left_down{};
        bool frame_left_pressed{};
        bool frame_right_pressed{};
        bool frame_click_consumed{};
        bool frame_right_click_consumed{};
        std::wstring frame_characters;

        void queue_character(const wchar_t character)
        {
            std::scoped_lock lock(character_mutex);
            if (pending_characters.size() < 64) pending_characters.push_back(character);
        }

        void begin_frame() noexcept
        {
            frame_mouse_x = mouse_x.load(std::memory_order_relaxed);
            frame_mouse_y = mouse_y.load(std::memory_order_relaxed);
            frame_wheel = wheel.exchange(0, std::memory_order_acq_rel);
            frame_left_down = left_down.load(std::memory_order_relaxed);
            frame_left_pressed = left_pressed.exchange(false, std::memory_order_acq_rel);
            frame_right_pressed = right_pressed.exchange(false, std::memory_order_acq_rel);
            {
                std::scoped_lock lock(character_mutex);
                frame_characters.swap(pending_characters);
                pending_characters.clear();
            }
            frame_click_consumed = false;
            frame_right_click_consumed = false;
        }

        void reset_pointer() noexcept
        {
            left_down.store(false, std::memory_order_relaxed);
            left_pressed.store(false, std::memory_order_relaxed);
            right_pressed.store(false, std::memory_order_relaxed);
            frame_left_down = false;
            frame_left_pressed = false;
            frame_right_pressed = false;
            frame_click_consumed = false;
            frame_right_click_consumed = false;
        }
    };

    class Overlay
    {
    public:
        bool initialize(IDXGISwapChain* swap_chain);
        void invalidate();
        void update_feature_hotkeys(Settings& settings);
        void render(IDXGISwapChain* swap_chain, Settings& settings, ArkRuntime& runtime,
            InputState& input, const std::filesystem::path& settings_path);

        // Оверлей не знает о kopt::Publisher и не должен -- рисование не
        // зависит от транспорта. payload.cpp зовёт это раз в кадр перед
        // render(), чтобы Diagnostics-вкладка могла показать статус
        // соединения без прямой связи с сетевым слоем.
        void set_share_connected(bool connected) noexcept { share_connected_ = connected; }

    private:
        bool share_connected_{};
        struct Rect { float left{}; float top{}; float right{}; float bottom{}; };
        struct Vertex { float x{}, y{}, u{}, v{}; std::uint32_t color{}; };
        enum class TextAlign { left, center, right };
        enum class BindingTarget { none, menu, unload, aim, dino_aim, feature, freecam, esp_toggle, panic };
        enum class PreviewDrag { none, label, health, torpor, status };

        bool ensure_device(IDXGISwapChain* swap_chain);
        bool create_pipeline();
        bool create_font_atlas();
        bool flush();
        void add_quad(const Rect& rect, float u0, float v0, float u1, float v1, const Color& color);
        void atlas_icon(const Rect& rect, int atlas_x, int atlas_y, int pixel_width, int pixel_height);
        void draw_esp(const Settings& settings, const ArkRuntime& runtime);
        void draw_aim_overlay(const Settings& settings, const ArkRuntime& runtime);
        void draw_radar(const Settings& settings, const ArkRuntime& runtime);
        void draw_alerts(const Settings& settings, const ArkRuntime& runtime);
        void draw_hotkey_list(const Settings& settings, const ArkRuntime& runtime);
        void draw_menu(Settings& settings, ArkRuntime& runtime, InputState& input,
            const std::filesystem::path& settings_path);
        void draw_command_palette(Settings& settings, InputState& input, const Rect& menu_frame);
        void draw_esp_preview(Settings& settings, float x, float y, InputState& input);
        void draw_debug(const ArkRuntime& runtime);
        void fill(const Rect& rect, const Color& color);
        void stroke(const Rect& rect, const Color& color, float width = 1.0F);
        void line(float x1, float y1, float x2, float y2, const Color& color, float width = 1.0F);
        void text(const std::wstring& value, const Rect& rect, const Color& color,
            float size = 14.0F, TextAlign alignment = TextAlign::left);
        bool button(const std::wstring& label, const Rect& rect, bool active, InputState& input);
        bool toggle(const std::wstring& label, bool& value, float x, float& y, InputState& input);
        bool checkbox(const std::wstring& label, bool& value, float x, float& y, InputState& input);
        void feature_binding_context(bool& value, const Rect& row, float x, float& y, InputState& input);
        bool slider(const std::wstring& label, float& value, float minimum, float maximum,
            float x, float& y, InputState& input, const wchar_t* suffix = L"");
        bool text_input(const std::wstring& label, std::wstring& value, int id,
            float x, float& y, InputState& input, std::size_t maximum = 256);
        bool combo(const std::wstring& label, std::int32_t& value, const wchar_t* const* options,
            std::size_t count, int id, float x, float& y, InputState& input);
        bool keybind(const std::wstring& label, std::uint32_t& value, BindingTarget target,
            float x, float& y, InputState& input);
        void process_binding_capture(Settings& settings, InputState& input);
        bool consume_click(const Rect& rect, InputState& input) const;
        bool consume_right_click(const Rect& rect, InputState& input) const;
        static bool contains(const Rect& rect, int x, int y);
        static std::wstring key_name(std::uint32_t key);
        static std::uint32_t pack(const Color& color);
        static std::size_t glyph_index(wchar_t character);

        ComPtr<ID3D11Device> device_;
        ComPtr<ID3D11DeviceContext> context_;
        ComPtr<ID3D11RenderTargetView> render_target_;
        ComPtr<ID3D11VertexShader> vertex_shader_;
        ComPtr<ID3D11PixelShader> pixel_shader_;
        ComPtr<ID3D11InputLayout> input_layout_;
        ComPtr<ID3D11Buffer> vertex_buffer_;
        ComPtr<ID3D11Buffer> screen_buffer_;
        ComPtr<ID3D11Texture2D> font_texture_;
        ComPtr<ID3D11ShaderResourceView> font_view_;
        ComPtr<ID3D11SamplerState> sampler_;
        ComPtr<ID3D11BlendState> blend_state_;
        ComPtr<ID3D11RasterizerState> rasterizer_state_;
        ComPtr<ID3D11DepthStencilState> depth_state_;
        std::vector<Vertex> vertices_;
        std::array<float, 352> glyph_advances_{};
        float width_{};
        float height_{};
        float last_overlay_build_ms_{};
        float last_overlay_flush_ms_{};
        std::size_t last_vertex_count_{};
        std::size_t vertex_capacity_{131072};
        int active_tab_{};
        int active_aim_section_{};
        int active_esp_section_{};
        int target_settings_page_{};
        int element_settings_page_{};
        int gear_settings_page_{};
        int radar_settings_page_{};
        int search_settings_page_{};
        int structure_catalog_page_{};
        int camera_settings_page_{};
        int alert_settings_page_{};
        int hotkey_page_{};
        int command_page_{};
        int loaded_layout_{-1};
        int relation_page_{};
        int color_target_{};
        int open_combo_{-1};
        int active_text_input_{-1};
        std::size_t active_slider_{};
        BindingTarget binding_target_{BindingTarget::none};
        std::wstring checkbox_binding_feature_id_;
        std::wstring binding_feature_id_;
        PreviewDrag preview_drag_{PreviewDrag::none};
        bool menu_position_initialized_{};
        bool menu_dragging_{};
        bool menu_resizing_{};
        float menu_left_{};
        float menu_top_{};
        float menu_drag_offset_x_{};
        float menu_drag_offset_y_{};
        float content_width_{510.0F};
        float current_menu_bottom_{};
        struct ComboPopup
        {
            bool visible{};
            const wchar_t* const* options{};
            std::size_t count{};
            std::int32_t selected{};
            Rect rect{};
        } combo_popup_;
        Rect active_combo_rect_{};
        Rect active_combo_control_rect_{};
        bool active_combo_rect_valid_{};
        bool preview_position_initialized_{};
        bool preview_window_dragging_{};
        bool preview_occluded_{};
        bool command_palette_open_{};
        bool aim_replay_live_{true};
        std::size_t aim_replay_index_{};
        float preview_left_{};
        float preview_top_{};
        float preview_drag_offset_x_{};
        float preview_drag_offset_y_{};
        std::wstring profile_name_{L"default"};
        std::size_t profile_index_{};
        std::wstring profile_delete_confirmation_;
        std::chrono::steady_clock::time_point profile_delete_confirmation_until_{};
        std::wstring structure_catalog_search_;
        std::wstring command_search_;
        struct StructureCatalogItem
        {
            std::wstring class_name;
            std::wstring display_name;
            int live_instances{};
        };
        std::vector<StructureCatalogItem> structure_catalog_;
        std::chrono::steady_clock::time_point structure_catalog_refresh_at_{};
        Settings* settings_context_{};
        struct FeatureBindingRuntime
        {
            std::wstring id;
            std::uint32_t key{};
            std::int32_t mode{};
            bool was_down{};
            bool armed{};
            bool hold_applied{};
            bool restore_value{};
            bool active{};
        };
        std::vector<FeatureBindingRuntime> feature_binding_runtime_;
        std::wstring toast_;
        std::chrono::steady_clock::time_point toast_until_{};
    };
}
