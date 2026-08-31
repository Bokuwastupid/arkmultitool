#include "kopt/overlay.hpp"
#include "kopt/armor_icon_assets.generated.h"
#include "kopt/weapon_icon_assets.generated.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <d3dcompiler.h>
#include <functional>
#include <iomanip>
#include <iterator>
#include <limits>
#include <numbers>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace
{
    constexpr kopt::Color panel{0.035F, 0.027F, 0.051F, 0.98F};
    constexpr kopt::Color surface{0.071F, 0.055F, 0.102F, 0.98F};
    constexpr kopt::Color surface_hover{0.118F, 0.082F, 0.173F, 1.0F};
    kopt::Color accent{0.545F, 0.361F, 0.965F, 1.0F};
    kopt::Color accent_dim{0.290F, 0.184F, 0.486F, 1.0F};
    constexpr kopt::Color text_primary{0.945F, 0.929F, 0.973F, 1.0F};
    constexpr kopt::Color text_secondary{0.655F, 0.608F, 0.733F, 1.0F};
    constexpr kopt::Color success{0.29F, 0.90F, 0.62F, 1.0F};
    constexpr kopt::Color warning{1.0F, 0.72F, 0.30F, 1.0F};
    constexpr int atlas_width = 1024;
    constexpr int atlas_height = 512;
    constexpr int glyph_cell_width = 28;
    constexpr int glyph_cell_height = 36;
    constexpr int glyph_columns = 32;

    float distance3(const kopt::Vec3& a, const kopt::Vec3& b)
    {
        const float x = a.x - b.x;
        const float y = a.y - b.y;
        const float z = a.z - b.z;
        return std::sqrt(x * x + y * y + z * z);
    }

    std::wstring lower_copy(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
        return value;
    }

    std::wstring safe_profile_name(std::wstring value)
    {
        std::wstring safe;
        safe.reserve(std::min<std::size_t>(value.size(), 32));
        for (const wchar_t character : value)
        {
            if (safe.size() >= 32) break;
            if (std::iswalnum(character) != 0 || character == L'_' || character == L'-')
                safe.push_back(character);
            else if ((character == L' ' || character == L'\t') &&
                !safe.empty() && safe.back() != L'_') safe.push_back(L'_');
        }
        while (!safe.empty() && safe.back() == L'_') safe.pop_back();
        return safe;
    }

    struct FeatureDescriptor
    {
        const wchar_t* id;
        const wchar_t* label;
        const wchar_t* category;
        bool kopt::Settings::* member;
    };

    const std::vector<FeatureDescriptor>& feature_catalog()
    {
        using S = kopt::Settings;
        static const std::vector<FeatureDescriptor> catalog{
            {L"aim.player", L"Player aim", L"Aim", &S::player_aim},
            {L"aim.dino", L"Dino aim", L"Aim", &S::dino_aim},
            {L"aim.enemies", L"Target enemies", L"Aim", &S::aim_target_enemies},
            {L"aim.allies", L"Target allies", L"Aim", &S::aim_target_allies},
            {L"aim.visibility", L"Visible targets only", L"Aim", &S::visibility_check},
            {L"aim.lock", L"Lock selected target", L"Aim", &S::aim_lock},
            {L"aim.fov_circle", L"Draw aim FOV", L"Aim", &S::aim_draw_fov},
            {L"aim.prediction", L"Projectile prediction", L"Aim", &S::aim_prediction},
            {L"aim.intercept", L"Moving-target intercept solver", L"Aim", &S::aim_intercept_solver},
            {L"esp.master", L"Master ESP", L"ESP", &S::esp_enabled},
            {L"esp.players", L"Players", L"ESP", &S::player_esp},
            {L"esp.dinos_tamed", L"Enemy / tamed dinosaurs", L"ESP", &S::enemy_dino_esp},
            {L"esp.dinos_wild", L"Wild dinosaurs", L"ESP", &S::wild_dino_esp},
            {L"esp.structures", L"Structures", L"ESP", &S::structure_esp},
            {L"esp.turrets", L"Turrets", L"ESP", &S::turret_esp},
            {L"esp.drops", L"Ground drops", L"ESP", &S::drop_esp},
            {L"esp.death_caches", L"Death caches", L"ESP", &S::death_cache_esp},
            {L"esp.player_caches", L"Player item caches", L"ESP", &S::player_item_cache_esp},
            {L"esp.dino_caches", L"Dino item caches", L"ESP", &S::dino_item_cache_esp},
            {L"esp.battle", L"Battle Mode", L"ESP", &S::battle_mode},
            {L"esp.own_players", L"Own players", L"ESP relations", &S::esp_own_players},
            {L"esp.allied_players", L"Allied players", L"ESP relations", &S::esp_allied_players},
            {L"esp.enemy_players", L"Enemy players", L"ESP relations", &S::esp_enemy_players},
            {L"esp.own_dinos", L"Own dinos", L"ESP relations", &S::esp_own_dinos},
            {L"esp.allied_dinos", L"Allied dinos", L"ESP relations", &S::esp_allied_dinos},
            {L"esp.enemy_dinos", L"Enemy dinos", L"ESP relations", &S::esp_enemy_dinos},
            {L"esp.own_structures", L"Own structures", L"ESP relations", &S::esp_own_structures},
            {L"esp.allied_structures", L"Allied structures", L"ESP relations", &S::esp_allied_structures},
            {L"esp.enemy_structures", L"Enemy structures", L"ESP relations", &S::esp_enemy_structures},
            {L"esp.neutral_structures", L"Neutral / world structures", L"ESP relations", &S::esp_neutral_structures},
            {L"esp.awake", L"Show awake players", L"ESP states", &S::show_awake_players},
            {L"esp.sleeping", L"Show sleeping players", L"ESP states", &S::show_sleeping_players},
            {L"esp.knocked", L"Show knocked-out players", L"ESP states", &S::show_knocked_out_players},
            {L"esp.dead", L"Show dead players", L"ESP states", &S::show_dead_players},
            {L"esp.names", L"Names", L"ESP elements", &S::show_names},
            {L"esp.tribes", L"Tribe labels", L"ESP elements", &S::show_tribes},
            {L"esp.distance", L"Distance labels", L"ESP elements", &S::show_distance},
            {L"esp.health", L"Health bars", L"ESP elements", &S::show_health},
            {L"esp.torpor", L"Torpor bars", L"ESP elements", &S::show_torpor},
            {L"esp.vitals", L"HP / Torpor values", L"ESP elements", &S::show_vital_values},
            {L"esp.boxes", L"2D boxes", L"ESP elements", &S::show_boxes},
            {L"esp.skeleton", L"Player skeleton", L"ESP elements", &S::show_skeleton},
            {L"esp.tracers", L"Tracers", L"ESP elements", &S::show_tracers},
            {L"esp.offscreen", L"Off-screen markers", L"ESP elements", &S::offscreen_arrows},
            {L"esp.summary_structures", L"Structure summary", L"ESP summaries", &S::show_structure_summary},
            {L"esp.summary_players", L"Player summary", L"ESP summaries", &S::show_player_summary},
            {L"esp.summary_dinos", L"Tamed dino summary", L"ESP summaries", &S::show_dino_summary},
            {L"esp.summary_filters", L"Summaries use ESP filters", L"ESP summaries", &S::summary_uses_filters},
            {L"esp.player_labels", L"Player labels", L"ESP player", &S::show_player_labels},
            {L"esp.player_status", L"Player status badge", L"ESP player", &S::show_player_status},
            {L"esp.player_occlusion_color", L"Separate occluded player color", L"ESP player",
                &S::player_occluded_color_enabled},
            {L"esp.player_health", L"Player health", L"ESP player", &S::show_player_health},
            {L"esp.player_torpor", L"Player torpor", L"ESP player", &S::show_player_torpor},
            {L"esp.dino_labels", L"Dino labels", L"ESP world", &S::show_dino_labels},
            {L"esp.dino_skeleton", L"Dino skeleton silhouettes", L"ESP world", &S::show_dino_skeleton},
            {L"esp.dino_health", L"Dino health", L"ESP world", &S::show_dino_health},
            {L"esp.dino_torpor", L"Dino torpor", L"ESP world", &S::show_dino_torpor},
            {L"esp.structure_labels", L"Structure labels", L"ESP world", &S::show_structure_labels},
            {L"esp.structure_health", L"Structure health", L"ESP world", &S::show_structure_health},
            {L"esp.held_items", L"Held weapon icons", L"ESP gear", &S::show_held_items},
            {L"esp.equipment", L"Armor icons + durability", L"ESP gear", &S::show_equipment},
            {L"esp.compact", L"Compact multi-data labels", L"ESP gear", &S::compact_labels},
            {L"esp.drop_quantity", L"Dropped item quantities", L"ESP gear", &S::show_drop_quantity},
            {L"esp.turret_details", L"Turret ammo / state / settings", L"Turrets", &S::show_turret_details},
            {L"esp.turret_ammo", L"Turret ammo", L"Turrets", &S::turret_show_ammo},
            {L"esp.turret_state", L"Turret state", L"Turrets", &S::turret_show_state},
            {L"esp.turret_power", L"Turret power", L"Turrets", &S::turret_show_power},
            {L"esp.turret_range", L"Turret range", L"Turrets", &S::turret_show_range},
            {L"esp.turret_target_mode", L"Turret target mode", L"Turrets", &S::turret_show_target_mode},
            {L"esp.turret_target_lock", L"Turret target lock", L"Turrets", &S::turret_show_target_state},
            {L"esp.turret_warning", L"Turret warning mode", L"Turrets", &S::turret_show_warning},
            {L"esp.turret_filter", L"Hide non-matching turrets", L"Turrets", &S::turret_hide_nonmatching},
            {L"esp.radar", L"Radar", L"Radar", &S::show_radar},
            {L"esp.threat", L"Threat panel", L"Radar", &S::show_threat_panel},
            {L"esp.grouping", L"Group dense structures", L"Radar", &S::structure_grouping},
            {L"esp.structure_whitelist", L"Use selected structure list", L"Structures", &S::structure_whitelist_enabled},
            {L"esp.declutter", L"Smart declutter", L"Radar", &S::smart_declutter},
            {L"camera.freecam", L"Free camera", L"Camera", &S::freecam},
            {L"camera.fov", L"FOV override", L"Camera", &S::fov_override},
            {L"weapon.recoil", L"No recoil", L"Weapon", &S::no_recoil},
            {L"weapon.sway", L"No weapon sway", L"Weapon", &S::no_sway},
            {L"chams.local", L"First-person hands + weapon", L"Chams", &S::local_chams},
            {L"alerts.master", L"Enable alerts", L"Alerts", &S::alerts_enabled},
            {L"alerts.new_player", L"New enemy player", L"Alerts", &S::alert_new_player},
            {L"alerts.approach", L"Enemy approaching", L"Alerts", &S::alert_approach},
            {L"alerts.sleep", L"Enemy sleep transition", L"Alerts", &S::alert_sleep},
            {L"alerts.death", L"Enemy death transition", L"Alerts", &S::alert_death},
            {L"alerts.noglin", L"Noglin in radius", L"Alerts", &S::alert_noglin},
            {L"alerts.turret", L"Active targeting turret", L"Alerts", &S::alert_turret},
            {L"alerts.group", L"Enemy group (3+)", L"Alerts", &S::alert_enemy_group},
            {L"alerts.sound", L"Notification sound", L"Alerts", &S::alert_sound},
            {L"runtime.debug", L"Runtime debug panel", L"Runtime", &S::debug_panel},
            {L"hotkeys.list", L"Show active bind list", L"Hotkeys", &S::show_hotkey_list}
        };
        return catalog;
    }

    const FeatureDescriptor* feature_descriptor(kopt::Settings& settings, const bool& value)
    {
        for (const FeatureDescriptor& descriptor : feature_catalog())
            if (&(settings.*descriptor.member) == &value) return &descriptor;
        return nullptr;
    }

    const FeatureDescriptor* feature_descriptor(const std::wstring& id)
    {
        const auto& catalog = feature_catalog();
        const auto found = std::find_if(catalog.begin(), catalog.end(), [&](const auto& descriptor) {
            return id == descriptor.id;
        });
        return found == catalog.end() ? nullptr : &*found;
    }

    bool token_list_contains(const std::wstring& list, const std::wstring& candidate)
    {
        if (list.empty() || candidate.empty()) return false;
        const std::wstring normalized_candidate = lower_copy(candidate);
        std::size_t start{};
        while (start <= list.size())
        {
            const std::size_t end = list.find_first_of(L";,\r\n", start);
            std::wstring token = lower_copy(list.substr(start,
                end == std::wstring::npos ? list.size() - start : end - start));
            const auto first = token.find_first_not_of(L" \t");
            const auto last = token.find_last_not_of(L" \t");
            if (first != std::wstring::npos)
            {
                token = token.substr(first, last - first + 1);
                if (!token.empty() && normalized_candidate.find(token) != std::wstring::npos) return true;
            }
            if (end == std::wstring::npos) break;
            start = end + 1;
        }
        return false;
    }

    std::vector<std::wstring> exact_tokens(const std::wstring& list)
    {
        std::vector<std::wstring> values;
        std::size_t start{};
        while (start <= list.size())
        {
            const std::size_t end = list.find(L';', start);
            std::wstring value = list.substr(start,
                end == std::wstring::npos ? list.size() - start : end - start);
            const auto first = value.find_first_not_of(L" \t\r\n");
            const auto last = value.find_last_not_of(L" \t\r\n");
            if (first != std::wstring::npos)
            {
                value = value.substr(first, last - first + 1);
                const std::wstring lowered = lower_copy(value);
                const bool duplicate = std::any_of(values.begin(), values.end(), [&](const auto& existing) {
                    return lower_copy(existing) == lowered;
                });
                if (!duplicate) values.push_back(std::move(value));
            }
            if (end == std::wstring::npos) break;
            start = end + 1;
        }
        return values;
    }

    bool exact_token_contains(const std::wstring& list, const std::wstring& value)
    {
        const std::wstring lowered = lower_copy(value);
        const auto values = exact_tokens(list);
        return std::any_of(values.begin(), values.end(), [&](const auto& token) {
            return lower_copy(token) == lowered;
        });
    }

    void set_exact_token(std::wstring& list, const std::wstring& value, const bool present)
    {
        std::vector<std::wstring> values = exact_tokens(list);
        const std::wstring lowered = lower_copy(value);
        std::erase_if(values, [&](const auto& token) { return lower_copy(token) == lowered; });
        if (present && !value.empty()) values.push_back(value);
        std::sort(values.begin(), values.end(), [](const auto& left, const auto& right) {
            return lower_copy(left) < lower_copy(right);
        });
        list.clear();
        for (const auto& token : values)
        {
            if (!list.empty()) list += L';';
            list += token;
        }
    }

    std::wstring pretty_structure_class(std::wstring value)
    {
        if (value.ends_with(L"_C")) value.resize(value.size() - 2);
        static constexpr std::array<std::wstring_view, 5> prefixes{
            L"PrimalStructure", L"Structure_", L"Structure", L"BP_", L"PrimalItemStructure_"};
        for (const auto prefix : prefixes)
        {
            if (value.starts_with(prefix))
            {
                value.erase(0, prefix.size());
                break;
            }
        }
        std::wstring result;
        result.reserve(value.size() + 8);
        for (std::size_t index = 0; index < value.size(); ++index)
        {
            const wchar_t character = value[index];
            if (character == L'_')
            {
                if (!result.empty() && result.back() != L' ') result.push_back(L' ');
                continue;
            }
            if (index > 0 && std::iswupper(character) != 0 &&
                std::iswlower(value[index - 1]) != 0 && !result.empty() && result.back() != L' ')
                result.push_back(L' ');
            result.push_back(character);
        }
        while (!result.empty() && result.front() == L' ') result.erase(result.begin());
        while (!result.empty() && result.back() == L' ') result.pop_back();
        return result.empty() ? value : result;
    }

    std::wstring fixed(const float value, const int precision = 0)
    {
        std::wostringstream stream;
        stream << std::fixed << std::setprecision(precision) << value;
        return stream.str();
    }

    enum class PlayerEspState
    {
        awake,
        sleeping,
        knocked_out,
        dead
    };

    PlayerEspState player_esp_state(const kopt::Actor& actor)
    {
        if (kopt::actor_is_dead(actor)) return PlayerEspState::dead;
        const float torpor_ratio = actor.max_torpor > 0.0F ?
            std::clamp(actor.torpor / actor.max_torpor, 0.0F, 1.0F) : 0.0F;
        if (torpor_ratio >= 0.95F) return PlayerEspState::knocked_out;
        if (actor.sleeping) return PlayerEspState::sleeping;
        return PlayerEspState::awake;
    }

    bool player_recently_rendered(const kopt::Snapshot& snapshot, const kopt::Actor& actor,
        const float grace_ms)
    {
        if (snapshot.world_time <= 0.0 || actor.last_render_time <= 0.0) return false;
        const double age = snapshot.world_time - actor.last_render_time;
        return std::isfinite(age) && age >= 0.0 && age <= static_cast<double>(grace_ms) * 0.001;
    }

    bool player_state_enabled(const kopt::Settings& settings, const PlayerEspState state)
    {
        switch (state)
        {
        case PlayerEspState::awake: return settings.show_awake_players;
        case PlayerEspState::sleeping: return settings.show_sleeping_players;
        case PlayerEspState::knocked_out: return settings.show_knocked_out_players;
        case PlayerEspState::dead: return settings.show_dead_players;
        }
        return true;
    }

    const wchar_t* player_state_label(const PlayerEspState state)
    {
        switch (state)
        {
        case PlayerEspState::awake: return L"ALIVE / AWAKE";
        case PlayerEspState::sleeping: return L"ALIVE / SLEEPING";
        case PlayerEspState::knocked_out: return L"ALIVE / KNOCKED OUT";
        case PlayerEspState::dead: return L"DEAD";
        }
        return L"UNKNOWN";
    }

    kopt::Color player_state_color(const kopt::Settings& settings, const PlayerEspState state)
    {
        switch (state)
        {
        case PlayerEspState::awake: return settings.player_awake_color;
        case PlayerEspState::sleeping: return settings.player_sleeping_color;
        case PlayerEspState::knocked_out: return settings.player_knocked_out_color;
        case PlayerEspState::dead: return settings.player_dead_color;
        }
        return settings.player_awake_color;
    }
}

namespace kopt
{
    bool Overlay::initialize(IDXGISwapChain* swap_chain)
    {
        return ensure_device(swap_chain);
    }

    void Overlay::invalidate()
    {
        render_target_.reset();
        width_ = 0.0F;
        height_ = 0.0F;
    }

    bool Overlay::ensure_device(IDXGISwapChain* swap_chain)
    {
        if (swap_chain == nullptr) return false;
        ComPtr<ID3D11Device> incoming;
        if (FAILED(swap_chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(incoming.put())))) return false;
        if (device_.get() != incoming.get())
        {
            invalidate();
            depth_state_.reset();
            rasterizer_state_.reset();
            blend_state_.reset();
            sampler_.reset();
            font_view_.reset();
            font_texture_.reset();
            screen_buffer_.reset();
            vertex_buffer_.reset();
            input_layout_.reset();
            pixel_shader_.reset();
            vertex_shader_.reset();
            context_.reset();
            device_ = std::move(incoming);
            device_->GetImmediateContext(context_.put());
            if (context_ == nullptr || !create_pipeline() || !create_font_atlas()) return false;
        }
        if (render_target_ != nullptr) return true;
        ComPtr<ID3D11Texture2D> back_buffer;
        if (FAILED(swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(back_buffer.put())))) return false;
        D3D11_TEXTURE2D_DESC description{};
        back_buffer->GetDesc(&description);
        width_ = static_cast<float>(description.Width);
        height_ = static_cast<float>(description.Height);
        if (FAILED(device_->CreateRenderTargetView(back_buffer.get(), nullptr, render_target_.put()))) return false;
        return true;
    }

    bool Overlay::create_pipeline()
    {
        static constexpr char vertex_source[] =
            "cbuffer Screen : register(b0) { float2 screen; float2 pad; };"
            "struct VIn { float2 pos:POSITION; float2 uv:TEXCOORD0; float4 color:COLOR0; };"
            "struct VOut { float4 pos:SV_POSITION; float2 uv:TEXCOORD0; float4 color:COLOR0; };"
            "VOut main(VIn i){ VOut o; o.pos=float4(i.pos.x/screen.x*2-1,1-i.pos.y/screen.y*2,0,1);"
            "o.uv=i.uv; o.color=i.color; return o; }";
        static constexpr char pixel_source[] =
            "Texture2D atlas:register(t0); SamplerState samp:register(s0);"
            "struct PIn { float4 pos:SV_POSITION; float2 uv:TEXCOORD0; float4 color:COLOR0; };"
            "float4 main(PIn i):SV_TARGET { float4 tex=atlas.Sample(samp,i.uv);"
            "return float4(i.color.rgb*tex.rgb, i.color.a*tex.a); }";
        ComPtr<ID3DBlob> vertex_blob;
        ComPtr<ID3DBlob> pixel_blob;
        ComPtr<ID3DBlob> errors;
        if (FAILED(D3DCompile(vertex_source, sizeof(vertex_source) - 1, nullptr, nullptr, nullptr,
                "main", "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, vertex_blob.put(), errors.put()))) return false;
        errors.reset();
        if (FAILED(D3DCompile(pixel_source, sizeof(pixel_source) - 1, nullptr, nullptr, nullptr,
                "main", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, pixel_blob.put(), errors.put()))) return false;
        if (FAILED(device_->CreateVertexShader(vertex_blob->GetBufferPointer(), vertex_blob->GetBufferSize(),
                nullptr, vertex_shader_.put())) ||
            FAILED(device_->CreatePixelShader(pixel_blob->GetBufferPointer(), pixel_blob->GetBufferSize(),
                nullptr, pixel_shader_.put()))) return false;
        const D3D11_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0}
        };
        if (FAILED(device_->CreateInputLayout(layout, static_cast<UINT>(std::size(layout)),
                vertex_blob->GetBufferPointer(), vertex_blob->GetBufferSize(), input_layout_.put()))) return false;

        D3D11_BUFFER_DESC vertex_desc{};
        vertex_desc.ByteWidth = static_cast<UINT>(vertex_capacity_ * sizeof(Vertex));
        vertex_desc.Usage = D3D11_USAGE_DYNAMIC;
        vertex_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vertex_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device_->CreateBuffer(&vertex_desc, nullptr, vertex_buffer_.put()))) return false;
        D3D11_BUFFER_DESC screen_desc{};
        screen_desc.ByteWidth = 16;
        screen_desc.Usage = D3D11_USAGE_DEFAULT;
        screen_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(device_->CreateBuffer(&screen_desc, nullptr, screen_buffer_.put()))) return false;

        D3D11_SAMPLER_DESC sampler_desc{};
        sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device_->CreateSamplerState(&sampler_desc, sampler_.put()))) return false;
        D3D11_BLEND_DESC blend_desc{};
        auto& blend = blend_desc.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        blend.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = D3D11_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D11_BLEND_ONE;
        blend.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blend.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device_->CreateBlendState(&blend_desc, blend_state_.put()))) return false;
        D3D11_RASTERIZER_DESC raster_desc{};
        raster_desc.FillMode = D3D11_FILL_SOLID;
        raster_desc.CullMode = D3D11_CULL_NONE;
        raster_desc.DepthClipEnable = TRUE;
        if (FAILED(device_->CreateRasterizerState(&raster_desc, rasterizer_state_.put()))) return false;
        D3D11_DEPTH_STENCIL_DESC depth_desc{};
        depth_desc.DepthEnable = FALSE;
        depth_desc.StencilEnable = FALSE;
        return SUCCEEDED(device_->CreateDepthStencilState(&depth_desc, depth_state_.put()));
    }

    bool Overlay::create_font_atlas()
    {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = atlas_width;
        info.bmiHeader.biHeight = -atlas_height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* raw_pixels{};
        const HDC dc = CreateCompatibleDC(nullptr);
        const HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &raw_pixels, nullptr, 0);
        if (dc == nullptr || bitmap == nullptr || raw_pixels == nullptr)
        {
            if (bitmap) DeleteObject(bitmap);
            if (dc) DeleteDC(dc);
            return false;
        }
        std::memset(raw_pixels, 0, atlas_width * atlas_height * 4);
        const HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
        const HFONT font = CreateFontW(-22, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
            VARIABLE_PITCH | FF_SWISS, L"Segoe UI Variable");
        const HGDIOBJ old_font = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        glyph_advances_.fill(12.0F);
        for (std::size_t index = 1; index < glyph_advances_.size(); ++index)
        {
            const wchar_t character = index <= 95 ? static_cast<wchar_t>(31 + index) :
                static_cast<wchar_t>(0x400 + index - 96);
            const int x = static_cast<int>(index % glyph_columns) * glyph_cell_width + 2;
            const int y = static_cast<int>(index / glyph_columns) * glyph_cell_height + 3;
            TextOutW(dc, x, y, &character, 1);
            SIZE extent{};
            if (GetTextExtentPoint32W(dc, &character, 1, &extent))
                glyph_advances_[index] = static_cast<float>(std::clamp(static_cast<int>(extent.cx) + 1, 6, glyph_cell_width - 2));
        }
        auto* pixels = static_cast<std::uint8_t*>(raw_pixels);
        for (int i = 0; i < atlas_width * atlas_height; ++i)
        {
            const std::uint8_t alpha = std::max({pixels[i*4], pixels[i*4+1], pixels[i*4+2]});
            pixels[i*4] = pixels[i*4+1] = pixels[i*4+2] = 255;
            pixels[i*4+3] = alpha;
        }
        const auto copy_rgba = [&](const std::uint8_t* source, const int source_width, const int source_height,
            const int destination_x, const int destination_y) {
            if (source == nullptr) return;
            for (int row = 0; row < source_height; ++row)
            for (int column = 0; column < source_width; ++column)
            {
                const auto source_index = static_cast<std::size_t>(row * source_width + column) * 4;
                const auto destination_index = static_cast<std::size_t>(
                    (destination_y + row) * atlas_width + destination_x + column) * 4;
                pixels[destination_index] = source[source_index + 2];
                pixels[destination_index + 1] = source[source_index + 1];
                pixels[destination_index + 2] = source[source_index];
                pixels[destination_index + 3] = source[source_index + 3];
            }
        };
        static constexpr const std::uint8_t* armor_icons[]{
            armor_icon_assets::flak_helmet.data(), armor_icon_assets::flak_chestpiece.data(),
            armor_icon_assets::flak_gauntlets.data(), armor_icon_assets::flak_leggings.data(), armor_icon_assets::flak_boots.data(),
            armor_icon_assets::riot_helmet.data(), armor_icon_assets::riot_chestpiece.data(),
            armor_icon_assets::riot_gauntlets.data(), armor_icon_assets::riot_leggings.data(), armor_icon_assets::riot_boots.data(),
            armor_icon_assets::tek_helmet.data(), armor_icon_assets::tek_chestpiece.data(),
            armor_icon_assets::tek_gauntlets.data(), armor_icon_assets::tek_leggings.data(), armor_icon_assets::tek_boots.data(),
            armor_icon_assets::exo_chestpiece.data(), armor_icon_assets::exo_gauntlets.data()};
        static constexpr const std::uint8_t* weapon_icons[]{
            weapon_icon_assets::tek_bow.data(), weapon_icon_assets::compound_bow.data(),
            weapon_icon_assets::crossbow.data(), weapon_icon_assets::fabricated_sniper_rifle.data(),
            weapon_icon_assets::handcuffs.data(), weapon_icon_assets::harpoon_launcher.data(),
            weapon_icon_assets::pump_action_shotgun.data(), weapon_icon_assets::rocket_launcher.data(),
            weapon_icon_assets::tek_grenade_launcher.data(), weapon_icon_assets::tek_rifle.data()};
        for (int index = 0; index < static_cast<int>(std::size(armor_icons)); ++index)
            copy_rgba(armor_icons[index], 48, 48, index * 48, 400);
        for (int index = 0; index < static_cast<int>(std::size(weapon_icons)); ++index)
            copy_rgba(weapon_icons[index], 64, 64, index * 64, 448);
        pixels[0] = pixels[1] = pixels[2] = pixels[3] = 255;
        D3D11_TEXTURE2D_DESC texture_desc{};
        texture_desc.Width = atlas_width;
        texture_desc.Height = atlas_height;
        texture_desc.MipLevels = texture_desc.ArraySize = 1;
        texture_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        texture_desc.SampleDesc.Count = 1;
        texture_desc.Usage = D3D11_USAGE_IMMUTABLE;
        texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA initial{raw_pixels, atlas_width * 4, 0};
        const HRESULT created = device_->CreateTexture2D(&texture_desc, &initial, font_texture_.put());
        SelectObject(dc, old_font);
        SelectObject(dc, old_bitmap);
        DeleteObject(font);
        DeleteObject(bitmap);
        DeleteDC(dc);
        if (FAILED(created)) return false;
        return SUCCEEDED(device_->CreateShaderResourceView(font_texture_.get(), nullptr, font_view_.put()));
    }

    void Overlay::render(IDXGISwapChain* swap_chain, Settings& settings, ArkRuntime& runtime,
        InputState& input, const std::filesystem::path& settings_path)
    {
        if (!ensure_device(swap_chain)) return;
        input.begin_frame();
        accent = settings.menu_accent_color;
        accent.a = 1.0F;
        accent_dim = {accent.r * 0.53F, accent.g * 0.51F, accent.b * 0.50F, 1.0F};
        if (!input.frame_left_down) active_slider_ = 0;
        process_binding_capture(settings, input);
        vertices_.clear();
        if (settings.aim_draw_fov) draw_aim_overlay(settings, runtime);
        if (settings.esp_enabled) draw_esp(settings, runtime);
        if (settings.alerts_enabled) draw_alerts(settings, runtime);
        if (settings.show_hotkey_list) draw_hotkey_list(settings, runtime);
        if (settings.menu_open) draw_menu(settings, runtime, input, settings_path);
        if (settings.debug_panel && !settings.menu_open) draw_debug(runtime);
        flush();
    }

    void Overlay::update_feature_hotkeys(Settings& settings)
    {
        DWORD foreground_process{};
        const HWND foreground = GetForegroundWindow();
        const bool accept_input = !settings.menu_open && foreground != nullptr &&
            GetWindowThreadProcessId(foreground, &foreground_process) != 0 &&
            foreground_process == GetCurrentProcessId();

        for (auto iterator = feature_binding_runtime_.begin(); iterator != feature_binding_runtime_.end();)
        {
            if (settings.find_feature_binding(iterator->id) != nullptr)
            {
                ++iterator;
                continue;
            }
            if (iterator->hold_applied)
            {
                if (const FeatureDescriptor* descriptor = feature_descriptor(iterator->id))
                    settings.*descriptor->member = iterator->restore_value;
            }
            iterator = feature_binding_runtime_.erase(iterator);
        }

        for (FeatureBinding& binding : settings.feature_bindings)
        {
            if (binding.key == 0 || binding.id == L"aim.player" || binding.id == L"aim.dino") continue;
            const FeatureDescriptor* descriptor = feature_descriptor(binding.id);
            if (descriptor == nullptr) continue;
            bool& value = settings.*descriptor->member;
            auto state = std::find_if(feature_binding_runtime_.begin(), feature_binding_runtime_.end(),
                [&](const auto& item) { return item.id == binding.id; });
            if (state == feature_binding_runtime_.end())
            {
                feature_binding_runtime_.push_back({binding.id, binding.key, binding.mode});
                state = std::prev(feature_binding_runtime_.end());
            }
            if (state->key != binding.key || state->mode != binding.mode)
            {
                if (state->hold_applied) value = state->restore_value;
                *state = {binding.id, binding.key, binding.mode};
            }
            if (!accept_input)
            {
                if (state->hold_applied) value = state->restore_value;
                state->was_down = false;
                state->armed = false;
                state->hold_applied = false;
                state->active = binding.mode == 1 && value;
                continue;
            }
            const bool down = (GetAsyncKeyState(static_cast<int>(binding.key)) & 0x8000) != 0;
            if (!down) state->armed = true;
            const bool pressed = down && !state->was_down && state->armed;
            state->was_down = down;
            if (binding.mode == 0)
            {
                if (down && state->armed && !state->hold_applied)
                {
                    state->restore_value = value;
                    state->hold_applied = true;
                }
                if (down && state->armed) value = true;
                else if (!down && state->hold_applied)
                {
                    value = state->restore_value;
                    state->hold_applied = false;
                }
                state->active = down && state->armed;
            }
            else
            {
                if (state->hold_applied)
                {
                    value = state->restore_value;
                    state->hold_applied = false;
                }
                if (pressed) value = !value;
                state->active = value;
            }
        }
    }

    void Overlay::draw_hotkey_list(const Settings& settings, const ArkRuntime& runtime)
    {
        struct ActiveHotkey
        {
            std::wstring label;
            std::wstring category;
            std::uint32_t key{};
            std::int32_t mode{};
        };

        std::vector<ActiveHotkey> active_hotkeys;
        const Snapshot& snapshot = runtime.snapshot();
        if (settings.aim_bind_show && snapshot.player_aim_active)
            active_hotkeys.push_back({L"Player aim", L"Aim", settings.aim_key, settings.aim_activation_mode});
        if (settings.dino_aim_bind_show && snapshot.dino_aim_active)
            active_hotkeys.push_back({L"Dino aim", L"Aim", settings.dino_aim_key, settings.dino_aim_activation_mode});

        for (const FeatureBinding& binding : settings.feature_bindings)
        {
            if (binding.key == 0 || !binding.show_in_list ||
                binding.id == L"aim.player" || binding.id == L"aim.dino") continue;
            const FeatureDescriptor* descriptor = feature_descriptor(binding.id);
            if (descriptor == nullptr) continue;
            const auto state = std::find_if(feature_binding_runtime_.begin(), feature_binding_runtime_.end(),
                [&](const FeatureBindingRuntime& item) { return item.id == binding.id; });
            if (state == feature_binding_runtime_.end() || !state->active) continue;
            active_hotkeys.push_back({descriptor->label, descriptor->category, binding.key, binding.mode});
        }

        // Keep the widget compact and useful: it appears only while at least one
        // visible bind is active, matching the usual competitive-overlay behavior.
        if (active_hotkeys.empty()) return;
        constexpr float list_width = 286.0F;
        constexpr float header_height = 31.0F;
        constexpr float row_height = 34.0F;
        const float list_height = header_height + row_height * static_cast<float>(active_hotkeys.size());
        const float left = std::clamp(width_ * settings.hotkey_list_x, 8.0F,
            std::max(8.0F, width_ - list_width - 8.0F));
        const float top = std::clamp(height_ * settings.hotkey_list_y, 8.0F,
            std::max(8.0F, height_ - list_height - 8.0F));
        const Rect frame{left, top, left + list_width, top + list_height};
        fill(frame, {panel.r, panel.g, panel.b, 0.94F});
        stroke(frame, accent_dim, 1.0F);
        fill({frame.left, frame.top, frame.right, frame.top + 3.0F}, accent);
        text(L"ACTIVE HOTKEYS", {frame.left + 12.0F, frame.top + 4.0F,
            frame.right - 12.0F, frame.top + header_height}, text_primary, 11.0F);
        text(std::to_wstring(active_hotkeys.size()), {frame.right - 42.0F, frame.top + 4.0F,
            frame.right - 12.0F, frame.top + header_height}, accent, 11.0F, TextAlign::right);

        static constexpr std::array<const wchar_t*, 3> modes{L"HOLD", L"TOGGLE", L"ALWAYS"};
        float row_top = frame.top + header_height;
        for (std::size_t index = 0; index < active_hotkeys.size(); ++index)
        {
            const ActiveHotkey& hotkey = active_hotkeys[index];
            const Rect row{frame.left + 1.0F, row_top, frame.right - 1.0F, row_top + row_height};
            if ((index & 1U) != 0) fill(row, {surface.r, surface.g, surface.b, 0.48F});
            fill({row.left + 7.0F, row.top + 14.0F, row.left + 12.0F, row.top + 19.0F}, success);
            text(hotkey.label, {row.left + 20.0F, row.top + 2.0F, row.right - 100.0F, row.top + 20.0F},
                text_primary, 11.0F);
            text(hotkey.category, {row.left + 20.0F, row.top + 17.0F, row.right - 100.0F, row.bottom - 1.0F},
                text_secondary, 9.0F);
            const std::int32_t mode = std::clamp(hotkey.mode, 0, 2);
            text(key_name(hotkey.key) + L" · " + modes[static_cast<std::size_t>(mode)],
                {row.right - 128.0F, row.top, row.right - 10.0F, row.bottom}, accent, 10.0F, TextAlign::right);
            row_top += row_height;
        }
    }

    void Overlay::draw_esp(const Settings& settings, const ArkRuntime& runtime)
    {
        const Snapshot& snapshot = runtime.snapshot();
        if (!snapshot.camera.valid) return;
        const std::wstring search = lower_copy(settings.esp_search);
        std::unordered_set<std::wstring> selected_structure_classes;
        if (settings.structure_whitelist_enabled)
        {
            for (auto& class_name : exact_tokens(settings.selected_structure_types))
                selected_structure_classes.insert(lower_copy(std::move(class_name)));
        }
        const auto actor_text_filtered = [&](const Actor& actor) {
            if (token_list_contains(settings.hidden_tribes, actor.tribe)) return true;
            if (actor.kind == ActorKind::dino &&
                (token_list_contains(settings.hidden_dino_types, actor.class_name) ||
                    token_list_contains(settings.hidden_dino_types, actor.name))) return true;
            if (actor.kind == ActorKind::structure)
            {
                if (settings.structure_whitelist_enabled &&
                    !selected_structure_classes.contains(lower_copy(actor.class_name))) return true;
                if (!settings.structure_whitelist_enabled &&
                    (token_list_contains(settings.hidden_structure_types, actor.class_name) ||
                        token_list_contains(settings.hidden_structure_types, actor.name))) return true;
            }
            if (search.empty()) return false;
            return lower_copy(actor.name).find(search) == std::wstring::npos &&
                lower_copy(actor.tribe).find(search) == std::wstring::npos &&
                lower_copy(actor.class_name).find(search) == std::wstring::npos;
        };
        const auto structure_group_enabled = [&](const Actor& actor) {
            return settings.structure_grouping && actor.kind == ActorKind::structure && !actor.turret &&
                (settings.grouped_structure_types.empty() ||
                    token_list_contains(settings.grouped_structure_types, actor.class_name) ||
                    token_list_contains(settings.grouped_structure_types, actor.name));
        };
        std::unordered_map<std::wstring, int> structure_summary;
        std::unordered_map<std::wstring, int> player_summary;
        std::unordered_map<std::wstring, int> dino_summary;
        int threat_players{};
        int threat_turrets{};
        float nearest_threat = std::numeric_limits<float>::max();
        const auto summary_key = [](const Actor& actor) {
            if (!actor.tribe.empty()) return actor.tribe;
            if (!actor.name.empty()) return actor.name;
            return std::wstring{L"Unknown"};
        };
        const auto count_summary = [&](const Actor& actor) {
            if (settings.show_structure_summary && actor.kind == ActorKind::structure && !actor.turret)
                ++structure_summary[actor.name.empty() ? L"Structure" : actor.name];
            if (settings.show_player_summary && actor.kind == ActorKind::player && !actor_is_dead(actor))
                ++player_summary[summary_key(actor)];
            if (settings.show_dino_summary && actor.kind == ActorKind::dino && actor.team >= 50000)
                ++dino_summary[summary_key(actor)];
        };
        if (!settings.summary_uses_filters &&
            (settings.show_structure_summary || settings.show_player_summary || settings.show_dino_summary))
        {
            for (const Actor& actor : snapshot.actors) count_summary(actor);
        }
        struct StructureGroup { std::uintptr_t representative{}; int count{}; float distance_squared{}; };
        std::unordered_map<std::int64_t, StructureGroup> structure_groups;
        std::unordered_map<std::uintptr_t, int> representative_counts;
        if (settings.structure_grouping)
        {
            const float cell = settings.structure_group_radius_m * 100.0F;
            for (const Actor& actor : snapshot.actors)
            {
                if (!structure_group_enabled(actor) || actor_text_filtered(actor)) continue;
                const auto grid_x = static_cast<std::int32_t>(std::floor(actor.position.x / cell));
                const auto grid_y = static_cast<std::int32_t>(std::floor(actor.position.y / cell));
                const auto key = static_cast<std::int64_t>(static_cast<std::uint32_t>(grid_x)) << 32 |
                    static_cast<std::uint32_t>(grid_y);
                const float dx = actor.position.x - snapshot.camera.location.x;
                const float dy = actor.position.y - snapshot.camera.location.y;
                const float dz = actor.position.z - snapshot.camera.location.z;
                const float squared = dx * dx + dy * dy + dz * dz;
                auto& group = structure_groups[key];
                ++group.count;
                if (group.representative == 0 || squared < group.distance_squared)
                {
                    group.representative = actor.address;
                    group.distance_squared = squared;
                }
            }
            for (const auto& [key, group] : structure_groups)
            {
                (void)key;
                representative_counts[group.representative] = group.count;
            }
        }
        for (const Actor& actor : snapshot.actors)
        {
            if (actor.address == snapshot.local_pawn || actor.address == snapshot.local_character) continue;
            if (actor_text_filtered(actor)) continue;
            int structure_group_count{1};
            if (structure_group_enabled(actor))
            {
                const auto group = representative_counts.find(actor.address);
                if (group == representative_counts.end()) continue;
                structure_group_count = group->second;
            }
            bool enabled{};
            if (settings.battle_mode)
            {
                enabled = actor.kind == ActorKind::player ||
                    (actor.kind == ActorKind::dino && actor.team >= 50000) ||
                    (actor.kind == ActorKind::structure && actor.turret);
            }
            else if (actor.kind == ActorKind::player) enabled = settings.player_esp;
            else if (actor.kind == ActorKind::dino)
            {
                const bool wild = actor.team < 50000;
                enabled = wild ? settings.wild_dino_esp : settings.enemy_dino_esp;
            }
            else if (actor.kind == ActorKind::structure) enabled = actor.turret ? settings.turret_esp : settings.structure_esp;
            else if (actor.kind == ActorKind::drop) enabled = settings.drop_esp;
            else if (actor.kind == ActorKind::death_cache)
            {
                const std::wstring cache_type = lower_copy(actor.class_name + L" " + actor.name);
                const bool dino_cache = cache_type.find(L"dino") != std::wstring::npos ||
                    cache_type.find(L"dinosaur") != std::wstring::npos;
                enabled = settings.death_cache_esp &&
                    (dino_cache ? settings.dino_item_cache_esp : settings.player_item_cache_esp);
            }
            if (!enabled) continue;
            const PlayerEspState player_state = actor.kind == ActorKind::player ?
                player_esp_state(actor) : PlayerEspState::awake;
            if (actor.kind == ActorKind::player && !player_state_enabled(settings, player_state)) continue;
            const bool own = snapshot.local_team != 0 && actor.team == snapshot.local_team;
            const bool allied = own || settings.is_allied(actor.team);
            const bool wild_relation = actor.kind == ActorKind::dino && actor.team < 50000;
            const bool neutral = actor.team == 0;
            if (actor.kind == ActorKind::player)
            {
                if ((own && !settings.esp_own_players) || (!own && allied && !settings.esp_allied_players) ||
                    (!allied && !settings.esp_enemy_players)) continue;
            }
            else if (actor.kind == ActorKind::dino)
            {
                if ((own && !settings.esp_own_dinos) || (!own && allied && !settings.esp_allied_dinos) ||
                    (!allied && !wild_relation && !settings.esp_enemy_dinos)) continue;
            }
            else if (actor.kind == ActorKind::structure)
            {
                if ((neutral && !settings.esp_neutral_structures) ||
                    (!neutral && own && !settings.esp_own_structures) ||
                    (!neutral && !own && allied && !settings.esp_allied_structures) ||
                    (!neutral && !allied && !settings.esp_enemy_structures)) continue;
                if (actor.turret && settings.turret_hide_nonmatching && settings.turret_target_filter >= 0 &&
                    actor.turret_targeting != settings.turret_target_filter) continue;
            }

            const float distance_m = distance3(actor.position, snapshot.camera.location) / 100.0F;
            if (distance_m > settings.esp_distance_m) continue;
            if (actor.kind == ActorKind::drop && distance_m > settings.drop_distance_m) continue;
            const bool detailed = distance_m <= settings.esp_detail_distance_m;
            if (!allied && distance_m <= settings.threat_distance_m)
            {
                if (actor.kind == ActorKind::player && !actor_is_dead(actor)) ++threat_players;
                else if (actor.kind == ActorKind::structure && actor.turret) ++threat_turrets;
                if ((actor.kind == ActorKind::player && !actor_is_dead(actor)) ||
                    (actor.kind == ActorKind::structure && actor.turret))
                    nearest_threat = std::min(nearest_threat, distance_m);
            }
            if (settings.summary_uses_filters) count_summary(actor);
            const bool compact_static = settings.smart_declutter && actor.kind == ActorKind::structure &&
                !actor.turret && distance_m > 250.0F;
            Vec3 top = actor.position;
            const float world_height = actor.kind == ActorKind::player ? 185.0F :
                (actor.kind == ActorKind::dino ? 150.0F : 105.0F);
            top.z += world_height;
            Vec2 feet{};
            Vec2 head{};
            const bool feet_visible = runtime.world_to_screen(actor.position, width_, height_, feet);
            const bool head_visible = runtime.world_to_screen(top, width_, height_, head);
            Color color = own ? settings.own_color : (settings.is_allied(actor.team) ?
                settings.ally_color : settings.enemy_color);
            const Color status_color = actor.kind == ActorKind::player ?
                player_state_color(settings, player_state) : color;
            if (actor.kind == ActorKind::player && settings.player_color_source == 1) color = status_color;
            const bool player_occluded = actor.kind == ActorKind::player &&
                player_state != PlayerEspState::dead &&
                !player_recently_rendered(snapshot, actor, settings.player_visibility_grace_ms);
            if (player_occluded && settings.player_occluded_color_enabled)
                color = settings.player_occluded_color;
            if (actor.kind == ActorKind::dino && actor.team < 50000) color = settings.wild_color;
            if (actor.kind == ActorKind::structure) color = settings.structure_color;
            color.a *= settings.esp_opacity;

            if (!feet_visible || !head_visible || feet.x < 0.0F || feet.x > width_ || feet.y < 0.0F || feet.y > height_)
            {
                if (!settings.offscreen_arrows || !feet_visible) continue;
                const float center_x = width_ * 0.5F;
                const float center_y = height_ * 0.5F;
                const float dx = feet.x - center_x;
                const float dy = feet.y - center_y;
                const float length = std::max(1.0F, std::sqrt(dx * dx + dy * dy));
                const float direction_x = dx / length;
                const float direction_y = dy / length;
                const float edge_x = (width_ * 0.5F - 30.0F) / std::max(0.001F, std::abs(direction_x));
                const float edge_y = (height_ * 0.5F - 30.0F) / std::max(0.001F, std::abs(direction_y));
                const float edge = std::min(edge_x, edge_y);
                const Vec2 tip{center_x + direction_x * edge, center_y + direction_y * edge};
                const Vec2 base{tip.x - direction_x * 14.0F, tip.y - direction_y * 14.0F};
                const Vec2 left_wing{base.x - direction_y * 7.0F, base.y + direction_x * 7.0F};
                const Vec2 right_wing{base.x + direction_y * 7.0F, base.y - direction_x * 7.0F};
                line(tip.x, tip.y, left_wing.x, left_wing.y, color, 2.5F);
                line(left_wing.x, left_wing.y, right_wing.x, right_wing.y, color, 2.5F);
                line(right_wing.x, right_wing.y, tip.x, tip.y, color, 2.5F);
                continue;
            }

            const float box_height = std::max(24.0F, feet.y - head.y);
            const float box_width = box_height * 0.45F;
            const Rect box{feet.x - box_width * 0.5F, head.y, feet.x + box_width * 0.5F, feet.y};
            const bool player_style = actor.kind == ActorKind::player;
            const std::int32_t box_style = player_style ? settings.esp_box_style : settings.world_box_style;
            const std::int32_t label_side = player_style ? settings.esp_label_side : settings.world_label_side;
            const std::int32_t health_side = player_style ? settings.esp_health_side : settings.world_health_side;
            const std::int32_t torpor_side = player_style ? settings.esp_torpor_side : settings.world_torpor_side;
            if (settings.show_boxes && !compact_static)
            {
                if (box_style == 0)
                {
                    stroke(box, color, settings.esp_box_thickness);
                }
                else
                {
                    const float corner_x = box_width * 0.28F;
                    const float corner_y = box_height * 0.20F;
                    line(box.left, box.top, box.left + corner_x, box.top, color, settings.esp_box_thickness);
                    line(box.left, box.top, box.left, box.top + corner_y, color, settings.esp_box_thickness);
                    line(box.right - corner_x, box.top, box.right, box.top, color, settings.esp_box_thickness);
                    line(box.right, box.top, box.right, box.top + corner_y, color, settings.esp_box_thickness);
                    line(box.left, box.bottom - corner_y, box.left, box.bottom, color, settings.esp_box_thickness);
                    line(box.left, box.bottom, box.left + corner_x, box.bottom, color, settings.esp_box_thickness);
                    line(box.right, box.bottom - corner_y, box.right, box.bottom, color, settings.esp_box_thickness);
                    line(box.right - corner_x, box.bottom, box.right, box.bottom, color, settings.esp_box_thickness);
                }
            }
            if (settings.show_skeleton && detailed && actor.kind == ActorKind::player)
            {
                int drawn_segments{};
                if (actor.bone_count == static_cast<std::int32_t>(actor.bones.size()))
                {
                    static constexpr std::array<std::array<int, 2>, 22> segments{{
                        {0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 5}, {5, 6},
                        {5, 7}, {7, 8}, {8, 9}, {9, 10},
                        {5, 11}, {11, 12}, {12, 13}, {13, 14},
                        {0, 15}, {15, 16}, {16, 17}, {17, 18},
                        {0, 19}, {19, 20}, {20, 21}, {21, 22}}};
                    std::array<Vec2, 23> projected{};
                    std::array<bool, 23> valid{};
                    for (std::size_t index = 0; index < projected.size(); ++index)
                        valid[index] = runtime.world_to_screen(actor.bones[index], width_, height_, projected[index]);
                    const float maximum_length = std::max(80.0F, std::max(box_width, box_height) * 1.5F);
                    for (const auto& segment : segments)
                    {
                        if (!valid[segment[0]] || !valid[segment[1]]) continue;
                        const float dx = projected[segment[0]].x - projected[segment[1]].x;
                        const float dy = projected[segment[0]].y - projected[segment[1]].y;
                        if (dx * dx + dy * dy > maximum_length * maximum_length) continue;
                        line(projected[segment[0]].x, projected[segment[0]].y,
                            projected[segment[1]].x, projected[segment[1]].y,
                            {0.0F, 0.0F, 0.0F, 0.78F * settings.esp_opacity},
                            settings.esp_skeleton_thickness + 1.8F);
                        line(projected[segment[0]].x, projected[segment[0]].y,
                            projected[segment[1]].x, projected[segment[1]].y,
                            color, settings.esp_skeleton_thickness);
                        ++drawn_segments;
                    }
                }
                if (drawn_segments < 8)
                {
                    const float center_x = (box.left + box.right) * 0.5F;
                    const float shoulder_y = box.top + box_height * 0.28F;
                    const float pelvis_y = box.top + box_height * 0.62F;
                    line(center_x, box.top + 4.0F, center_x, pelvis_y, color, settings.esp_skeleton_thickness);
                    line(center_x, shoulder_y, box.left + box_width * 0.08F, box.top + box_height * 0.50F, color, settings.esp_skeleton_thickness);
                    line(center_x, shoulder_y, box.right - box_width * 0.08F, box.top + box_height * 0.50F, color, settings.esp_skeleton_thickness);
                    line(center_x, pelvis_y, box.left + box_width * 0.18F, box.bottom, color, settings.esp_skeleton_thickness);
                    line(center_x, pelvis_y, box.right - box_width * 0.18F, box.bottom, color, settings.esp_skeleton_thickness);
                }
            }
            if (settings.show_dino_skeleton && detailed && actor.kind == ActorKind::dino && !compact_static)
            {
                const auto point = [&](const float x, const float y) {
                    return Vec2{box.left + box_width * x, box.top + box_height * y};
                };
                const auto segment = [&](const Vec2& from, const Vec2& to) {
                    line(from.x, from.y, to.x, to.y, {0.0F, 0.0F, 0.0F, 0.76F * settings.esp_opacity},
                        settings.esp_skeleton_thickness + 1.7F);
                    line(from.x, from.y, to.x, to.y, color, settings.esp_skeleton_thickness);
                };
                const auto dino_head = [&](const Vec2& center) {
                    const float radius = std::clamp(std::min(box_width, box_height) * 0.04F, 1.5F, 6.0F);
                    for (int index = 0; index < 12; ++index)
                    {
                        const float first = std::numbers::pi_v<float> * 2.0F * static_cast<float>(index) / 12.0F;
                        const float second = std::numbers::pi_v<float> * 2.0F * static_cast<float>(index + 1) / 12.0F;
                        segment({center.x + std::cos(first) * radius, center.y + std::sin(first) * radius},
                            {center.x + std::cos(second) * radius, center.y + std::sin(second) * radius});
                    }
                };
                const std::wstring type = lower_copy(actor.class_name + L" " + actor.name);
                const auto has = [&](const wchar_t* value) { return type.find(value) != std::wstring::npos; };
                const bool flying = has(L"wyvern") || has(L"ptero") || has(L"argent") || has(L"quetz") ||
                    has(L"griffin") || has(L"owl") || has(L"moth") || has(L"bat");
                const bool aquatic = has(L"mosa") || has(L"plesio") || has(L"basil") || has(L"dolphin") ||
                    has(L"ichthy") || has(L"shark") || has(L"megalodon") || has(L"tuso") || has(L"dunkle");
                const bool serpentine = has(L"snake") || has(L"titanoboa") || has(L"basilisk") ||
                    has(L"eel") || has(L"lamprey");
                const bool biped = has(L"rex") || has(L"raptor") || has(L"spino") || has(L"theriz") ||
                    has(L"giga") || has(L"yuty") || has(L"carno") || has(L"allo") || has(L"troodon") || has(L"dodo");
                if (flying)
                {
                    const Vec2 chest = point(.50F, .48F);
                    segment(point(.16F, .55F), chest); segment(chest, point(.73F, .39F));
                    segment(point(.73F, .39F), point(.88F, .33F));
                    segment(chest, point(.15F, .12F)); segment(point(.15F, .12F), point(.04F, .43F));
                    segment(chest, point(.85F, .10F)); segment(point(.85F, .10F), point(.96F, .43F));
                    segment(point(.43F, .51F), point(.39F, .80F)); segment(point(.57F, .51F), point(.61F, .80F));
                    dino_head(point(.90F, .32F));
                }
                else if (aquatic || serpentine)
                {
                    const Vec2 a = point(.08F, .58F), b = point(.25F, .46F), c = point(.44F, .53F);
                    const Vec2 d = point(.63F, .39F), e = point(.82F, .43F);
                    segment(a, b); segment(b, c); segment(c, d); segment(d, e);
                    if (aquatic)
                    {
                        segment(c, point(.38F, .75F)); segment(d, point(.60F, .68F));
                        segment(a, point(.02F, .40F)); segment(a, point(.02F, .73F));
                    }
                    dino_head(point(.88F, .42F));
                }
                else if (biped)
                {
                    const Vec2 chest = point(.58F, .36F), hip = point(.45F, .60F);
                    segment(point(.08F, .55F), hip); segment(hip, chest); segment(chest, point(.68F, .20F));
                    segment(chest, point(.73F, .50F)); segment(hip, point(.58F, .77F));
                    segment(point(.58F, .77F), point(.53F, .97F)); segment(hip, point(.36F, .78F));
                    segment(point(.36F, .78F), point(.31F, .97F)); dino_head(point(.71F, .17F));
                }
                else
                {
                    const Vec2 shoulder = point(.65F, .45F), hip = point(.35F, .50F);
                    segment(point(.06F, .40F), hip); segment(hip, shoulder); segment(shoulder, point(.78F, .31F));
                    segment(point(.78F, .31F), point(.89F, .31F)); segment(hip, point(.28F, .73F));
                    segment(point(.28F, .73F), point(.23F, .97F)); segment(point(.43F, .50F), point(.47F, .74F));
                    segment(point(.47F, .74F), point(.44F, .97F)); segment(shoulder, point(.61F, .72F));
                    segment(point(.61F, .72F), point(.58F, .97F)); segment(point(.72F, .43F), point(.76F, .70F));
                    segment(point(.76F, .70F), point(.73F, .97F)); dino_head(point(.91F, .30F));
                }
            }
            if (actor.kind == ActorKind::player && detailed)
            {
                if (settings.show_held_items && !actor.held_item.empty())
                {
                    std::wstring normalized = lower_copy(actor.held_item);
                    normalized.erase(std::remove_if(normalized.begin(), normalized.end(), [](const wchar_t character) {
                        return character == L' ' || character == L'_' || character == L'-';
                    }), normalized.end());
                    int icon = normalized.find(L"tekgrenade") != std::wstring::npos ? 8 :
                        normalized.find(L"tekrifle") != std::wstring::npos ? 9 :
                        normalized.find(L"tekbow") != std::wstring::npos ? 0 :
                        normalized.find(L"compoundbow") != std::wstring::npos ? 1 :
                        normalized.find(L"crossbow") != std::wstring::npos ? 2 :
                        normalized.find(L"sniperrifle") != std::wstring::npos ? 3 :
                        normalized.find(L"handcuff") != std::wstring::npos ? 4 :
                        normalized.find(L"harpoon") != std::wstring::npos ? 5 :
                        normalized.find(L"pumpshotgun") != std::wstring::npos ? 6 :
                        normalized.find(L"rocket") != std::wstring::npos ? 7 : -1;
                    if (icon >= 0)
                    {
                        const float size = settings.esp_icon_size;
                        const Rect icon_rect{box.left - size - 9.0F, box.top, box.left - 9.0F, box.top + size};
                        fill({icon_rect.left - 2.0F, icon_rect.top - 2.0F, icon_rect.right + 2.0F, icon_rect.bottom + 2.0F},
                            {0.02F, 0.03F, 0.05F, 0.86F * settings.esp_opacity});
                        atlas_icon(icon_rect, icon * 64, 448, 64, 64);
                    }
                }
                if (settings.show_equipment)
                {
                    int visual_slot{};
                    for (int part = 0; part < 5; ++part)
                    {
                        const int tier = actor.armor_types[static_cast<std::size_t>(part)];
                        int icon = tier == 4 ? part : tier == 5 ? 5 + part : tier == 6 ? 10 + part :
                            tier == 12 && part == 1 ? 15 : tier == 12 && part == 2 ? 16 : -1;
                        if (icon < 0) continue;
                        const float size = settings.esp_icon_size;
                        const float icon_top = box.top + static_cast<float>(visual_slot) * (size + 4.0F);
                        const Rect icon_rect{box.right + 9.0F, icon_top, box.right + 9.0F + size, icon_top + size};
                        const float ratio = std::clamp(actor.armor_ratios[static_cast<std::size_t>(part)], 0.0F, 1.0F);
                        fill({icon_rect.left - 2.0F, icon_rect.top - 2.0F, icon_rect.right + 2.0F, icon_rect.bottom + 4.0F},
                            {0.02F, 0.03F, 0.05F, 0.86F * settings.esp_opacity});
                        atlas_icon(icon_rect, icon * 48, 400, 48, 48);
                        const Color durability{1.0F - ratio, ratio, 0.20F, settings.esp_opacity};
                        fill({icon_rect.left, icon_rect.bottom + 1.0F, icon_rect.right, icon_rect.bottom + 3.0F},
                            {0.01F, 0.01F, 0.01F, 0.85F});
                        fill({icon_rect.left, icon_rect.bottom + 1.0F,
                            icon_rect.left + size * ratio, icon_rect.bottom + 3.0F}, durability);
                        ++visual_slot;
                    }
                }
            }
            if (settings.show_tracers && !compact_static) line(width_ * 0.5F, height_, feet.x, feet.y, color, 1.0F);
            const bool category_health = actor.kind == ActorKind::player ? settings.show_player_health :
                actor.kind == ActorKind::dino ? settings.show_dino_health : settings.show_structure_health;
            if (settings.show_health && category_health && detailed && !compact_static && actor.max_health > 0.0F)
            {
                const float ratio = std::clamp(actor.health / actor.max_health, 0.0F, 1.0F);
                const Color health_color{
                    (1.0F - ratio) + settings.health_color.r * ratio,
                    settings.health_color.g * ratio,
                    settings.health_color.b * ratio,
                    settings.esp_opacity};
                const Color health_back{0.02F, 0.02F, 0.02F, 0.8F * settings.esp_opacity};
                if (health_side == 2)
                {
                    fill({box.right + 3.0F, box.top, box.right + 6.0F, box.bottom}, health_back);
                    fill({box.right + 3.0F, box.bottom - box_height * ratio, box.right + 6.0F, box.bottom}, health_color);
                }
                else if (health_side == 0 || health_side == 3)
                {
                    const float bar_y = health_side == 0 ? box.top - 6.0F : box.bottom + 3.0F;
                    fill({box.left, bar_y, box.right, bar_y + 3.0F}, health_back);
                    fill({box.left, bar_y, box.left + box_width * ratio, bar_y + 3.0F}, health_color);
                }
                else
                {
                    fill({box.left - 6.0F, box.top, box.left - 3.0F, box.bottom}, health_back);
                    fill({box.left - 6.0F, box.bottom - box_height * ratio, box.left - 3.0F, box.bottom}, health_color);
                }
            }
            const bool category_torpor = actor.kind == ActorKind::player ? settings.show_player_torpor :
                actor.kind == ActorKind::dino ? settings.show_dino_torpor : false;
            if (settings.show_torpor && category_torpor && detailed && !compact_static && actor.max_torpor > 0.0F)
            {
                const float ratio = std::clamp(actor.torpor / actor.max_torpor, 0.0F, 1.0F);
                const float extra = torpor_side == health_side ? 5.0F : 0.0F;
                const Color torpor_color{settings.torpor_color.r, settings.torpor_color.g,
                    settings.torpor_color.b, settings.esp_opacity};
                const Color torpor_back{0.02F, 0.02F, 0.02F, 0.8F * settings.esp_opacity};
                if (torpor_side == 1)
                {
                    const float right = box.left - 3.0F - extra;
                    fill({right - 3.0F, box.top, right, box.bottom}, torpor_back);
                    fill({right - 3.0F, box.bottom - box_height * ratio, right, box.bottom}, torpor_color);
                }
                else if (torpor_side == 2)
                {
                    const float left = box.right + 3.0F + extra;
                    fill({left, box.top, left + 3.0F, box.bottom}, torpor_back);
                    fill({left, box.bottom - box_height * ratio, left + 3.0F, box.bottom}, torpor_color);
                }
                else
                {
                    const float bar_y = torpor_side == 0 ? box.top - 6.0F - extra : box.bottom + 3.0F + extra;
                    fill({box.left, bar_y, box.right, bar_y + 3.0F}, torpor_back);
                    fill({box.left, bar_y, box.left + box_width * ratio, bar_y + 3.0F}, torpor_color);
                }
            }
            std::wstring label;
            const bool category_label = actor.kind == ActorKind::player ? settings.show_player_labels :
                actor.kind == ActorKind::dino ? settings.show_dino_labels :
                actor.kind == ActorKind::structure ? settings.show_structure_labels : true;
            if (settings.show_names && category_label && detailed) label = actor.name;
            if (settings.show_tribes && detailed && !actor.tribe.empty())
            {
                if (!label.empty()) label += settings.compact_labels ? L"  " : L"\n";
                label += L"[" + actor.tribe + L"]";
            }
            if (settings.show_distance)
            {
                if (!label.empty()) label += settings.compact_labels ? L"  " : L"\n";
                label += fixed(distance_m) + L"m";
            }
            if (actor.kind == ActorKind::drop && settings.show_drop_quantity && actor.quantity > 1)
                label += L"  x" + std::to_wstring(actor.quantity);
            if (actor.turret && settings.show_turret_details)
            {
                const std::wstring separator = settings.compact_labels ? L"  " : L"\n";
                if (settings.turret_show_ammo)
                    label += separator + L"AMMO " + (actor.turret_ammo >= 0 ? std::to_wstring(actor.turret_ammo) : L"?");
                if (settings.turret_show_range) label += separator + L"RANGE " + std::to_wstring(actor.turret_range);
                if (settings.turret_show_target_mode) label += separator + L"TARGET " + std::to_wstring(actor.turret_targeting);
                if (settings.turret_show_warning) label += separator + L"WARN " + std::to_wstring(actor.turret_warning);
                if (settings.turret_show_power) label += separator + (actor.turret_powered ? L"POWERED" : L"NO POWER");
                if (settings.turret_show_state) label += separator + (actor.turret_active ? L"ACTIVE" : L"INACTIVE");
                if (settings.turret_show_target_state)
                    label += separator + (actor.turret_targeting_actor ? L"TARGET LOCK" : L"NO TARGET");
            }
            if (structure_group_count > 1) label += L"  x" + std::to_wstring(structure_group_count);
            if (!label.empty())
            {
                Rect label_rect{box.left - 120.0F, box.top - 23.0F, box.right + 120.0F, box.top};
                TextAlign alignment = TextAlign::center;
                if (label_side == 1)
                {
                    label_rect = {box.left - 230.0F, box.top, box.left - 10.0F, box.bottom};
                    alignment = TextAlign::right;
                }
                else if (label_side == 2)
                {
                    label_rect = {box.right + 10.0F, box.top, box.right + 230.0F, box.bottom};
                    alignment = TextAlign::left;
                }
                else if (label_side == 3)
                {
                    label_rect = {box.left - 120.0F, box.bottom + 4.0F, box.right + 120.0F, box.bottom + 28.0F};
                }
                text(label, label_rect, color, settings.esp_label_size, alignment);
            }
            if (actor.kind == ActorKind::player && settings.show_player_status)
            {
                const float badge_width = player_state == PlayerEspState::knocked_out ? 148.0F : 126.0F;
                Rect status_rect{};
                if (settings.esp_status_side == 0)
                {
                    float bottom = box.top - 9.0F;
                    if (label_side == 0) bottom -= 28.0F;
                    status_rect = {feet.x - badge_width * 0.5F, bottom - 21.0F,
                        feet.x + badge_width * 0.5F, bottom};
                }
                else if (settings.esp_status_side == 1)
                {
                    float right = box.left - 10.0F;
                    if (settings.show_held_items) right -= settings.esp_icon_size + 8.0F;
                    const float status_top = box.top + (label_side == 1 ? 28.0F : 0.0F);
                    status_rect = {right - badge_width, status_top, right, status_top + 21.0F};
                }
                else if (settings.esp_status_side == 2)
                {
                    float left = box.right + 10.0F;
                    if (settings.show_equipment) left += settings.esp_icon_size + 8.0F;
                    const float status_top = box.top + (label_side == 2 ? 28.0F : 0.0F);
                    status_rect = {left, status_top, left + badge_width, status_top + 21.0F};
                }
                else
                {
                    float status_top = box.bottom + 7.0F;
                    if (label_side == 3) status_top += 28.0F;
                    if (settings.show_vital_values && detailed && !compact_static) status_top += 40.0F;
                    status_rect = {feet.x - badge_width * 0.5F, status_top,
                        feet.x + badge_width * 0.5F, status_top + 21.0F};
                }
                const Color badge_color{status_color.r, status_color.g, status_color.b,
                    status_color.a * settings.esp_opacity};
                fill(status_rect, {0.018F, 0.012F, 0.028F, 0.88F * settings.esp_opacity});
                stroke(status_rect, badge_color, 1.0F);
                text(player_state_label(player_state), status_rect, badge_color,
                    std::max(10.0F, settings.esp_label_size - 2.0F), TextAlign::center);
            }
            if (settings.show_vital_values && detailed && !compact_static && (category_health || category_torpor) &&
                (actor.max_health > 0.0F || actor.max_torpor > 0.0F))
            {
                std::wstring values;
                if (category_health && actor.max_health > 0.0F)
                    values = L"HP " + fixed(actor.health) + L"/" + fixed(actor.max_health);
                if (category_torpor && actor.max_torpor > 0.0F)
                {
                    if (!values.empty()) values += L"   ";
                    values += L"TP " + fixed(actor.torpor) + L"/" + fixed(actor.max_torpor);
                }
                text(values, {box.left - 100.0F, box.bottom + 20.0F, box.right + 100.0F, box.bottom + 40.0F},
                    text_primary, std::max(10.0F, settings.esp_label_size - 2.0F), TextAlign::center);
            }
        }
        const auto draw_summary = [&](const std::wstring& title,
            const std::unordered_map<std::wstring, int>& values, const float left, const float top) {
            if (values.empty()) return;
            std::vector<std::pair<std::wstring, int>> sorted(values.begin(), values.end());
            std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
                return a.second != b.second ? a.second > b.second : a.first < b.first;
            });
            if (sorted.size() > 8) sorted.resize(8);
            const float panel_height = 34.0F + static_cast<float>(sorted.size()) * 20.0F;
            fill({left, top, left + 230.0F, top + panel_height}, {panel.r, panel.g, panel.b, 0.90F});
            stroke({left, top, left + 230.0F, top + panel_height}, accent_dim);
            text(title, {left + 10.0F, top + 6.0F, left + 220.0F, top + 28.0F}, accent, 12.0F);
            float row = top + 31.0F;
            for (const auto& [name, count] : sorted)
            {
                text(name, {left + 10.0F, row, left + 178.0F, row + 19.0F}, text_primary, 11.0F);
                text(std::to_wstring(count), {left + 180.0F, row, left + 220.0F, row + 19.0F},
                    text_secondary, 11.0F, TextAlign::right);
                row += 20.0F;
            }
        };
        float summary_x = 18.0F;
        if (settings.show_player_summary)
        {
            draw_summary(L"PLAYERS / TRIBES", player_summary, summary_x, 72.0F);
            summary_x += 242.0F;
        }
        if (settings.show_dino_summary)
        {
            draw_summary(L"TAMED DINOS / TRIBES", dino_summary, summary_x, 72.0F);
            summary_x += 242.0F;
        }
        if (settings.show_structure_summary)
            draw_summary(L"STRUCTURES", structure_summary, summary_x, 72.0F);
        if (settings.show_threat_panel && (threat_players > 0 || threat_turrets > 0))
        {
            const float left = width_ * 0.5F - 145.0F;
            const Rect threat{left, 18.0F, left + 290.0F, 72.0F};
            fill(threat, {0.09F, 0.018F, 0.055F, 0.92F});
            stroke(threat, {1.0F, 0.22F, 0.48F, 0.85F}, 1.5F);
            text(L"THREAT  P" + std::to_wstring(threat_players) + L"  T" + std::to_wstring(threat_turrets),
                {threat.left + 10.0F, threat.top + 4.0F, threat.right - 10.0F, threat.top + 28.0F},
                {1.0F, 0.36F, 0.60F, 1.0F}, 12.0F, TextAlign::center);
            text(nearest_threat < std::numeric_limits<float>::max() ?
                L"nearest " + fixed(nearest_threat) + L"m" : L"inside configured radius",
                {threat.left + 10.0F, threat.top + 27.0F, threat.right - 10.0F, threat.bottom - 3.0F},
                text_secondary, 11.0F, TextAlign::center);
        }
        if (settings.show_radar) draw_radar(settings, runtime);
    }

    void Overlay::draw_radar(const Settings& settings, const ArkRuntime& runtime)
    {
        const Snapshot& snapshot = runtime.snapshot();
        if (!snapshot.local_valid || !snapshot.camera.valid) return;
        const float size = settings.radar_size;
        const float center_x = std::clamp(width_ * settings.radar_x, size * 0.5F + 8.0F, width_ - size * 0.5F - 8.0F);
        const float center_y = std::clamp(height_ * settings.radar_y, size * 0.5F + 8.0F, height_ - size * 0.5F - 8.0F);
        const Rect panel_rect{center_x - size * 0.5F, center_y - size * 0.5F,
            center_x + size * 0.5F, center_y + size * 0.5F};
        fill(panel_rect, {0.018F, 0.026F, 0.043F, 0.82F});
        stroke(panel_rect, {0.20F, 0.30F, 0.42F, 0.92F});
        line(panel_rect.left, center_y, panel_rect.right, center_y, {0.14F, 0.22F, 0.31F, 0.80F});
        line(center_x, panel_rect.top, center_x, panel_rect.bottom, {0.14F, 0.22F, 0.31F, 0.80F});
        fill({center_x - 3.0F, center_y - 3.0F, center_x + 3.0F, center_y + 3.0F}, settings.own_color);
        const float yaw = snapshot.camera.rotation.y * std::numbers::pi_v<float> / 180.0F;
        const float cosine = std::cos(yaw), sine = std::sin(yaw);
        int enemy_players{}, enemy_dinos{};
        const float half = size * 0.46F;
        const std::wstring search = lower_copy(settings.esp_search);
        for (const Actor& actor : snapshot.actors)
        {
            if (actor.address == snapshot.local_pawn || actor.address == snapshot.local_character) continue;
            if (token_list_contains(settings.hidden_tribes, actor.tribe) ||
                (actor.kind == ActorKind::dino && (token_list_contains(settings.hidden_dino_types, actor.class_name) ||
                    token_list_contains(settings.hidden_dino_types, actor.name)))) continue;
            if (!search.empty() && lower_copy(actor.name).find(search) == std::wstring::npos &&
                lower_copy(actor.tribe).find(search) == std::wstring::npos &&
                lower_copy(actor.class_name).find(search) == std::wstring::npos) continue;
            if (actor.kind != ActorKind::player && actor.kind != ActorKind::dino) continue;
            const PlayerEspState player_state = actor.kind == ActorKind::player ?
                player_esp_state(actor) : PlayerEspState::awake;
            if (actor.kind == ActorKind::player && !player_state_enabled(settings, player_state)) continue;
            if (actor.kind == ActorKind::dino && actor.dead) continue;
            const bool own = snapshot.local_team != 0 && actor.team == snapshot.local_team;
            const bool allied = own || settings.is_allied(actor.team);
            const bool wild = actor.kind == ActorKind::dino && actor.team < 50000;
            if (!settings.battle_mode)
            {
                if (actor.kind == ActorKind::player && !settings.player_esp) continue;
                if (actor.kind == ActorKind::dino && (wild ? !settings.wild_dino_esp : !settings.enemy_dino_esp)) continue;
            }
            else if (wild) continue;
            if (actor.kind == ActorKind::player && ((own && !settings.esp_own_players) ||
                (!own && allied && !settings.esp_allied_players) || (!allied && !settings.esp_enemy_players))) continue;
            if (actor.kind == ActorKind::dino && ((own && !settings.esp_own_dinos) ||
                (!own && allied && !settings.esp_allied_dinos) ||
                (!allied && !wild && !settings.esp_enemy_dinos))) continue;
            const float dx = (actor.position.x - snapshot.local_position.x) / 100.0F;
            const float dy = (actor.position.y - snapshot.local_position.y) / 100.0F;
            const float actor_distance = std::sqrt(dx * dx + dy * dy);
            if (actor_distance > settings.radar_range_m) continue;
            const float forward = cosine * dx + sine * dy;
            const float right = -sine * dx + cosine * dy;
            const float x = center_x + right / settings.radar_range_m * half;
            const float y = center_y - forward / settings.radar_range_m * half;
            Color color = own ? settings.own_color : allied ? settings.ally_color : settings.enemy_color;
            if (actor.kind == ActorKind::player && settings.player_color_source == 1)
                color = player_state_color(settings, player_state);
            if (wild) color = settings.wild_color;
            const float dot = actor.kind == ActorKind::player ? 3.0F : 2.0F;
            fill({x - dot, y - dot, x + dot, y + dot}, color);
            if (!allied)
            {
                if (actor.kind == ActorKind::player)
                {
                    if (!actor_is_dead(actor)) ++enemy_players;
                }
                else ++enemy_dinos;
            }
        }
        text(L"RADAR " + fixed(settings.radar_range_m) + L"m  P" + std::to_wstring(enemy_players) +
            L" D" + std::to_wstring(enemy_dinos),
            {panel_rect.left + 6.0F, panel_rect.top + 4.0F, panel_rect.right - 6.0F, panel_rect.top + 24.0F},
            text_secondary, 10.0F, TextAlign::center);
    }

    void Overlay::draw_aim_overlay(const Settings& settings, const ArkRuntime& runtime)
    {
        const float effective_fov = runtime.snapshot().local_mounted ? settings.mounted_aim_fov : settings.aim_fov;
        if ((!settings.player_aim && !settings.dino_aim) || !runtime.snapshot().camera.valid ||
            effective_fov <= 0.0F || runtime.snapshot().camera.fov <= 1.0F) return;
        const float horizontal_focal = (width_ * 0.5F) /
            std::tan(runtime.snapshot().camera.fov * std::numbers::pi_v<float> / 360.0F);
        const float radius = std::clamp(horizontal_focal *
            std::tan(effective_fov * std::numbers::pi_v<float> / 180.0F), 4.0F, width_);
        constexpr int segments = 96;
        constexpr float tau = std::numbers::pi_v<float> * 2.0F;
        for (int segment = 0; segment < segments; ++segment)
        {
            const float first = tau * static_cast<float>(segment) / static_cast<float>(segments);
            const float second = tau * static_cast<float>(segment + 1) / static_cast<float>(segments);
            line(width_ * 0.5F + std::cos(first) * radius, height_ * 0.5F + std::sin(first) * radius,
                width_ * 0.5F + std::cos(second) * radius, height_ * 0.5F + std::sin(second) * radius,
                {0.29F, 0.90F, 0.62F, 0.68F}, 1.2F);
        }
    }

    void Overlay::draw_alerts(const Settings& settings, const ArkRuntime& runtime)
    {
        const auto& alerts = runtime.snapshot().alerts;
        if (alerts.empty()) return;
        constexpr float card_width = 352.0F;
        constexpr float card_height = 90.0F;
        constexpr float gap = 8.0F;
        const float left = std::max(12.0F, width_ - card_width - 22.0F);
        float top = std::max(12.0F, height_ - 22.0F -
            static_cast<float>(alerts.size()) * (card_height + gap));
        for (const Alert& alert : alerts)
        {
            Color kind_color = accent;
            switch (alert.kind)
            {
            case AlertKind::death: kind_color = {0.94F, 0.30F, 0.38F, 1.0F}; break;
            case AlertKind::approach: kind_color = warning; break;
            case AlertKind::new_player: kind_color = {0.72F, 0.47F, 1.0F, 1.0F}; break;
            case AlertKind::sleep: kind_color = {0.45F, 0.63F, 1.0F, 1.0F}; break;
            case AlertKind::noglin: kind_color = {0.40F, 0.92F, 0.66F, 1.0F}; break;
            case AlertKind::turret: kind_color = {1.0F, 0.40F, 0.24F, 1.0F}; break;
            case AlertKind::enemy_group: kind_color = {0.95F, 0.30F, 0.78F, 1.0F}; break;
            }
            const Rect card{left, top, left + card_width, top + card_height};
            fill(card, {panel.r, panel.g, panel.b, 0.94F});
            stroke(card, {kind_color.r, kind_color.g, kind_color.b, 0.72F}, 1.0F);
            fill({card.left, card.top, card.left + 4.0F, card.bottom}, kind_color);
            text(alert.title, {card.left + 16.0F, card.top + 6.0F, card.right - 14.0F, card.top + 28.0F},
                kind_color, 12.0F);
            const bool player_event = alert.kind == AlertKind::new_player || alert.kind == AlertKind::approach ||
                alert.kind == AlertKind::sleep || alert.kind == AlertKind::death;
            std::wstring detail = player_event ? L"Player: " : L"Actor: ";
            detail += alert.name.empty() ? (player_event ? L"Unknown player" : L"Unknown actor") : alert.name;
            if (alert.distance_m > 0.0F) detail += L"  |  Distance: " + fixed(alert.distance_m) + L" m";
            if (alert.kind == AlertKind::approach && alert.value > 0.0F)
                detail += L"  |  " + fixed(alert.value, 1) + L" m/s";
            if (alert.kind == AlertKind::sleep && alert.value > 0.0F)
                detail += L"  |  Torpor: " + fixed(alert.value) + L"%";
            text(detail, {card.left + 16.0F, card.top + 29.0F, card.right - 14.0F, card.top + 55.0F},
                text_primary, 12.0F);
            std::wstring tribe_detail;
            if (player_event)
                tribe_detail = L"Tribe: " + (alert.tribe.empty() ? std::wstring(L"Unknown tribe") : alert.tribe);
            else if (!alert.tribe.empty())
                tribe_detail = L"Tribe: " + alert.tribe;
            if (!tribe_detail.empty())
                text(tribe_detail, {card.left + 16.0F, card.top + 49.0F, card.right - 14.0F, card.top + 73.0F},
                    text_secondary, 11.0F);
            const float ratio = std::clamp(alert.remaining_s / settings.alert_lifetime_s, 0.0F, 1.0F);
            fill({card.left + 16.0F, card.bottom - 8.0F, card.right - 14.0F, card.bottom - 5.0F}, surface);
            fill({card.left + 16.0F, card.bottom - 8.0F,
                card.left + 16.0F + (card_width - 46.0F) * ratio, card.bottom - 5.0F}, kind_color);
            top += card_height + gap;
        }
    }

    void Overlay::draw_menu(Settings& settings, ArkRuntime& runtime, InputState& input,
        const std::filesystem::path& settings_path)
    {
        settings_context_ = &settings;
        if (open_combo_ != -1 && active_combo_rect_valid_ && input.frame_left_pressed &&
            !contains(active_combo_rect_, input.frame_mouse_x, input.frame_mouse_y) &&
            !contains(active_combo_control_rect_, input.frame_mouse_x, input.frame_mouse_y))
        {
            open_combo_ = -1;
            active_combo_rect_valid_ = false;
        }
        combo_popup_ = {};
        const float maximum_width = std::max(320.0F, width_ - 16.0F);
        const float maximum_height = std::max(320.0F, height_ - 16.0F);
        const float minimum_width = std::min(760.0F, maximum_width);
        const float minimum_height = std::min(480.0F, maximum_height);
        float menu_width = std::clamp(settings.menu_width, minimum_width, maximum_width);
        float menu_height = std::clamp(settings.menu_height, minimum_height, maximum_height);
        settings.menu_width = menu_width;
        settings.menu_height = menu_height;
        if (!menu_position_initialized_)
        {
            menu_left_ = std::max(12.0F, (width_ - menu_width) * 0.5F);
            menu_top_ = std::max(12.0F, (height_ - menu_height) * 0.5F);
            menu_position_initialized_ = true;
        }
        menu_left_ = std::clamp(menu_left_, 8.0F, std::max(8.0F, width_ - menu_width - 8.0F));
        menu_top_ = std::clamp(menu_top_, 8.0F, std::max(8.0F, height_ - menu_height - 8.0F));
        Rect resize_region{menu_left_ + menu_width - 22.0F, menu_top_ + menu_height - 22.0F,
            menu_left_ + menu_width, menu_top_ + menu_height};
        if (!menu_dragging_ && !menu_resizing_ && consume_click(resize_region, input))
        {
            menu_resizing_ = true;
            open_combo_ = -1;
        }
        if (menu_resizing_)
        {
            if (input.frame_left_down)
            {
                menu_width = std::clamp(static_cast<float>(input.frame_mouse_x) - menu_left_,
                    minimum_width, maximum_width);
                menu_height = std::clamp(static_cast<float>(input.frame_mouse_y) - menu_top_,
                    minimum_height, maximum_height);
                settings.menu_width = menu_width;
                settings.menu_height = menu_height;
            }
            else menu_resizing_ = false;
        }
        menu_left_ = std::clamp(menu_left_, 8.0F, std::max(8.0F, width_ - menu_width - 8.0F));
        menu_top_ = std::clamp(menu_top_, 8.0F, std::max(8.0F, height_ - menu_height - 8.0F));
        const Rect drag_region{menu_left_, menu_top_, menu_left_ + menu_width, menu_top_ + 72.0F};
        if (!menu_resizing_ && !menu_dragging_ && consume_click(drag_region, input))
        {
            menu_dragging_ = true;
            menu_drag_offset_x_ = static_cast<float>(input.frame_mouse_x) - menu_left_;
            menu_drag_offset_y_ = static_cast<float>(input.frame_mouse_y) - menu_top_;
        }
        if (menu_dragging_)
        {
            if (input.frame_left_down)
            {
                menu_left_ = std::clamp(static_cast<float>(input.frame_mouse_x) - menu_drag_offset_x_,
                    8.0F, std::max(8.0F, width_ - menu_width - 8.0F));
                menu_top_ = std::clamp(static_cast<float>(input.frame_mouse_y) - menu_drag_offset_y_,
                    8.0F, std::max(8.0F, height_ - menu_height - 8.0F));
            }
            else
            {
                menu_dragging_ = false;
            }
        }
        const float left = menu_left_;
        const float top = menu_top_;
        const Rect frame{left, top, left + menu_width, top + menu_height};
        current_menu_bottom_ = frame.bottom;
        fill(frame, panel);
        stroke(frame, {0.220F, 0.145F, 0.365F, 1.0F}, 1.0F);
        fill({left, top, left + 188.0F, top + menu_height}, {0.025F, 0.018F, 0.037F, 0.99F});
        fill({left, top, left + 4.0F, top + menu_height}, accent);
        text(L"KOPT", {left + 24.0F, top + 22.0F, left + 164.0F, top + 55.0F}, text_primary, 25.0F);
        text(L"INTERNAL / PROTON", {left + 24.0F, top + 54.0F, left + 170.0F, top + 76.0F}, accent, 10.0F);

        static constexpr const wchar_t* tabs[]{L"Aim", L"ESP", L"Camera", L"Visuals", L"Chams", L"Bindings", L"Hotkeys", L"Alerts"};
        float tab_y = top + 92.0F;
        for (int i = 0; i < 8; ++i)
        {
            if (button(tabs[i], {left + 16.0F, tab_y, left + 172.0F, tab_y + 38.0F}, active_tab_ == i, input))
            {
                active_tab_ = i;
                open_combo_ = -1;
            }
            tab_y += 40.0F;
        }

        text(key_name(settings.menu_key) + L"  Toggle menu", {left + 24.0F, top + menu_height - 66.0F, left + 180.0F, top + menu_height - 45.0F}, text_secondary, 11.0F);
        text(key_name(settings.unload_key) + L"  Unload payload", {left + 24.0F, top + menu_height - 43.0F, left + 180.0F, top + menu_height - 22.0F}, text_secondary, 11.0F);

        const float content_left = left + 220.0F;
        content_width_ = std::max(480.0F, frame.right - 28.0F - content_left);
        float y = top + 78.0F;
        text(tabs[active_tab_], {content_left, top + 24.0F, frame.right - 24.0F, top + 58.0F}, text_primary, 24.0F);
        line(content_left, top + 62.0F, frame.right - 28.0F, top + 62.0F, {0.220F, 0.145F, 0.365F, 1.0F});

        if (active_tab_ == 0)
        {
            static constexpr const wchar_t* sections[]{L"General", L"Targets", L"Tuning", L"Prediction"};
            for (int index = 0; index < 4; ++index)
            {
                const float section_left = content_left + static_cast<float>(index) * 128.0F;
                if (button(sections[index], {section_left, y, section_left + 120.0F, y + 34.0F},
                    active_aim_section_ == index, input))
                {
                    active_aim_section_ = index;
                    open_combo_ = -1;
                }
            }
            y += 48.0F;
            if (active_aim_section_ == 0)
            {
                checkbox(L"Player aim", settings.player_aim, content_left, y, input);
                checkbox(L"Dino aim", settings.dino_aim, content_left, y, input);
                checkbox(L"Lock selected target", settings.aim_lock, content_left, y, input);
                checkbox(L"Draw aim FOV", settings.aim_draw_fov, content_left, y, input);
                text(runtime.snapshot().local_mounted ? L"Mounted controller route: active" : L"Mounted controller route: ready",
                    {content_left + 2.0F, y + 8.0F, frame.right - 32.0F, y + 36.0F},
                    runtime.snapshot().local_mounted ? success : text_secondary, 12.0F);
            }
            else if (active_aim_section_ == 1)
            {
                static constexpr const wchar_t* hitbox_modes[]{L"Minimal presets", L"Advanced custom"};
                static constexpr const wchar_t* hitboxes[]{L"Head", L"Chest", L"Arms", L"Legs", L"Boots"};
                static constexpr const wchar_t* point_methods[]{L"Adaptive", L"Lowest durability", L"Closest to crosshair"};
                static constexpr const wchar_t* priorities[]{L"Crosshair angle", L"Distance", L"Lowest health", L"Balanced"};
                checkbox(L"Target enemies", settings.aim_target_enemies, content_left, y, input);
                checkbox(L"Target allies", settings.aim_target_allies, content_left, y, input);
                checkbox(L"Visible targets only", settings.visibility_check, content_left, y, input);
                combo(L"Hitbox setup", settings.aim_hitbox_mode, hitbox_modes, std::size(hitbox_modes), 2,
                    content_left, y, input);
                if (settings.aim_hitbox_mode == 0)
                {
                    combo(L"Hit zone preset", settings.aim_hitbox, hitboxes, std::size(hitboxes), 3,
                        content_left, y, input);
                    text(L"Paired presets include both left and right limbs and pick the point closest to the crosshair.",
                        {content_left + 2.0F, y, frame.right - 32.0F, y + 40.0F}, text_secondary, 11.0F);
                    y += 44.0F;
                }
                else
                {
                    static constexpr std::array<const wchar_t*, 8> labels{
                        L"Head", L"Chest", L"Left arm", L"Right arm",
                        L"Left leg", L"Right leg", L"Left boot", L"Right boot"};
                    for (int row = 0; row < 4; ++row)
                    {
                        for (int column = 0; column < 2; ++column)
                        {
                            const int index = row * 2 + column;
                            const std::uint32_t mask = 1U << index;
                            const float option_left = content_left + static_cast<float>(column) * 254.0F;
                            if (button(labels[static_cast<std::size_t>(index)],
                                {option_left, y, option_left + 246.0F, y + 30.0F},
                                (settings.aim_hitbox_mask & mask) != 0, input))
                            {
                                settings.aim_hitbox_mask ^= mask;
                                if (settings.aim_hitbox_mask == 0) settings.aim_hitbox_mask = mask;
                            }
                        }
                        y += 36.0F;
                    }
                    combo(L"Custom point method", settings.aim_point_method, point_methods,
                        std::size(point_methods), 4, content_left, y, input);
                }
                combo(L"Target priority", settings.aim_priority, priorities, std::size(priorities), 5, content_left, y, input);
            }
            else if (active_aim_section_ == 2)
            {
                slider(L"Aim FOV", settings.aim_fov, 1.0F, 45.0F, content_left, y, input, L" deg");
                slider(L"Maximum distance", settings.aim_distance_m, 25.0F, 1500.0F, content_left, y, input, L" m");
                slider(L"Smoothing", settings.aim_smoothing, 1.0F, 25.0F, content_left, y, input);
                slider(L"Angle catch-up", settings.aim_angle_boost, 0.0F, 4.0F, content_left, y, input);
                slider(L"Mounted FOV", settings.mounted_aim_fov, 1.0F, 45.0F, content_left, y, input, L" deg");
                slider(L"Mounted smoothing", settings.mounted_aim_smoothing, 1.0F, 25.0F, content_left, y, input);
            }
            else
            {
                checkbox(L"Projectile prediction", settings.aim_prediction, content_left, y, input);
                checkbox(L"Moving-target intercept solver", settings.aim_intercept_solver, content_left, y, input);
                slider(L"Projectile velocity", settings.projectile_velocity_mps, 10.0F, 2500.0F,
                    content_left, y, input, L" m/s");
                slider(L"Gravity", settings.projectile_gravity_mps2, 0.0F, 50.0F,
                    content_left, y, input, L" m/s2");
                slider(L"Network latency", settings.prediction_latency_ms, 0.0F, 500.0F,
                    content_left, y, input, L" ms");
                text(L"Intercept solves lateral/diagonal travel time; Angle catch-up accelerates large FOV errors without changing near-target smoothing.",
                    {content_left, y + 8.0F, frame.right - 32.0F, y + 50.0F}, text_secondary, 12.0F);
            }
        }
        else if (active_tab_ == 1)
        {
            static constexpr const wchar_t* sections[]{L"Targets", L"Elements", L"Player", L"World", L"Allies", L"Gear", L"Radar", L"Search", L"Colors"};
            for (int index = 0; index < 9; ++index)
            {
                const float section_left = content_left + static_cast<float>(index) * 56.0F;
                if (button(sections[index], {section_left, y, section_left + 52.0F, y + 34.0F},
                    active_esp_section_ == index, input))
                {
                    active_esp_section_ = index;
                    open_combo_ = -1;
                }
            }
            y += 48.0F;
            if (active_esp_section_ == 0)
            {
                static constexpr const wchar_t* target_pages[]{L"Categories", L"Relations", L"Distance"};
                for (int page = 0; page < 3; ++page)
                {
                    const float page_left = content_left + static_cast<float>(page) * 170.0F;
                    if (button(target_pages[page], {page_left, y, page_left + 162.0F, y + 30.0F},
                        target_settings_page_ == page, input)) target_settings_page_ = page;
                }
                y += 42.0F;
                if (target_settings_page_ == 0)
                {
                    checkbox(L"Master ESP", settings.esp_enabled, content_left, y, input);
                    checkbox(L"Players", settings.player_esp, content_left, y, input);
                    checkbox(L"Enemy / tamed dinosaurs", settings.enemy_dino_esp, content_left, y, input);
                    checkbox(L"Wild dinosaurs", settings.wild_dino_esp, content_left, y, input);
                    checkbox(L"Structures", settings.structure_esp, content_left, y, input);
                    checkbox(L"Turrets", settings.turret_esp, content_left, y, input);
                    checkbox(L"Ground drops", settings.drop_esp, content_left, y, input);
                    checkbox(L"Death caches", settings.death_cache_esp, content_left, y, input);
                    if (settings.death_cache_esp)
                    {
                        checkbox(L"Player item caches", settings.player_item_cache_esp, content_left, y, input);
                        checkbox(L"Dino item caches", settings.dino_item_cache_esp, content_left, y, input);
                    }
                    checkbox(L"Battle Mode (players / tames / turrets)", settings.battle_mode, content_left, y, input);
                }
                else if (target_settings_page_ == 1)
                {
                    const auto relation_row = [&](const wchar_t* label, bool& own, bool& allied, bool& enemy) {
                        text(label, {content_left + 2.0F, y, content_left + 138.0F, y + 28.0F}, text_primary, 12.0F);
                        if (button(L"OWN", {content_left + 145.0F, y, content_left + 250.0F, y + 28.0F}, own, input)) own = !own;
                        if (button(L"ALLY", {content_left + 256.0F, y, content_left + 361.0F, y + 28.0F}, allied, input)) allied = !allied;
                        if (button(L"ENEMY", {content_left + 367.0F, y, content_left + 500.0F, y + 28.0F}, enemy, input)) enemy = !enemy;
                        y += 32.0F;
                    };
                    relation_row(L"Players", settings.esp_own_players, settings.esp_allied_players, settings.esp_enemy_players);
                    relation_row(L"Tamed dinos", settings.esp_own_dinos, settings.esp_allied_dinos, settings.esp_enemy_dinos);
                    relation_row(L"Structures", settings.esp_own_structures, settings.esp_allied_structures, settings.esp_enemy_structures);
                    checkbox(L"Neutral / world structures", settings.esp_neutral_structures, content_left, y, input);
                    checkbox(L"Show awake players", settings.show_awake_players, content_left, y, input);
                    checkbox(L"Show sleeping players", settings.show_sleeping_players, content_left, y, input);
                    checkbox(L"Show knocked-out players", settings.show_knocked_out_players, content_left, y, input);
                    checkbox(L"Show dead players", settings.show_dead_players, content_left, y, input);
                }
                else
                {
                    slider(L"ESP distance", settings.esp_distance_m, 50.0F, 5000.0F, content_left, y, input, L" m");
                    slider(L"Full detail distance", settings.esp_detail_distance_m, 10.0F, 5000.0F,
                        content_left, y, input, L" m");
                    slider(L"Ground item distance", settings.drop_distance_m, 10.0F, 5000.0F, content_left, y, input, L" m");
                    text(L"Full-detail range is configured in World. All distance controls preserve maximum quality by default.",
                        {content_left + 2.0F, y + 4.0F, frame.right - 32.0F, y + 52.0F}, text_secondary, 12.0F);
                }
            }
            else if (active_esp_section_ == 1)
            {
                if (button(L"Visual", {content_left, y, content_left + 162.0F, y + 30.0F},
                    element_settings_page_ == 0, input)) element_settings_page_ = 0;
                if (button(L"Typography", {content_left + 170.0F, y, content_left + 332.0F, y + 30.0F},
                    element_settings_page_ == 1, input)) element_settings_page_ = 1;
                if (button(L"Summaries", {content_left + 340.0F, y, content_left + 502.0F, y + 30.0F},
                    element_settings_page_ == 2, input)) element_settings_page_ = 2;
                y += 42.0F;
                if (element_settings_page_ == 0)
                {
                    checkbox(L"Names", settings.show_names, content_left, y, input);
                    checkbox(L"Tribe labels", settings.show_tribes, content_left, y, input);
                    checkbox(L"Distance labels", settings.show_distance, content_left, y, input);
                    checkbox(L"Health bars", settings.show_health, content_left, y, input);
                    checkbox(L"Torpor bars", settings.show_torpor, content_left, y, input);
                    checkbox(L"HP / Torpor values", settings.show_vital_values, content_left, y, input);
                    checkbox(L"2D boxes", settings.show_boxes, content_left, y, input);
                    checkbox(L"Player skeleton", settings.show_skeleton, content_left, y, input);
                    checkbox(L"Tracers", settings.show_tracers, content_left, y, input);
                    checkbox(L"Off-screen markers", settings.offscreen_arrows, content_left, y, input);
                }
                else if (element_settings_page_ == 1)
                {
                    slider(L"Global opacity", settings.esp_opacity, 0.15F, 1.0F, content_left, y, input);
                    slider(L"Label size", settings.esp_label_size, 10.0F, 22.0F, content_left, y, input, L" px");
                }
                else
                {
                    checkbox(L"Structure summary", settings.show_structure_summary, content_left, y, input);
                    checkbox(L"Player summary", settings.show_player_summary, content_left, y, input);
                    checkbox(L"Tamed dino summary", settings.show_dino_summary, content_left, y, input);
                    checkbox(L"Summaries use ESP filters", settings.summary_uses_filters, content_left, y, input);
                }
            }
            else if (active_esp_section_ == 2)
            {
                static constexpr const wchar_t* box_styles[]{L"Full box", L"Corner box"};
                static constexpr const wchar_t* sides[]{L"Top", L"Left", L"Right", L"Bottom"};
                static constexpr const wchar_t* player_color_sources[]{L"Relation", L"Player status"};
                text(L"PLAYER STYLE · controlled by the side Preview", {content_left + 2.0F, y,
                    frame.right - 32.0F, y + 24.0F}, accent, 11.0F);
                y += 28.0F;
                combo(L"Box style", settings.esp_box_style, box_styles, std::size(box_styles), 10, content_left, y, input);
                combo(L"ESP color source", settings.player_color_source, player_color_sources,
                    std::size(player_color_sources), 21, content_left, y, input);
                combo(L"Label anchor", settings.esp_label_side, sides, std::size(sides), 11, content_left, y, input);
                combo(L"Health anchor", settings.esp_health_side, sides, std::size(sides), 12, content_left, y, input);
                combo(L"Torpor anchor", settings.esp_torpor_side, sides, std::size(sides), 13, content_left, y, input);
                combo(L"Status anchor", settings.esp_status_side, sides, std::size(sides), 22, content_left, y, input);
                checkbox(L"Player labels", settings.show_player_labels, content_left, y, input);
                checkbox(L"Player status badge", settings.show_player_status, content_left, y, input);
                checkbox(L"Player health", settings.show_player_health, content_left, y, input);
                checkbox(L"Player torpor", settings.show_player_torpor, content_left, y, input);
                checkbox(L"Separate occluded player color", settings.player_occluded_color_enabled,
                    content_left, y, input);
                slider(L"Visibility grace", settings.player_visibility_grace_ms, 50.0F, 500.0F,
                    content_left, y, input, L" ms");
                text(L"Visible players keep Relation/Status colors; geometry-occluded players use the dedicated color.",
                    {content_left + 2.0F, y + 2.0F, frame.right - 32.0F, y + 42.0F}, text_secondary, 11.0F);
                y += 44.0F;
                slider(L"Box thickness", settings.esp_box_thickness, 0.5F, 4.0F, content_left, y, input, L" px");
                slider(L"Skeleton thickness", settings.esp_skeleton_thickness, 0.5F, 3.0F, content_left, y, input, L" px");
            }
            else if (active_esp_section_ == 3)
            {
                static constexpr const wchar_t* box_styles[]{L"Full box", L"Corner box"};
                static constexpr const wchar_t* sides[]{L"Top", L"Left", L"Right", L"Bottom"};
                text(L"WORLD STYLE · dinos, structures, turrets, drops and caches", {content_left + 2.0F, y,
                    frame.right - 32.0F, y + 24.0F}, accent, 11.0F);
                y += 28.0F;
                combo(L"Box style", settings.world_box_style, box_styles, std::size(box_styles), 14, content_left, y, input);
                combo(L"Label anchor", settings.world_label_side, sides, std::size(sides), 15, content_left, y, input);
                combo(L"Health anchor", settings.world_health_side, sides, std::size(sides), 16, content_left, y, input);
                combo(L"Torpor anchor", settings.world_torpor_side, sides, std::size(sides), 17, content_left, y, input);
                checkbox(L"Dino labels", settings.show_dino_labels, content_left, y, input);
                checkbox(L"Dino skeleton silhouettes", settings.show_dino_skeleton, content_left, y, input);
                checkbox(L"Dino health", settings.show_dino_health, content_left, y, input);
                checkbox(L"Dino torpor", settings.show_dino_torpor, content_left, y, input);
                checkbox(L"Structure labels", settings.show_structure_labels, content_left, y, input);
                checkbox(L"Structure health", settings.show_structure_health, content_left, y, input);
                text(L"These controls are independent from Player Preview.", {content_left + 2.0F, y + 4.0F,
                    frame.right - 32.0F, y + 34.0F}, text_secondary, 12.0F);
            }
            else if (active_esp_section_ == 4)
            {
                const Snapshot& snapshot = runtime.snapshot();
                text(L"Local Team: " + std::to_wstring(snapshot.local_team),
                    {content_left + 2.0F, y, content_left + 500.0F, y + 28.0F}, accent, 13.0F);
                y += 32.0F;
                struct TeamEntry
                {
                    std::int32_t id{};
                    std::wstring tribe;
                    int name_quality{};
                };
                std::vector<TeamEntry> teams;
                for (const Actor& actor : snapshot.actors)
                {
                    if (actor.team <= 0 || actor.team == snapshot.local_team ||
                        (actor.kind == ActorKind::dino && actor.team < 50000)) continue;
                    const auto duplicate = std::find_if(teams.begin(), teams.end(), [&](const auto& item) {
                        return item.id == actor.team;
                    });
                    const int quality = actor.tribe.empty() ? 0 :
                        (actor.kind == ActorKind::player || actor.kind == ActorKind::dino ? 2 : 1);
                    if (duplicate == teams.end())
                        teams.push_back({actor.team, actor.tribe, quality});
                    else if (quality > duplicate->name_quality)
                    {
                        duplicate->tribe = actor.tribe;
                        duplicate->name_quality = quality;
                    }
                }
                std::sort(teams.begin(), teams.end(), [](const auto& left_team, const auto& right_team) {
                    return left_team.id < right_team.id;
                });
                constexpr int teams_per_page = 8;
                const int page_count = std::max(1, static_cast<int>((teams.size() + teams_per_page - 1) / teams_per_page));
                relation_page_ = std::clamp(relation_page_, 0, page_count - 1);
                const int begin = relation_page_ * teams_per_page;
                const int end = std::min(static_cast<int>(teams.size()), begin + teams_per_page);
                if (teams.empty())
                {
                    text(L"No foreign Team/Tribe is present in the current client snapshot.",
                        {content_left + 2.0F, y, content_left + 500.0F, y + 48.0F}, text_secondary, 12.0F);
                    y += 56.0F;
                }
                for (int index = begin; index < end; ++index)
                {
                    const auto& team = teams[index];
                    const std::wstring label = (team.tribe.empty() ? L"Unknown tribe" : team.tribe) +
                        L"  [ID: " + std::to_wstring(team.id) + L"]";
                    bool allied = settings.is_allied(team.id);
                    if (checkbox(label, allied, content_left, y, input))
                        settings.set_allied(team.id, allied);
                }
                if (page_count > 1)
                {
                    if (button(L"Previous", {content_left, y + 4.0F, content_left + 118.0F, y + 38.0F},
                        false, input)) relation_page_ = std::max(0, relation_page_ - 1);
                    text(std::to_wstring(relation_page_ + 1) + L" / " + std::to_wstring(page_count),
                        {content_left + 126.0F, y + 4.0F, content_left + 210.0F, y + 38.0F}, text_secondary, 12.0F,
                        TextAlign::center);
                    if (button(L"Next", {content_left + 218.0F, y + 4.0F, content_left + 336.0F, y + 38.0F},
                        false, input)) relation_page_ = std::min(page_count - 1, relation_page_ + 1);
                    y += 46.0F;
                }
                if (!settings.allied_teams.empty() && button(L"Clear manual allies",
                    {content_left, y + 4.0F, content_left + 190.0F, y + 40.0F}, false, input))
                    settings.allied_teams.clear();
            }
            else if (active_esp_section_ == 5)
            {
                if (button(L"Equipment", {content_left, y, content_left + 248.0F, y + 30.0F},
                    gear_settings_page_ == 0, input)) gear_settings_page_ = 0;
                if (button(L"Turrets", {content_left + 256.0F, y, content_left + 504.0F, y + 30.0F},
                    gear_settings_page_ == 1, input)) gear_settings_page_ = 1;
                y += 42.0F;
                if (gear_settings_page_ == 0)
                {
                    checkbox(L"Held weapon icons", settings.show_held_items, content_left, y, input);
                    checkbox(L"Armor icons + durability", settings.show_equipment, content_left, y, input);
                    checkbox(L"Compact multi-data labels", settings.compact_labels, content_left, y, input);
                    checkbox(L"Dropped item quantities", settings.show_drop_quantity, content_left, y, input);
                    slider(L"Equipment icon size", settings.esp_icon_size, 18.0F, 48.0F, content_left, y, input, L" px");
                    text(L"Equipment is sampled on actor discovery, not on the high-frequency position/bone path.",
                        {content_left + 2.0F, y + 4.0F, frame.right - 32.0F, y + 44.0F}, text_secondary, 12.0F);
                }
                else
                {
                    checkbox(L"Turret ammo / state / settings", settings.show_turret_details, content_left, y, input);
                    checkbox(L"Turret ammo", settings.turret_show_ammo, content_left, y, input);
                    checkbox(L"Turret state", settings.turret_show_state, content_left, y, input);
                    checkbox(L"Turret power", settings.turret_show_power, content_left, y, input);
                    checkbox(L"Turret range", settings.turret_show_range, content_left, y, input);
                    checkbox(L"Turret target mode", settings.turret_show_target_mode, content_left, y, input);
                    checkbox(L"Turret target lock", settings.turret_show_target_state, content_left, y, input);
                    checkbox(L"Turret warning mode", settings.turret_show_warning, content_left, y, input);
                    static constexpr const wchar_t* target_filters[]{L"Any", L"Mode 0", L"Mode 1", L"Mode 2", L"Mode 3", L"Mode 4", L"Mode 5"};
                    std::int32_t filter_index = settings.turret_target_filter + 1;
                    if (combo(L"Target-mode filter", filter_index, target_filters, std::size(target_filters), 18,
                        content_left, y, input)) settings.turret_target_filter = filter_index - 1;
                    checkbox(L"Hide non-matching turrets", settings.turret_hide_nonmatching, content_left, y, input);
                }
            }
            else if (active_esp_section_ == 6)
            {
                if (button(L"Radar layout", {content_left, y, content_left + 248.0F, y + 30.0F},
                    radar_settings_page_ == 0, input)) radar_settings_page_ = 0;
                if (button(L"Intel / declutter", {content_left + 256.0F, y, content_left + 504.0F, y + 30.0F},
                    radar_settings_page_ == 1, input)) radar_settings_page_ = 1;
                y += 42.0F;
                if (radar_settings_page_ == 0)
                {
                    checkbox(L"Radar", settings.show_radar, content_left, y, input);
                    slider(L"Radar size", settings.radar_size, 120.0F, 360.0F, content_left, y, input, L" px");
                    slider(L"Radar range", settings.radar_range_m, 50.0F, 1500.0F, content_left, y, input, L" m");
                    slider(L"Horizontal position", settings.radar_x, 0.05F, 0.95F, content_left, y, input);
                    slider(L"Vertical position", settings.radar_y, 0.05F, 0.95F, content_left, y, input);
                }
                else
                {
                    checkbox(L"Threat panel", settings.show_threat_panel, content_left, y, input);
                    slider(L"Threat radius", settings.threat_distance_m, 25.0F, 2000.0F, content_left, y, input, L" m");
                    checkbox(L"Group dense structures", settings.structure_grouping, content_left, y, input);
                    checkbox(L"Smart declutter (static structures only)", settings.smart_declutter, content_left, y, input);
                    if (settings.structure_grouping)
                        slider(L"Structure group radius", settings.structure_group_radius_m, 2.0F, 50.0F,
                            content_left, y, input, L" m");
                }
            }
            else if (active_esp_section_ == 7)
            {
                if (button(L"Quick filters", {content_left, y, content_left + 248.0F, y + 30.0F},
                    search_settings_page_ == 0, input)) search_settings_page_ = 0;
                if (button(L"Structure catalog", {content_left + 256.0F, y, content_left + 504.0F, y + 30.0F},
                    search_settings_page_ == 1, input)) search_settings_page_ = 1;
                y += 42.0F;
                if (search_settings_page_ == 0)
                {
                    text_input(L"Live search: name / tribe / class", settings.esp_search, 1, content_left, y, input, 128);
                    text_input(L"Hidden tribes (separate with ;)", settings.hidden_tribes, 2, content_left, y, input, 256);
                    text_input(L"Hidden dino types (class/name; separated)", settings.hidden_dino_types, 3,
                        content_left, y, input, 512);
                    text_input(L"Legacy hidden structures (used when catalog mode is off)",
                        settings.hidden_structure_types, 4, content_left, y, input, 512);
                    text_input(L"Grouped structure types (empty = all)", settings.grouped_structure_types, 5,
                        content_left, y, input, 512);
                    text(L"Filters apply live to ESP, radar grouping and summaries. They never alter game state.",
                        {content_left + 2.0F, y + 4.0F, frame.right - 32.0F, y + 44.0F}, text_secondary, 12.0F);
                }
                else
                {
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= structure_catalog_refresh_at_)
                    {
                        structure_catalog_refresh_at_ = now + std::chrono::seconds(1);
                        std::unordered_map<std::wstring, std::size_t> catalog_index;
                        catalog_index.reserve(structure_catalog_.size() + 64);
                        for (std::size_t index = 0; index < structure_catalog_.size(); ++index)
                            catalog_index.emplace(lower_copy(structure_catalog_[index].class_name), index);
                        for (const auto& known_class : exact_tokens(settings.known_structure_types))
                        {
                            const std::wstring lowered = lower_copy(known_class);
                            if (!catalog_index.contains(lowered))
                            {
                                catalog_index.emplace(lowered, structure_catalog_.size());
                                structure_catalog_.push_back({known_class, pretty_structure_class(known_class), 0});
                            }
                        }
                        for (auto& item : structure_catalog_) item.live_instances = 0;
                        for (const Actor& actor : runtime.snapshot().actors)
                        {
                            if (actor.kind != ActorKind::structure || actor.class_name.empty()) continue;
                            const std::wstring lowered = lower_copy(actor.class_name);
                            const auto found = catalog_index.find(lowered);
                            if (found == catalog_index.end())
                            {
                                catalog_index.emplace(lowered, structure_catalog_.size());
                                structure_catalog_.push_back({actor.class_name,
                                    pretty_structure_class(actor.class_name), 1});
                            }
                            else ++structure_catalog_[found->second].live_instances;
                        }
                        std::sort(structure_catalog_.begin(), structure_catalog_.end(),
                            [](const StructureCatalogItem& left, const StructureCatalogItem& right) {
                                const std::wstring left_name = lower_copy(left.display_name);
                                const std::wstring right_name = lower_copy(right.display_name);
                                return left_name != right_name ? left_name < right_name :
                                    lower_copy(left.class_name) < lower_copy(right.class_name);
                            });
                        settings.known_structure_types.clear();
                        for (const auto& item : structure_catalog_)
                        {
                            if (!settings.known_structure_types.empty()) settings.known_structure_types += L';';
                            settings.known_structure_types += item.class_name;
                        }
                    }

                    checkbox(L"Use selected structure list", settings.structure_whitelist_enabled,
                        content_left, y, input);
                    if (text_input(L"Search build name or class", structure_catalog_search_, 31,
                        content_left, y, input, 96)) structure_catalog_page_ = 0;
                    const std::wstring catalog_search = lower_copy(structure_catalog_search_);
                    std::vector<StructureCatalogItem*> filtered;
                    for (auto& item : structure_catalog_)
                    {
                        if (!catalog_search.empty() &&
                            lower_copy(item.display_name).find(catalog_search) == std::wstring::npos &&
                            lower_copy(item.class_name).find(catalog_search) == std::wstring::npos) continue;
                        filtered.push_back(&item);
                    }
                    if (button(L"Enable filtered", {content_left, y, content_left + 162.0F, y + 32.0F},
                        true, input))
                        for (const auto* item : filtered)
                            set_exact_token(settings.selected_structure_types, item->class_name, true);
                    if (button(L"Disable filtered", {content_left + 170.0F, y, content_left + 332.0F, y + 32.0F},
                        false, input))
                        for (const auto* item : filtered)
                            set_exact_token(settings.selected_structure_types, item->class_name, false);
                    if (button(L"Clear search", {content_left + 340.0F, y, content_left + 502.0F, y + 32.0F},
                        false, input))
                    {
                        structure_catalog_search_.clear();
                        structure_catalog_page_ = 0;
                    }
                    y += 40.0F;
                    const std::size_t selected_count = exact_tokens(settings.selected_structure_types).size();
                    int live_types{};
                    for (const auto& item : structure_catalog_) if (item.live_instances > 0) ++live_types;
                    text(L"Selected " + std::to_wstring(selected_count) + L" / catalog " +
                        std::to_wstring(structure_catalog_.size()) + L" · live types " +
                        std::to_wstring(live_types),
                        {content_left + 2.0F, y, content_left + 500.0F, y + 22.0F}, text_secondary, 11.0F);
                    y += 25.0F;

                    constexpr int structures_per_page = 6;
                    const int page_count = std::max(1, static_cast<int>(
                        (filtered.size() + structures_per_page - 1) / structures_per_page));
                    structure_catalog_page_ = std::clamp(structure_catalog_page_, 0, page_count - 1);
                    const int begin = structure_catalog_page_ * structures_per_page;
                    const int end = std::min(static_cast<int>(filtered.size()), begin + structures_per_page);
                    if (filtered.empty())
                    {
                        text(structure_catalog_.empty() ?
                            L"Catalog will populate from the next live world scan." : L"No structures match this search.",
                            {content_left + 2.0F, y, frame.right - 32.0F, y + 42.0F}, text_secondary, 12.0F);
                        y += 48.0F;
                    }
                    for (int index = begin; index < end; ++index)
                    {
                        const StructureCatalogItem& item = *filtered[static_cast<std::size_t>(index)];
                        bool selected = exact_token_contains(settings.selected_structure_types, item.class_name);
                        if (checkbox(item.display_name + (item.live_instances > 0 ?
                            L"  [" + std::to_wstring(item.live_instances) + L"]" : L""),
                            selected, content_left, y, input))
                            set_exact_token(settings.selected_structure_types, item.class_name, selected);
                        text(item.class_name, {content_left + 31.0F, y - 9.0F,
                            content_left + content_width_ - 10.0F, y + 9.0F}, text_secondary, 9.0F);
                        y += 12.0F;
                    }
                    if (page_count > 1)
                    {
                        if (button(L"Previous", {content_left, y, content_left + 110.0F, y + 30.0F}, true, input))
                            structure_catalog_page_ = (structure_catalog_page_ + page_count - 1) % page_count;
                        if (button(L"Next", {content_left + 120.0F, y, content_left + 230.0F, y + 30.0F}, true, input))
                            structure_catalog_page_ = (structure_catalog_page_ + 1) % page_count;
                        text(std::to_wstring(structure_catalog_page_ + 1) + L" / " + std::to_wstring(page_count),
                            {content_left + 250.0F, y, frame.right - 32.0F, y + 30.0F}, text_secondary, 11.0F);
                    }
                }
            }
            else
            {
                static constexpr const wchar_t* color_targets[]{
                    L"Menu accent", L"Own tribe", L"Allies", L"Enemies", L"Player awake",
                    L"Player sleeping", L"Player knocked out", L"Player dead", L"Player occluded",
                    L"Wild dinos", L"Structures", L"Health", L"Torpor", L"Local chams"};
                combo(L"Color target", color_target_, color_targets, std::size(color_targets), 19,
                    content_left, y, input);
                Color* selected = color_target_ == 0 ? &settings.menu_accent_color :
                    color_target_ == 1 ? &settings.own_color :
                    color_target_ == 2 ? &settings.ally_color :
                    color_target_ == 3 ? &settings.enemy_color :
                    color_target_ == 4 ? &settings.player_awake_color :
                    color_target_ == 5 ? &settings.player_sleeping_color :
                    color_target_ == 6 ? &settings.player_knocked_out_color :
                    color_target_ == 7 ? &settings.player_dead_color :
                    color_target_ == 8 ? &settings.player_occluded_color :
                    color_target_ == 9 ? &settings.wild_color :
                    color_target_ == 10 ? &settings.structure_color :
                    color_target_ == 11 ? &settings.health_color :
                    color_target_ == 12 ? &settings.torpor_color : &settings.local_chams_color;
                fill({content_left + 2.0F, y, content_left + 500.0F, y + 34.0F},
                    {selected->r, selected->g, selected->b, 1.0F});
                stroke({content_left + 2.0F, y, content_left + 500.0F, y + 34.0F}, text_secondary);
                y += 48.0F;
                slider(L"Red", selected->r, 0.0F, 1.0F, content_left, y, input);
                slider(L"Green", selected->g, 0.0F, 1.0F, content_left, y, input);
                slider(L"Blue", selected->b, 0.0F, 1.0F, content_left, y, input);
                slider(L"Opacity", selected->a, 0.10F, 1.0F, content_left, y, input);
                text(L"All color changes are live and saved with the current configuration.",
                    {content_left + 2.0F, y + 4.0F, frame.right - 32.0F, y + 42.0F}, text_secondary, 12.0F);
            }
            // Preview is a dedicated side window and remains visible for every ESP
            // section. It follows the menu and stays on its right whenever space allows.
            preview_left_ = frame.right + 12.0F;
            if (preview_left_ + 510.0F > width_ - 8.0F) preview_left_ = frame.left - 522.0F;
            preview_left_ = std::clamp(preview_left_, 8.0F, std::max(8.0F, width_ - 518.0F));
            preview_top_ = std::clamp(frame.top + 82.0F, 8.0F, std::max(8.0F, height_ - 416.0F));
            draw_esp_preview(settings, preview_left_, preview_top_, input);
        }
        else if (active_tab_ == 2)
        {
            if (button(L"Movement", {content_left, y, content_left + 248.0F, y + 30.0F},
                camera_settings_page_ == 0, input)) camera_settings_page_ = 0;
            if (button(L"Optics", {content_left + 256.0F, y, content_left + 504.0F, y + 30.0F},
                camera_settings_page_ == 1, input)) camera_settings_page_ = 1;
            y += 42.0F;
            if (camera_settings_page_ == 0)
            {
                toggle(L"Free camera", settings.freecam, content_left, y, input);
                slider(L"Freecam speed", settings.freecam_speed, 100.0F, 5000.0F, content_left, y, input, L" uu/s");
                slider(L"Shift speed multiplier", settings.freecam_sprint_multiplier, 1.0F, 10.0F, content_left, y, input, L" x");
                slider(L"Vertical speed multiplier", settings.freecam_vertical_multiplier, 0.1F, 5.0F, content_left, y, input, L" x");
                slider(L"Camera smoothing", settings.freecam_smoothing, 0.0F, 0.5F, content_left, y, input, L" s");
                slider(L"Mouse sensitivity", settings.freecam_mouse_sensitivity, 0.01F, 0.5F, content_left, y, input);
            }
            else
            {
                toggle(L"FOV override", settings.fov_override, content_left, y, input);
                slider(L"Camera FOV", settings.camera_fov, 50.0F, 170.0F, content_left, y, input, L" deg");
                text(L"Custom FOV pauses automatically while aiming down sights and resumes after ADS.",
                    {content_left, y + 2.0F, frame.right - 32.0F, y + 34.0F}, text_secondary, 12.0F);
                y += 36.0F;
                toggle(L"No recoil", settings.no_recoil, content_left, y, input);
                toggle(L"No weapon sway", settings.no_sway, content_left, y, input);
                text(L"Recoil removes firing kick; sway removes aim drift and weapon bob. Spread is unchanged.",
                    {content_left, y + 8.0F, frame.right - 32.0F, y + 42.0F}, text_secondary, 12.0F);
                y += 44.0F;
                text(L"Freecam: WASD, Q/E or Ctrl/Space vertical, Shift sprint, wheel changes speed, raw mouse rotates.",
                    {content_left, y + 8.0F, frame.right - 32.0F, y + 52.0F}, warning, 12.0F);
            }
        }
        else if (active_tab_ == 3)
        {
            checkbox(L"Runtime debug panel", settings.debug_panel, content_left, y, input);
            slider(L"Live actor refresh", settings.refresh_interval_ms, 16.0F, 500.0F, content_left, y, input, L" ms");
            slider(L"World actor discovery", settings.discovery_interval_ms, 250.0F, 5000.0F, content_left, y, input, L" ms");
            slider(L"Discovery frame budget", settings.discovery_budget_ms, 1.0F, 20.0F, content_left, y, input, L" ms");
            text(L"Positions, vitals and bones use the fast path; class discovery uses the slow path.",
                {content_left, y + 2.0F, frame.right - 32.0F, y + 42.0F}, text_secondary, 12.0F);
            y += 48.0F;
            if (button(L"Save configuration", {content_left, y, content_left + 190.0F, y + 40.0F}, true, input))
            {
                settings.normalize();
                settings.save(settings_path);
                toast_ = L"Configuration saved";
                toast_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            }
            y += 54.0F;

            text(L"LOCAL PROFILES", {content_left + 2.0F, y, frame.right - 32.0F, y + 24.0F}, accent, 11.0F);
            y += 28.0F;
            text_input(L"Profile name", profile_name_, 30, content_left, y, input, 32);

            const std::filesystem::path profiles_directory = settings_path.parent_path() / L"profiles";
            std::vector<std::filesystem::path> profiles;
            std::error_code profile_error;
            if (std::filesystem::exists(profiles_directory, profile_error))
            {
                for (std::filesystem::directory_iterator iterator(profiles_directory, profile_error), end;
                    !profile_error && iterator != end; iterator.increment(profile_error))
                {
                    if (iterator->is_regular_file(profile_error) && iterator->path().extension() == L".ini")
                        profiles.push_back(iterator->path());
                }
            }
            std::sort(profiles.begin(), profiles.end(), [](const auto& left, const auto& right) {
                return lower_copy(left.filename().wstring()) < lower_copy(right.filename().wstring());
            });
            if (profiles.empty()) profile_index_ = 0;
            else profile_index_ %= profiles.size();

            if (button(L"Save profile", {content_left, y, content_left + 156.0F, y + 38.0F}, true, input))
            {
                const std::wstring safe_name = safe_profile_name(profile_name_);
                std::error_code create_error;
                std::filesystem::create_directories(profiles_directory, create_error);
                if (safe_name.empty() || create_error)
                    toast_ = L"Profile name or folder is invalid";
                else
                {
                    settings.normalize();
                    const bool saved = settings.save(profiles_directory / (safe_name + L".ini"));
                    profile_name_ = safe_name;
                    toast_ = saved ? L"Profile saved: " + safe_name : L"Could not save profile";
                }
                toast_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            }
            if (button(L"Load profile", {content_left + 166.0F, y, content_left + 322.0F, y + 38.0F}, true, input))
            {
                const std::wstring safe_name = safe_profile_name(profile_name_);
                Settings loaded;
                if (!safe_name.empty() && loaded.load(profiles_directory / (safe_name + L".ini")))
                {
                    loaded.menu_open = true;
                    settings = std::move(loaded);
                    profile_name_ = safe_name;
                    toast_ = L"Profile loaded: " + safe_name;
                }
                else toast_ = L"Profile not found: " + safe_name;
                toast_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            }
            y += 48.0F;
            if (!profiles.empty())
            {
                if (button(L"Previous", {content_left, y, content_left + 110.0F, y + 34.0F}, true, input))
                {
                    profile_index_ = (profile_index_ + profiles.size() - 1) % profiles.size();
                    profile_name_ = profiles[profile_index_].stem().wstring();
                    profile_delete_confirmation_.clear();
                }
                if (button(L"Next", {content_left + 120.0F, y, content_left + 230.0F, y + 34.0F}, true, input))
                {
                    profile_index_ = (profile_index_ + 1) % profiles.size();
                    profile_name_ = profiles[profile_index_].stem().wstring();
                    profile_delete_confirmation_.clear();
                }
                text(L"Selected: " + profiles[profile_index_].stem().wstring(),
                    {content_left + 242.0F, y + 3.0F, frame.right - 32.0F, y + 32.0F}, text_secondary, 12.0F);
                y += 44.0F;
                const std::filesystem::path selected_profile = profiles[profile_index_];
                const std::wstring selected_name = selected_profile.stem().wstring();
                const auto confirmation_now = std::chrono::steady_clock::now();
                if (confirmation_now >= profile_delete_confirmation_until_)
                    profile_delete_confirmation_.clear();
                const bool confirming = profile_delete_confirmation_ == selected_name;
                if (button(confirming ? L"Confirm delete " + selected_name : L"Delete selected profile",
                    {content_left, y, content_left + 250.0F, y + 36.0F}, false, input))
                {
                    if (!confirming)
                    {
                        profile_delete_confirmation_ = selected_name;
                        profile_delete_confirmation_until_ = confirmation_now + std::chrono::seconds(6);
                        toast_ = L"Press delete again to confirm: " + selected_name;
                    }
                    else
                    {
                        std::error_code delete_error;
                        const bool safe_target = selected_profile.parent_path() == profiles_directory &&
                            selected_profile.extension() == L".ini" &&
                            !std::filesystem::is_symlink(selected_profile, delete_error);
                        const bool removed = safe_target && !delete_error &&
                            std::filesystem::remove(selected_profile, delete_error);
                        toast_ = removed && !delete_error ? L"Profile deleted: " + selected_name :
                            L"Could not delete profile: " + selected_name;
                        if (removed)
                        {
                            profile_name_ = L"default";
                            profile_index_ = 0;
                        }
                        profile_delete_confirmation_.clear();
                    }
                    toast_until_ = confirmation_now + std::chrono::seconds(3);
                }
                text(L"Only the selected file inside profiles\\ is removed; the base configuration is protected.",
                    {content_left + 262.0F, y, frame.right - 32.0F, y + 38.0F}, text_secondary, 10.0F);
            }
            else
                text(L"No saved profiles yet.", {content_left + 2.0F, y,
                    frame.right - 32.0F, y + 32.0F}, text_secondary, 12.0F);
        }
        else if (active_tab_ == 4)
        {
            static constexpr const wchar_t* styles[]{L"Solid", L"Wireframe", L"Solid + wireframe"};
            checkbox(L"First-person hands + weapon", settings.local_chams, content_left, y, input);
            combo(L"Local chams style", settings.local_chams_style, styles, std::size(styles), 20,
                content_left, y, input);
            fill({content_left + 2.0F, y, content_left + 500.0F, y + 24.0F},
                {settings.local_chams_color.r, settings.local_chams_color.g,
                    settings.local_chams_color.b, 1.0F});
            stroke({content_left + 2.0F, y, content_left + 500.0F, y + 24.0F}, text_secondary);
            y += 34.0F;
            slider(L"Red", settings.local_chams_color.r, 0.0F, 1.0F, content_left, y, input);
            slider(L"Green", settings.local_chams_color.g, 0.0F, 1.0F, content_left, y, input);
            slider(L"Blue", settings.local_chams_color.b, 0.0F, 1.0F, content_left, y, input);
            slider(L"Opacity", settings.local_chams_color.a, 0.10F, 1.0F, content_left, y, input);
            text(runtime.chams_status(), {content_left + 2.0F, y + 4.0F, frame.right - 32.0F, y + 36.0F},
                runtime.chams_status().find(L"active") != std::wstring::npos ? success : warning, 12.0F);
            y += 44.0F;
            text(L"Only AShooterCharacter::Mesh1P and AShooterWeapon::Mesh1P are modified. Third-person body, enemies and dinos stay untouched; all flags are restored on disable, weapon swap and world change.",
                {content_left + 2.0F, y, frame.right - 32.0F, y + 72.0F}, text_secondary, 12.0F);
        }
        else if (active_tab_ == 5)
        {
            keybind(L"Menu toggle", settings.menu_key, BindingTarget::menu, content_left, y, input);
            keybind(L"Unload DLL", settings.unload_key, BindingTarget::unload, content_left, y, input);
            keybind(L"Aim activation", settings.aim_key, BindingTarget::aim, content_left, y, input);
            keybind(L"Dino aim activation", settings.dino_aim_key, BindingTarget::dino_aim, content_left, y, input);
            keybind(L"Freecam toggle", settings.freecam_key, BindingTarget::freecam, content_left, y, input);
            keybind(L"ESP toggle", settings.esp_toggle_key, BindingTarget::esp_toggle, content_left, y, input);
            keybind(L"Panic / restore", settings.panic_key, BindingTarget::panic, content_left, y, input);
            text(L"Click Rebind, then press a keyboard key or mouse button. Esc cancels.",
                {content_left, y + 8.0F, frame.right - 32.0F, y + 42.0F}, text_secondary, 12.0F);
            if (button(L"Save bindings", {content_left, y + 54.0F, content_left + 190.0F, y + 94.0F}, true, input))
            {
                settings.save(settings_path);
                toast_ = L"Bindings saved";
                toast_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            }
        }
        else if (active_tab_ == 6)
        {
            checkbox(L"Show active bind list", settings.show_hotkey_list, content_left, y, input);
            slider(L"List horizontal position", settings.hotkey_list_x, 0.05F, 0.95F, content_left, y, input);
            slider(L"List vertical position", settings.hotkey_list_y, 0.05F, 0.95F, content_left, y, input);
            text(L"CONFIGURED HOTKEYS", {content_left + 2.0F, y, frame.right - 32.0F, y + 24.0F}, accent, 11.0F);
            y += 28.0F;

            struct HotkeyEntry
            {
                std::wstring id;
                std::wstring label;
                std::wstring category;
                std::uint32_t key{};
                std::int32_t mode{};
                bool active{};
                bool shown{};
                bool special{};
                bool visibility_control{};
            };
            std::vector<HotkeyEntry> entries;
            entries.push_back({L"system.menu", L"Menu toggle", L"System", settings.menu_key,
                1, settings.menu_open, false, true, false});
            entries.push_back({L"system.unload", L"Unload DLL", L"System", settings.unload_key,
                -1, false, false, true, false});
            entries.push_back({L"system.freecam", L"Quick freecam toggle", L"System", settings.freecam_key,
                1, settings.freecam, false, true, false});
            entries.push_back({L"system.esp", L"Quick ESP toggle", L"System", settings.esp_toggle_key,
                1, settings.esp_enabled, false, true, false});
            entries.push_back({L"system.panic", L"Panic / restore", L"System", settings.panic_key,
                -1, false, false, true, false});
            entries.push_back({L"aim.player", L"Player aim", L"Aim", settings.aim_key,
                settings.aim_activation_mode, runtime.snapshot().player_aim_active, settings.aim_bind_show, true, true});
            entries.push_back({L"aim.dino", L"Dino aim", L"Aim", settings.dino_aim_key,
                settings.dino_aim_activation_mode, runtime.snapshot().dino_aim_active,
                settings.dino_aim_bind_show, true, true});
            for (const FeatureBinding& binding : settings.feature_bindings)
            {
                if (binding.key == 0) continue;
                const FeatureDescriptor* descriptor = feature_descriptor(binding.id);
                if (descriptor == nullptr) continue;
                const auto state = std::find_if(feature_binding_runtime_.begin(), feature_binding_runtime_.end(),
                    [&](const auto& item) { return item.id == binding.id; });
                entries.push_back({binding.id, descriptor->label, descriptor->category, binding.key, binding.mode,
                    state != feature_binding_runtime_.end() && state->active,
                    binding.show_in_list, false, true});
            }
            constexpr int entries_per_page = 6;
            const int page_count = std::max(1, static_cast<int>((entries.size() + entries_per_page - 1) / entries_per_page));
            hotkey_page_ = std::clamp(hotkey_page_, 0, page_count - 1);
            const int begin = hotkey_page_ * entries_per_page;
            const int end = std::min(static_cast<int>(entries.size()), begin + entries_per_page);
            if (entries.empty())
            {
                text(L"No feature binds yet. Right-click any checkbox or switch to add one.",
                    {content_left + 2.0F, y, frame.right - 32.0F, y + 42.0F}, text_secondary, 12.0F);
                y += 48.0F;
            }
            for (int index = begin; index < end; ++index)
            {
                const HotkeyEntry& entry = entries[static_cast<std::size_t>(index)];
                const Rect row{content_left, y, content_left + content_width_, y + 44.0F};
                fill(row, entry.active ? Color{accent_dim.r, accent_dim.g, accent_dim.b, 0.72F} : surface);
                fill({row.left, row.top, row.left + 3.0F, row.bottom}, entry.active ? success : accent_dim);
                text(entry.label, {row.left + 12.0F, row.top + 3.0F, row.left + 245.0F, row.top + 24.0F},
                    text_primary, 12.0F);
                text(entry.category + L" · " + key_name(entry.key) + L" · " +
                    (entry.mode < 0 ? L"Action" : entry.mode == 0 ? L"Hold" :
                        entry.mode == 1 ? L"Toggle" : L"Always"),
                    {row.left + 12.0F, row.top + 22.0F, row.left + 350.0F, row.bottom - 2.0F},
                    text_secondary, 10.0F);
                if (entry.visibility_control && button(entry.shown ? L"Eye" : L"Hidden",
                    {row.right - 158.0F, row.top + 7.0F, row.right - 82.0F, row.bottom - 7.0F}, entry.shown, input))
                {
                    if (entry.id == L"aim.player") settings.aim_bind_show = !settings.aim_bind_show;
                    else if (entry.id == L"aim.dino") settings.dino_aim_bind_show = !settings.dino_aim_bind_show;
                    else if (FeatureBinding* binding = settings.find_feature_binding(entry.id))
                        binding->show_in_list = !binding->show_in_list;
                }
                else if (!entry.visibility_control)
                    text(L"SYSTEM", {row.right - 158.0F, row.top + 7.0F,
                        row.right - 82.0F, row.bottom - 7.0F}, text_secondary, 9.0F, TextAlign::center);
                if (!entry.special && button(L"Remove",
                    {row.right - 76.0F, row.top + 7.0F, row.right - 8.0F, row.bottom - 7.0F}, false, input))
                {
                    const std::wstring id = entry.id;
                    std::erase_if(settings.feature_bindings, [&](const FeatureBinding& binding) { return binding.id == id; });
                    checkbox_binding_feature_id_.clear();
                    break;
                }
                y += 50.0F;
            }
            if (page_count > 1)
            {
                if (button(L"Previous", {content_left, y, content_left + 110.0F, y + 32.0F}, true, input))
                    hotkey_page_ = (hotkey_page_ + page_count - 1) % page_count;
                if (button(L"Next", {content_left + 120.0F, y, content_left + 230.0F, y + 32.0F}, true, input))
                    hotkey_page_ = (hotkey_page_ + 1) % page_count;
                text(std::to_wstring(hotkey_page_ + 1) + L" / " + std::to_wstring(page_count),
                    {content_left + 250.0F, y, frame.right - 32.0F, y + 32.0F}, text_secondary, 11.0F);
            }
        }
        else
        {
            if (button(L"Types", {content_left, y, content_left + 162.0F, y + 34.0F},
                alert_settings_page_ == 0, input)) alert_settings_page_ = 0;
            if (button(L"Tuning", {content_left + 170.0F, y, content_left + 332.0F, y + 34.0F},
                alert_settings_page_ == 1, input)) alert_settings_page_ = 1;
            y += 48.0F;
            checkbox(L"Enable alerts", settings.alerts_enabled, content_left, y, input);
            if (alert_settings_page_ == 0)
            {
                checkbox(L"New enemy player", settings.alert_new_player, content_left, y, input);
                checkbox(L"Enemy approaching", settings.alert_approach, content_left, y, input);
                checkbox(L"Enemy sleep transition", settings.alert_sleep, content_left, y, input);
                checkbox(L"Enemy death transition", settings.alert_death, content_left, y, input);
                checkbox(L"Noglin in radius", settings.alert_noglin, content_left, y, input);
                checkbox(L"Active targeting turret", settings.alert_turret, content_left, y, input);
                checkbox(L"Enemy group (3+)", settings.alert_enemy_group, content_left, y, input);
                checkbox(L"Notification sound", settings.alert_sound, content_left, y, input);
            }
            else
            {
                slider(L"General radius", settings.alert_radius_m, 25.0F, 2000.0F,
                    content_left, y, input, L" m");
                slider(L"Noglin radius", settings.alert_noglin_radius_m, 10.0F, 1000.0F,
                    content_left, y, input, L" m");
                slider(L"Approach speed", settings.alert_approach_speed_mps, 1.0F, 100.0F,
                    content_left, y, input, L" m/s");
                slider(L"Card lifetime", settings.alert_lifetime_s, 2.0F, 20.0F,
                    content_left, y, input, L" s");
                slider(L"Per-event cooldown", settings.alert_cooldown_s, 2.0F, 120.0F,
                    content_left, y, input, L" s");
                if (button(L"Clear active alerts", {content_left, y, content_left + 190.0F, y + 38.0F}, false, input))
                    runtime.clear_alert_history();
            }
        }

        fill({content_left, frame.bottom - 54.0F, frame.right - 28.0F, frame.bottom - 20.0F}, surface);
        text(runtime.status(), {content_left + 12.0F, frame.bottom - 49.0F, frame.right - 40.0F, frame.bottom - 24.0F},
            runtime.snapshot().local_valid ? success : warning, 12.0F);
        if (!toast_.empty() && std::chrono::steady_clock::now() < toast_until_)
            text(toast_, {frame.right - 220.0F, top + 28.0F, frame.right - 30.0F, top + 55.0F}, success, 12.0F, TextAlign::right);
        if (combo_popup_.visible)
        {
            fill(combo_popup_.rect, {0.025F, 0.018F, 0.037F, 0.995F});
            stroke(combo_popup_.rect, accent, 1.0F);
            for (std::size_t index = 0; index < combo_popup_.count; ++index)
            {
                const Rect option{combo_popup_.rect.left + 2.0F,
                    combo_popup_.rect.top + 2.0F + static_cast<float>(index) * 30.0F,
                    combo_popup_.rect.right - 2.0F,
                    combo_popup_.rect.top + 30.0F + static_cast<float>(index) * 30.0F};
                const bool selected = combo_popup_.selected == static_cast<std::int32_t>(index);
                const bool hovered = contains(option, input.frame_mouse_x, input.frame_mouse_y);
                fill(option, selected ? accent_dim : (hovered ? surface_hover : surface));
                if (selected) fill({option.left, option.top, option.left + 3.0F, option.bottom}, accent);
                text(combo_popup_.options[index], {option.left + 12.0F, option.top,
                    option.right - 8.0F, option.bottom}, selected ? text_primary : text_secondary, 12.0F);
            }
        }
        const Rect grip{frame.right - 20.0F, frame.bottom - 20.0F, frame.right - 5.0F, frame.bottom - 5.0F};
        line(grip.left + 4.0F, grip.bottom, grip.right, grip.top + 4.0F, accent_dim, 1.5F);
        line(grip.left + 9.0F, grip.bottom, grip.right, grip.top + 9.0F, accent, 1.5F);
    }

    void Overlay::draw_esp_preview(Settings& settings, const float x, const float y, InputState& input)
    {
        const Rect preview{x, y, x + 510.0F, y + 408.0F};
        fill(preview, {0.025F, 0.018F, 0.037F, 0.98F});
        stroke(preview, {0.220F, 0.145F, 0.365F, 1.0F});
        text(L"PLAYER ESP PREVIEW", {x + 14.0F, y + 7.0F, x + 496.0F, y + 28.0F},
            accent, 12.0F, TextAlign::center);
        text(L"Drag NAME, STATUS, HP or TORPOR to a highlighted anchor", {x + 14.0F, y + 28.0F, x + 496.0F, y + 49.0F},
            text_secondary, 11.0F, TextAlign::center);
        if (button(preview_occluded_ ? L"OCCLUDED" : L"VISIBLE",
            {x + 394.0F, y + 51.0F, x + 496.0F, y + 74.0F}, preview_occluded_, input))
        {
            preview_occluded_ = !preview_occluded_;
        }

        const Rect model{x + 203.0F, y + 78.0F, x + 307.0F, y + 340.0F};
        const Rect zones[]{
            {model.left - 54.0F, model.top - 40.0F, model.right + 54.0F, model.top - 8.0F},
            {model.left - 150.0F, model.top + 34.0F, model.left - 18.0F, model.bottom - 34.0F},
            {model.right + 18.0F, model.top + 34.0F, model.right + 150.0F, model.bottom - 34.0F},
            {model.left - 54.0F, model.bottom + 8.0F, model.right + 54.0F, model.bottom + 40.0F}
        };
        for (const Rect& zone : zones)
        {
            const bool hovered = contains(zone, input.frame_mouse_x, input.frame_mouse_y);
            fill(zone, hovered && preview_drag_ != PreviewDrag::none ?
                Color{accent_dim.r, accent_dim.g, accent_dim.b, 0.55F} :
                Color{surface.r, surface.g, surface.b, 0.30F});
            stroke(zone, hovered && preview_drag_ != PreviewDrag::none ? accent :
                Color{0.12F, 0.17F, 0.24F, 0.8F});
        }

        const Color normal_preview_base = settings.player_color_source == 1 ?
            settings.player_awake_color : settings.enemy_color;
        const Color preview_base = preview_occluded_ && settings.player_occluded_color_enabled ?
            settings.player_occluded_color : normal_preview_base;
        const Color preview_color{preview_base.r, preview_base.g, preview_base.b, settings.esp_opacity};
        if (settings.esp_box_style == 0)
        {
            stroke(model, preview_color, settings.esp_box_thickness);
        }
        else
        {
            constexpr float cx = 28.0F;
            constexpr float cy = 42.0F;
            line(model.left, model.top, model.left + cx, model.top, preview_color, settings.esp_box_thickness);
            line(model.left, model.top, model.left, model.top + cy, preview_color, settings.esp_box_thickness);
            line(model.right - cx, model.top, model.right, model.top, preview_color, settings.esp_box_thickness);
            line(model.right, model.top, model.right, model.top + cy, preview_color, settings.esp_box_thickness);
            line(model.left, model.bottom - cy, model.left, model.bottom, preview_color, settings.esp_box_thickness);
            line(model.left, model.bottom, model.left + cx, model.bottom, preview_color, settings.esp_box_thickness);
            line(model.right, model.bottom - cy, model.right, model.bottom, preview_color, settings.esp_box_thickness);
            line(model.right - cx, model.bottom, model.right, model.bottom, preview_color, settings.esp_box_thickness);
        }

        const float center_x = (model.left + model.right) * 0.5F;
        const float shoulders = model.top + 72.0F;
        const float pelvis = model.top + 165.0F;
        line(center_x, model.top + 20.0F, center_x, pelvis, preview_color, settings.esp_skeleton_thickness);
        line(center_x, shoulders, model.left + 12.0F, model.top + 132.0F, preview_color, settings.esp_skeleton_thickness);
        line(center_x, shoulders, model.right - 12.0F, model.top + 132.0F, preview_color, settings.esp_skeleton_thickness);
        line(center_x, pelvis, model.left + 20.0F, model.bottom, preview_color, settings.esp_skeleton_thickness);
        line(center_x, pelvis, model.right - 20.0F, model.bottom, preview_color, settings.esp_skeleton_thickness);

        const auto label_rect_for = [&](const int side) -> Rect {
            if (side == 1) return {model.left - 154.0F, model.top + 82.0F, model.left - 18.0F, model.top + 112.0F};
            if (side == 2) return {model.right + 18.0F, model.top + 82.0F, model.right + 154.0F, model.top + 112.0F};
            if (side == 3) return {model.left - 48.0F, model.bottom + 10.0F, model.right + 48.0F, model.bottom + 38.0F};
            return {model.left - 48.0F, model.top - 38.0F, model.right + 48.0F, model.top - 10.0F};
        };
        const auto health_rect_for = [&](const int side) -> Rect {
            if (side == 1) return {model.left - 12.0F, model.top, model.left - 6.0F, model.bottom};
            if (side == 2) return {model.right + 6.0F, model.top, model.right + 12.0F, model.bottom};
            if (side == 3) return {model.left, model.bottom + 6.0F, model.right, model.bottom + 12.0F};
            return {model.left, model.top - 12.0F, model.right, model.top - 6.0F};
        };
        const auto status_rect_for = [&](const int side) -> Rect {
            constexpr float badge_width = 120.0F;
            if (side == 1)
            {
                float right = model.left - 18.0F;
                if (settings.show_held_items) right -= settings.esp_icon_size + 8.0F;
                const float top = model.top + (settings.esp_label_side == 1 ? 122.0F : 82.0F);
                return {right - badge_width, top, right, top + 23.0F};
            }
            if (side == 2)
            {
                float left = model.right + 18.0F;
                if (settings.show_equipment) left += settings.esp_icon_size + 8.0F;
                const float top = model.top + (settings.esp_label_side == 2 ? 122.0F : 82.0F);
                return {left, top, left + badge_width, top + 23.0F};
            }
            if (side == 3)
            {
                float top = model.bottom + 14.0F;
                if (settings.esp_label_side == 3) top += 30.0F;
                return {center_x - badge_width * 0.5F, top,
                    center_x + badge_width * 0.5F, top + 23.0F};
            }
            float bottom = model.top - 14.0F;
            if (settings.esp_label_side == 0)
                return {center_x - badge_width * 0.5F, model.top + 10.0F,
                    center_x + badge_width * 0.5F, model.top + 33.0F};
            return {center_x - badge_width * 0.5F, bottom - 23.0F,
                center_x + badge_width * 0.5F, bottom};
        };

        Rect label_rect = label_rect_for(settings.esp_label_side);
        Rect health_rect = health_rect_for(settings.esp_health_side);
        Rect torpor_rect = health_rect_for(settings.esp_torpor_side);
        Rect status_rect = status_rect_for(settings.esp_status_side);
        if (settings.esp_torpor_side == settings.esp_health_side)
        {
            if (settings.esp_torpor_side == 1) { torpor_rect.left -= 6.0F; torpor_rect.right -= 6.0F; }
            else if (settings.esp_torpor_side == 2) { torpor_rect.left += 6.0F; torpor_rect.right += 6.0F; }
            else if (settings.esp_torpor_side == 0) { torpor_rect.top -= 6.0F; torpor_rect.bottom -= 6.0F; }
            else { torpor_rect.top += 6.0F; torpor_rect.bottom += 6.0F; }
        }
        if (preview_drag_ == PreviewDrag::none && consume_click(label_rect, input))
            preview_drag_ = PreviewDrag::label;
        else if (preview_drag_ == PreviewDrag::none && consume_click(health_rect, input))
            preview_drag_ = PreviewDrag::health;
        else if (preview_drag_ == PreviewDrag::none && consume_click(torpor_rect, input))
            preview_drag_ = PreviewDrag::torpor;
        else if (settings.show_player_status && preview_drag_ == PreviewDrag::none && consume_click(status_rect, input))
            preview_drag_ = PreviewDrag::status;

        if (preview_drag_ != PreviewDrag::none && !input.frame_left_down)
        {
            const float dx = static_cast<float>(input.frame_mouse_x) - center_x;
            const float dy = static_cast<float>(input.frame_mouse_y) - (model.top + model.bottom) * 0.5F;
            const int side = std::abs(dx) > std::abs(dy) ? (dx < 0.0F ? 1 : 2) : (dy < 0.0F ? 0 : 3);
            if (preview_drag_ == PreviewDrag::label) settings.esp_label_side = side;
            else if (preview_drag_ == PreviewDrag::health) settings.esp_health_side = side;
            else if (preview_drag_ == PreviewDrag::torpor) settings.esp_torpor_side = side;
            else settings.esp_status_side = side;
            preview_drag_ = PreviewDrag::none;
            label_rect = label_rect_for(settings.esp_label_side);
            health_rect = health_rect_for(settings.esp_health_side);
            torpor_rect = health_rect_for(settings.esp_torpor_side);
            status_rect = status_rect_for(settings.esp_status_side);
        }

        fill(label_rect, {surface.r, surface.g, surface.b, 0.94F});
        stroke(label_rect, preview_drag_ == PreviewDrag::label ? accent : preview_color);
        text(L"ENEMY [TRIBE] 124m", label_rect, text_primary, 11.0F, TextAlign::center);
        if (settings.show_player_status)
        {
            const Color status_color{settings.player_awake_color.r, settings.player_awake_color.g,
                settings.player_awake_color.b, settings.esp_opacity};
            fill(status_rect, {0.018F, 0.012F, 0.028F, 0.92F});
            stroke(status_rect, preview_drag_ == PreviewDrag::status ? accent : status_color);
            text(L"ALIVE / AWAKE", status_rect, status_color, 10.0F, TextAlign::center);
        }
        fill(health_rect, {0.03F, 0.03F, 0.03F, 0.9F});
        Rect health_fill = health_rect;
        if (settings.esp_health_side == 1 || settings.esp_health_side == 2)
            health_fill.top = health_rect.top + (health_rect.bottom - health_rect.top) * 0.28F;
        else
            health_fill.right = health_rect.left + (health_rect.right - health_rect.left) * 0.72F;
        fill(health_fill, {settings.health_color.r, settings.health_color.g,
            settings.health_color.b, settings.esp_opacity});
        stroke(health_rect, preview_drag_ == PreviewDrag::health ? accent : text_secondary);
        fill(torpor_rect, {0.03F, 0.03F, 0.03F, 0.9F});
        Rect torpor_fill = torpor_rect;
        if (settings.esp_torpor_side == 1 || settings.esp_torpor_side == 2)
            torpor_fill.top = torpor_rect.top + (torpor_rect.bottom - torpor_rect.top) * 0.46F;
        else
            torpor_fill.right = torpor_rect.left + (torpor_rect.right - torpor_rect.left) * 0.54F;
        fill(torpor_fill, {settings.torpor_color.r, settings.torpor_color.g,
            settings.torpor_color.b, settings.esp_opacity});
        stroke(torpor_rect, preview_drag_ == PreviewDrag::torpor ? accent : text_secondary);

        if (preview_drag_ != PreviewDrag::none && input.frame_left_down)
        {
            const float mx = static_cast<float>(input.frame_mouse_x);
            const float my = static_cast<float>(input.frame_mouse_y);
            const Rect ghost{mx - 62.0F, my - 14.0F, mx + 62.0F, my + 14.0F};
            fill(ghost, {accent_dim.r, accent_dim.g, accent_dim.b, 0.90F});
            stroke(ghost, accent, 1.5F);
            const wchar_t* ghost_label = preview_drag_ == PreviewDrag::label ? L"NAME" :
                (preview_drag_ == PreviewDrag::health ? L"HP" :
                    (preview_drag_ == PreviewDrag::torpor ? L"TORPOR" : L"STATUS"));
            text(ghost_label, ghost, text_primary, 12.0F, TextAlign::center);
        }
    }

    void Overlay::draw_debug(const ArkRuntime& runtime)
    {
        const Snapshot& snapshot = runtime.snapshot();
        const Rect rect{width_ - 330.0F, 18.0F, width_ - 18.0F, 112.0F};
        fill(rect, {0.02F, 0.03F, 0.05F, 0.82F});
        text(L"KOPT INTERNAL", {rect.left + 12.0F, rect.top + 8.0F, rect.right - 12.0F, rect.top + 30.0F}, accent, 12.0F);
        text(runtime.status(), {rect.left + 12.0F, rect.top + 31.0F, rect.right - 12.0F, rect.top + 52.0F}, text_primary, 12.0F);
        text(L"Capture #" + std::to_wstring(snapshot.captures) + L"  |  Home: menu",
            {rect.left + 12.0F, rect.top + 52.0F, rect.right - 12.0F, rect.top + 72.0F}, text_secondary, 11.0F);
        text(L"Aim " + std::wstring(snapshot.aim_active ? L"ACTIVE" : L"idle") +
            L"  armed=" + (snapshot.aim_armed ? L"yes" : L"no") +
            L"  target=" + (snapshot.aim_target != 0 ? L"yes" : L"no"),
            {rect.left + 12.0F, rect.top + 72.0F, rect.right - 12.0F, rect.bottom - 5.0F},
            snapshot.aim_active ? warning : text_secondary, 11.0F);
    }

    bool Overlay::flush()
    {
        if (vertices_.empty()) return true;
        if (vertices_.size() > vertex_capacity_)
        {
            while (vertex_capacity_ < vertices_.size()) vertex_capacity_ *= 2;
            vertex_buffer_.reset();
            D3D11_BUFFER_DESC description{};
            description.ByteWidth = static_cast<UINT>(vertex_capacity_ * sizeof(Vertex));
            description.Usage = D3D11_USAGE_DYNAMIC;
            description.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(device_->CreateBuffer(&description, nullptr, vertex_buffer_.put()))) return false;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(vertex_buffer_.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
        std::memcpy(mapped.pData, vertices_.data(), vertices_.size() * sizeof(Vertex));
        context_->Unmap(vertex_buffer_.get(), 0);
        const float screen[4]{width_, height_, 0.0F, 0.0F};
        context_->UpdateSubresource(screen_buffer_.get(), 0, nullptr, screen, 0, 0);

        ID3D11RenderTargetView* old_rtv_raw{};
        ID3D11DepthStencilView* old_dsv_raw{};
        context_->OMGetRenderTargets(1, &old_rtv_raw, &old_dsv_raw);
        ComPtr<ID3D11RenderTargetView> old_rtv(old_rtv_raw);
        ComPtr<ID3D11DepthStencilView> old_dsv(old_dsv_raw);
        ID3D11BlendState* old_blend_raw{};
        float old_blend_factor[4]{};
        UINT old_sample_mask{};
        context_->OMGetBlendState(&old_blend_raw, old_blend_factor, &old_sample_mask);
        ComPtr<ID3D11BlendState> old_blend(old_blend_raw);
        ID3D11DepthStencilState* old_depth_raw{};
        UINT old_stencil_ref{};
        context_->OMGetDepthStencilState(&old_depth_raw, &old_stencil_ref);
        ComPtr<ID3D11DepthStencilState> old_depth(old_depth_raw);
        ID3D11RasterizerState* old_raster_raw{};
        context_->RSGetState(&old_raster_raw);
        ComPtr<ID3D11RasterizerState> old_raster(old_raster_raw);
        std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> old_viewports{};
        UINT old_viewport_count = static_cast<UINT>(old_viewports.size());
        context_->RSGetViewports(&old_viewport_count, old_viewports.data());
        std::array<D3D11_RECT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> old_scissors{};
        UINT old_scissor_count = static_cast<UINT>(old_scissors.size());
        context_->RSGetScissorRects(&old_scissor_count, old_scissors.data());
        ID3D11InputLayout* old_layout_raw{};
        context_->IAGetInputLayout(&old_layout_raw);
        ComPtr<ID3D11InputLayout> old_layout(old_layout_raw);
        D3D11_PRIMITIVE_TOPOLOGY old_topology{};
        context_->IAGetPrimitiveTopology(&old_topology);
        ID3D11Buffer* old_vertex_raw{};
        UINT old_stride{}, old_offset{};
        context_->IAGetVertexBuffers(0, 1, &old_vertex_raw, &old_stride, &old_offset);
        ComPtr<ID3D11Buffer> old_vertex(old_vertex_raw);
        ID3D11Buffer* old_index_raw{};
        DXGI_FORMAT old_index_format{};
        UINT old_index_offset{};
        context_->IAGetIndexBuffer(&old_index_raw, &old_index_format, &old_index_offset);
        ComPtr<ID3D11Buffer> old_index(old_index_raw);
        ID3D11VertexShader* old_vs_raw{};
        context_->VSGetShader(&old_vs_raw, nullptr, nullptr);
        ComPtr<ID3D11VertexShader> old_vs(old_vs_raw);
        ID3D11PixelShader* old_ps_raw{};
        context_->PSGetShader(&old_ps_raw, nullptr, nullptr);
        ComPtr<ID3D11PixelShader> old_ps(old_ps_raw);
        ID3D11Buffer* old_constant_raw{};
        context_->VSGetConstantBuffers(0, 1, &old_constant_raw);
        ComPtr<ID3D11Buffer> old_constant(old_constant_raw);
        ID3D11ShaderResourceView* old_resource_raw{};
        context_->PSGetShaderResources(0, 1, &old_resource_raw);
        ComPtr<ID3D11ShaderResourceView> old_resource(old_resource_raw);
        ID3D11SamplerState* old_sampler_raw{};
        context_->PSGetSamplers(0, 1, &old_sampler_raw);
        ComPtr<ID3D11SamplerState> old_sampler(old_sampler_raw);

        ID3D11RenderTargetView* rtv = render_target_.get();
        context_->OMSetRenderTargets(1, &rtv, nullptr);
        const float blend_factor[4]{};
        context_->OMSetBlendState(blend_state_.get(), blend_factor, 0xFFFFFFFF);
        context_->OMSetDepthStencilState(depth_state_.get(), 0);
        context_->RSSetState(rasterizer_state_.get());
        const D3D11_VIEWPORT viewport{0.0F, 0.0F, width_, height_, 0.0F, 1.0F};
        context_->RSSetViewports(1, &viewport);
        const UINT stride = sizeof(Vertex);
        const UINT offset = 0;
        ID3D11Buffer* vertex = vertex_buffer_.get();
        context_->IASetInputLayout(input_layout_.get());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->IASetVertexBuffers(0, 1, &vertex, &stride, &offset);
        context_->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
        context_->VSSetShader(vertex_shader_.get(), nullptr, 0);
        ID3D11Buffer* screen_buffer = screen_buffer_.get();
        context_->VSSetConstantBuffers(0, 1, &screen_buffer);
        context_->PSSetShader(pixel_shader_.get(), nullptr, 0);
        ID3D11ShaderResourceView* font_view = font_view_.get();
        context_->PSSetShaderResources(0, 1, &font_view);
        ID3D11SamplerState* sampler = sampler_.get();
        context_->PSSetSamplers(0, 1, &sampler);
        context_->Draw(static_cast<UINT>(vertices_.size()), 0);

        ID3D11RenderTargetView* restore_rtv = old_rtv.get();
        context_->OMSetRenderTargets(1, &restore_rtv, old_dsv.get());
        context_->OMSetBlendState(old_blend.get(), old_blend_factor, old_sample_mask);
        context_->OMSetDepthStencilState(old_depth.get(), old_stencil_ref);
        context_->RSSetState(old_raster.get());
        if (old_viewport_count > 0) context_->RSSetViewports(old_viewport_count, old_viewports.data());
        if (old_scissor_count > 0) context_->RSSetScissorRects(old_scissor_count, old_scissors.data());
        context_->IASetInputLayout(old_layout.get());
        context_->IASetPrimitiveTopology(old_topology);
        ID3D11Buffer* restore_vertex = old_vertex.get();
        context_->IASetVertexBuffers(0, 1, &restore_vertex, &old_stride, &old_offset);
        context_->IASetIndexBuffer(old_index.get(), old_index_format, old_index_offset);
        context_->VSSetShader(old_vs.get(), nullptr, 0);
        ID3D11Buffer* restore_constant = old_constant.get();
        context_->VSSetConstantBuffers(0, 1, &restore_constant);
        context_->PSSetShader(old_ps.get(), nullptr, 0);
        ID3D11ShaderResourceView* restore_resource = old_resource.get();
        context_->PSSetShaderResources(0, 1, &restore_resource);
        ID3D11SamplerState* restore_sampler = old_sampler.get();
        context_->PSSetSamplers(0, 1, &restore_sampler);
        return true;
    }

    void Overlay::add_quad(const Rect& rect, const float u0, const float v0,
        const float u1, const float v1, const Color& color)
    {
        const std::uint32_t packed = pack(color);
        vertices_.insert(vertices_.end(), {
            {rect.left, rect.top, u0, v0, packed}, {rect.right, rect.top, u1, v0, packed},
            {rect.right, rect.bottom, u1, v1, packed}, {rect.left, rect.top, u0, v0, packed},
            {rect.right, rect.bottom, u1, v1, packed}, {rect.left, rect.bottom, u0, v1, packed}
        });
    }

    void Overlay::atlas_icon(const Rect& rect, const int atlas_x, const int atlas_y,
        const int pixel_width, const int pixel_height)
    {
        add_quad(rect, static_cast<float>(atlas_x) / atlas_width, static_cast<float>(atlas_y) / atlas_height,
            static_cast<float>(atlas_x + pixel_width) / atlas_width,
            static_cast<float>(atlas_y + pixel_height) / atlas_height,
            {1.0F, 1.0F, 1.0F, 1.0F});
    }

    void Overlay::fill(const Rect& rect, const Color& color)
    {
        const float u = 0.5F / atlas_width;
        const float v = 0.5F / atlas_height;
        add_quad(rect, u, v, u, v, color);
    }

    void Overlay::stroke(const Rect& rect, const Color& color, const float width)
    {
        fill({rect.left, rect.top, rect.right, rect.top + width}, color);
        fill({rect.left, rect.bottom - width, rect.right, rect.bottom}, color);
        fill({rect.left, rect.top + width, rect.left + width, rect.bottom - width}, color);
        fill({rect.right - width, rect.top + width, rect.right, rect.bottom - width}, color);
    }

    void Overlay::line(const float x1, const float y1, const float x2, const float y2,
        const Color& color, const float width)
    {
        const float dx = x2 - x1;
        const float dy = y2 - y1;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length < 0.001F) return;
        const float nx = -dy / length * width * 0.5F;
        const float ny = dx / length * width * 0.5F;
        const std::uint32_t packed = pack(color);
        const float u = 0.5F / atlas_width;
        const float v = 0.5F / atlas_height;
        vertices_.insert(vertices_.end(), {
            {x1 + nx, y1 + ny, u, v, packed}, {x2 + nx, y2 + ny, u, v, packed},
            {x2 - nx, y2 - ny, u, v, packed}, {x1 + nx, y1 + ny, u, v, packed},
            {x2 - nx, y2 - ny, u, v, packed}, {x1 - nx, y1 - ny, u, v, packed}
        });
    }

    void Overlay::text(const std::wstring& value, const Rect& rect, const Color& color,
        const float size, const TextAlign alignment)
    {
        if (value.empty()) return;
        if (value.find(L'\n') != std::wstring::npos)
        {
            std::vector<std::wstring> lines;
            std::size_t start{};
            while (start <= value.size())
            {
                const std::size_t end = value.find(L'\n', start);
                lines.push_back(value.substr(start, end == std::wstring::npos ? value.size() - start : end - start));
                if (end == std::wstring::npos) break;
                start = end + 1;
            }
            const float line_height = size * 1.25F;
            const float total_height = line_height * static_cast<float>(lines.size());
            float top = rect.top + std::max(0.0F, (rect.bottom - rect.top - total_height) * 0.5F);
            for (const std::wstring& line_value : lines)
            {
                text(line_value, {rect.left, top, rect.right, top + line_height}, color, size, alignment);
                top += line_height;
            }
            return;
        }
        const float scale = size / 22.0F;
        float text_width{};
        for (const wchar_t character : value) text_width += glyph_advances_[glyph_index(character)] * scale;
        float x = rect.left;
        if (alignment == TextAlign::center) x = (rect.left + rect.right - text_width) * 0.5F;
        else if (alignment == TextAlign::right) x = rect.right - text_width;
        const float glyph_height = 27.0F * scale;
        const float y = rect.top + std::max(0.0F, (rect.bottom - rect.top - glyph_height) * 0.5F);
        for (const wchar_t character : value)
        {
            const std::size_t index = glyph_index(character);
            const float advance = glyph_advances_[index] * scale;
            if (x + advance >= rect.left && x <= rect.right)
            {
                const int cell_x = static_cast<int>(index % glyph_columns) * glyph_cell_width;
                const int cell_y = static_cast<int>(index / glyph_columns) * glyph_cell_height;
                const float u0 = static_cast<float>(cell_x + 1) / atlas_width;
                const float v0 = static_cast<float>(cell_y + 1) / atlas_height;
                const float u1 = static_cast<float>(cell_x + glyph_cell_width - 1) / atlas_width;
                const float v1 = static_cast<float>(cell_y + glyph_cell_height - 1) / atlas_height;
                add_quad({x, y, x + glyph_cell_width * scale, y + glyph_cell_height * scale},
                    u0, v0, u1, v1, color);
            }
            x += advance;
            if (x > rect.right) break;
        }
    }

    bool Overlay::button(const std::wstring& label, const Rect& rect, const bool active, InputState& input)
    {
        const int x = input.frame_mouse_x;
        const int y = input.frame_mouse_y;
        const bool hovered = contains(rect, x, y);
        fill(rect, active ? accent_dim : (hovered ? surface_hover : surface));
        if (active) fill({rect.left, rect.top, rect.left + 3.0F, rect.bottom}, accent);
        text(label, {rect.left + 14.0F, rect.top, rect.right - 10.0F, rect.bottom},
            active ? text_primary : text_secondary, 13.0F);
        return consume_click(rect, input);
    }

    bool Overlay::toggle(const std::wstring& label, bool& value, const float x, float& y, InputState& input)
    {
        const Rect row{x, y, x + content_width_, y + 34.0F};
        const bool hovered = contains(row, input.frame_mouse_x, input.frame_mouse_y);
        if (hovered) fill(row, {surface_hover.r, surface_hover.g, surface_hover.b, 0.45F});
        text(label, {x + 2.0F, y, x + content_width_ - 110.0F, y + 34.0F}, text_primary, 13.0F);
        const Rect switch_rect{x + content_width_ - 60.0F, y + 7.0F,
            x + content_width_ - 14.0F, y + 27.0F};
        fill(switch_rect, value ? accent_dim : surface);
        const float knob = value ? switch_rect.right - 16.0F : switch_rect.left + 4.0F;
        fill({knob, switch_rect.top + 4.0F, knob + 12.0F, switch_rect.bottom - 4.0F}, value ? accent : text_secondary);
        bool changed{};
        if (consume_click(row, input))
        {
            value = !value;
            changed = true;
        }
        y += 38.0F;
        feature_binding_context(value, row, x, y, input);
        return changed;
    }

    bool Overlay::checkbox(const std::wstring& label, bool& value, const float x, float& y, InputState& input)
    {
        const Rect row{x, y, x + content_width_, y + 28.0F};
        const bool hovered = contains(row, input.frame_mouse_x, input.frame_mouse_y);
        if (hovered) fill(row, {surface_hover.r, surface_hover.g, surface_hover.b, 0.42F});
        const Rect box{x + 2.0F, y + 5.0F, x + 20.0F, y + 23.0F};
        fill(box, value ? accent_dim : surface);
        stroke(box, value ? accent : text_secondary);
        if (value) fill({box.left + 5.0F, box.top + 5.0F, box.right - 5.0F, box.bottom - 5.0F}, accent);
        text(label, {x + 31.0F, y, x + content_width_ - 10.0F, y + 28.0F}, text_primary, 13.0F);
        const bool changed = consume_click(row, input);
        if (changed) value = !value;
        y += 32.0F;
        feature_binding_context(value, row, x, y, input);
        return changed;
    }

    void Overlay::feature_binding_context(bool& value, const Rect& row, const float x, float& y, InputState& input)
    {
        if (settings_context_ == nullptr) return;
        const FeatureDescriptor* descriptor = feature_descriptor(*settings_context_, value);
        if (descriptor == nullptr) return;

        const bool player_aim = std::wstring_view(descriptor->id) == L"aim.player";
        const bool dino_aim = std::wstring_view(descriptor->id) == L"aim.dino";
        FeatureBinding* binding = settings_context_->find_feature_binding(descriptor->id);
        std::uint32_t key = player_aim ? settings_context_->aim_key :
            dino_aim ? settings_context_->dino_aim_key : binding != nullptr ? binding->key : 0U;
        std::int32_t mode = player_aim ? settings_context_->aim_activation_mode :
            dino_aim ? settings_context_->dino_aim_activation_mode : binding != nullptr ? binding->mode : 0;
        bool show = player_aim ? settings_context_->aim_bind_show :
            dino_aim ? settings_context_->dino_aim_bind_show : binding != nullptr && binding->show_in_list;
        static constexpr std::array<const wchar_t*, 3> mode_names{L"Hold", L"Toggle", L"Always"};
        if (key != 0)
            text(key_name(key) + L" · " + mode_names[static_cast<std::size_t>(std::clamp(mode, 0, 2))],
                {row.left + 220.0F, row.top, row.right - 12.0F, row.bottom},
                checkbox_binding_feature_id_ == descriptor->id ? accent : text_secondary,
                11.0F, TextAlign::right);
        else
            text(L"RMB to bind", {row.left + 220.0F, row.top, row.right - 12.0F, row.bottom},
                checkbox_binding_feature_id_ == descriptor->id ? accent : text_secondary,
                11.0F, TextAlign::right);

        if (consume_right_click(row, input))
        {
            if (checkbox_binding_feature_id_ == descriptor->id)
            {
                checkbox_binding_feature_id_.clear();
            }
            else
            {
                checkbox_binding_feature_id_ = descriptor->id;
                if (!player_aim && !dino_aim && binding == nullptr)
                {
                    settings_context_->feature_bindings.push_back({descriptor->id, 0U, 0, true});
                    binding = &settings_context_->feature_bindings.back();
                }
                open_combo_ = -1;
                active_combo_rect_valid_ = false;
            }
        }
        if (checkbox_binding_feature_id_ != descriptor->id) return;

        binding = settings_context_->find_feature_binding(descriptor->id);
        key = player_aim ? settings_context_->aim_key : dino_aim ? settings_context_->dino_aim_key :
            binding != nullptr ? binding->key : 0U;
        mode = player_aim ? settings_context_->aim_activation_mode : dino_aim ?
            settings_context_->dino_aim_activation_mode : binding != nullptr ? binding->mode : 0;
        show = player_aim ? settings_context_->aim_bind_show : dino_aim ? settings_context_->dino_aim_bind_show :
            binding != nullptr && binding->show_in_list;

        const float card_height = 126.0F;
        const Rect card{x + 14.0F, y, x + content_width_ - 8.0F, y + card_height};
        fill(card, {surface.r, surface.g, surface.b, 0.98F});
        stroke(card, accent_dim);
        text(std::wstring(L"BIND · ") + descriptor->category,
            {card.left + 12.0F, card.top + 5.0F, card.right - 12.0F, card.top + 26.0F}, accent, 10.0F);
        const BindingTarget target = player_aim ? BindingTarget::aim :
            dino_aim ? BindingTarget::dino_aim : BindingTarget::feature;
        const bool waiting = binding_target_ == target &&
            (target != BindingTarget::feature || binding_feature_id_ == descriptor->id);
        const Rect bind_rect{card.left + 12.0F, card.top + 28.0F, card.right - 12.0F, card.top + 60.0F};
        if (button(waiting ? L"Press any key or mouse button..." :
            key == 0 ? L"Add keybind" : key_name(key), bind_rect, waiting, input))
        {
            binding_target_ = target;
            binding_feature_id_ = descriptor->id;
            input.captured_key.store(0, std::memory_order_relaxed);
            input.binding_capture.store(true, std::memory_order_release);
        }

        const int mode_count = player_aim || dino_aim ? 3 : 2;
        const float mode_width = (card.right - card.left - 30.0F) / static_cast<float>(mode_count);
        for (int index = 0; index < mode_count; ++index)
        {
            const float left = card.left + 12.0F + static_cast<float>(index) * (mode_width + 3.0F);
            if (button(mode_names[static_cast<std::size_t>(index)],
                {left, card.top + 66.0F, left + mode_width, card.top + 96.0F}, mode == index, input))
            {
                mode = index;
                if (player_aim) settings_context_->aim_activation_mode = mode;
                else if (dino_aim) settings_context_->dino_aim_activation_mode = mode;
                else if (binding != nullptr) binding->mode = mode;
            }
        }
        const Rect list_button{card.left + 12.0F, card.top + 101.0F, card.left + 172.0F, card.bottom - 5.0F};
        if (button(show ? L"Eye · shown" : L"Eye · hidden", list_button, show, input))
        {
            show = !show;
            if (player_aim) settings_context_->aim_bind_show = show;
            else if (dino_aim) settings_context_->dino_aim_bind_show = show;
            else if (binding != nullptr) binding->show_in_list = show;
        }
        if (!player_aim && !dino_aim && binding != nullptr &&
            button(L"Remove bind", {card.right - 150.0F, card.top + 101.0F,
                card.right - 12.0F, card.bottom - 5.0F}, false, input))
        {
            const std::wstring id = descriptor->id;
            std::erase_if(settings_context_->feature_bindings, [&](const FeatureBinding& item) { return item.id == id; });
            checkbox_binding_feature_id_.clear();
        }
        y += card_height + 8.0F;
    }

    bool Overlay::combo(const std::wstring& label, std::int32_t& value, const wchar_t* const* options,
        const std::size_t count, const int id, const float x, float& y, InputState& input)
    {
        if (options == nullptr || count == 0) return false;
        value = std::clamp(value, 0, static_cast<std::int32_t>(count - 1));
        const float control_left = x + std::min(270.0F, content_width_ * 0.52F);
        text(label, {x + 2.0F, y, control_left - 14.0F, y + 34.0F}, text_primary, 13.0F);
        const Rect control{control_left, y + 1.0F, x + content_width_ - 10.0F, y + 33.0F};
        bool changed{};
        if (button(std::wstring(options[value]) + (open_combo_ == id ? L"  ^" : L"  v"),
            control, open_combo_ == id, input))
            open_combo_ = open_combo_ == id ? -1 : id;
        y += 38.0F;
        if (open_combo_ == id)
        {
            const float popup_height = static_cast<float>(count) * 30.0F + 4.0F;
            float popup_top = control.bottom + 2.0F;
            if (popup_top + popup_height > current_menu_bottom_ - 12.0F)
                popup_top = control.top - popup_height - 2.0F;
            const Rect popup{control.left, popup_top, control.right, popup_top + popup_height};
            active_combo_rect_ = popup;
            active_combo_control_rect_ = control;
            active_combo_rect_valid_ = true;
            for (std::size_t index = 0; index < count; ++index)
            {
                const Rect option{popup.left + 2.0F, popup.top + 2.0F + static_cast<float>(index) * 30.0F,
                    popup.right - 2.0F, popup.top + 30.0F + static_cast<float>(index) * 30.0F};
                if (!input.frame_click_consumed && input.frame_left_pressed &&
                    contains(option, input.frame_mouse_x, input.frame_mouse_y))
                {
                    input.frame_click_consumed = true;
                    value = static_cast<std::int32_t>(index);
                    open_combo_ = -1;
                    active_combo_rect_valid_ = false;
                    changed = true;
                }
            }
            if (open_combo_ == id)
                combo_popup_ = {true, options, count, value, popup};
        }
        else if (open_combo_ == -1) active_combo_rect_valid_ = false;
        y += 6.0F;
        return changed;
    }

    bool Overlay::slider(const std::wstring& label, float& value, const float minimum, const float maximum,
        const float x, float& y, InputState& input, const wchar_t* suffix)
    {
        text(label, {x + 2.0F, y, x + content_width_ - 170.0F, y + 25.0F}, text_primary, 13.0F);
        text(fixed(value, value < 2.0F ? 2 : 0) + suffix,
            {x + content_width_ - 150.0F, y, x + content_width_ - 10.0F, y + 25.0F},
            accent, 12.0F, TextAlign::right);
        const Rect track{x + 2.0F, y + 28.0F, x + content_width_ - 10.0F, y + 34.0F};
        fill(track, surface);
        float ratio = (value - minimum) / (maximum - minimum);
        ratio = std::clamp(ratio, 0.0F, 1.0F);
        fill({track.left, track.top, track.left + (track.right - track.left) * ratio, track.bottom}, accent);
        const Rect hit{track.left, track.top - 7.0F, track.right, track.bottom + 7.0F};
        std::size_t slider_id = std::hash<std::wstring>{}(label);
        slider_id ^= static_cast<std::size_t>(active_tab_ + 1) * 0x9E3779B185EBCA87ULL;
        slider_id ^= static_cast<std::size_t>(active_aim_section_ + active_esp_section_ * 17 + 1) << 32;
        if (slider_id == 0) slider_id = 1;
        bool changed{};
        if (input.frame_left_pressed && active_slider_ == 0 && consume_click(hit, input))
            active_slider_ = slider_id;
        if (input.frame_left_down && active_slider_ == slider_id)
        {
            ratio = std::clamp((static_cast<float>(input.frame_mouse_x) - track.left) /
                (track.right - track.left), 0.0F, 1.0F);
            value = minimum + ratio * (maximum - minimum);
            changed = true;
        }
        y += 54.0F;
        return changed;
    }

    bool Overlay::text_input(const std::wstring& label, std::wstring& value, const int id,
        const float x, float& y, InputState& input, const std::size_t maximum)
    {
        text(label, {x + 2.0F, y, x + content_width_ - 10.0F, y + 24.0F}, text_primary, 12.0F);
        const Rect field{x + 2.0F, y + 25.0F, x + content_width_ - 10.0F, y + 59.0F};
        const bool active = active_text_input_ == id;
        fill(field, active ? surface_hover : surface);
        stroke(field, active ? accent : accent_dim);
        std::wstring shown = value;
        if (active) shown += L"|";
        text(shown.empty() ? L"Type to filter..." : shown,
            {field.left + 9.0F, field.top, field.right - 9.0F, field.bottom},
            shown.empty() ? text_secondary : text_primary, 12.0F);
        if (input.frame_left_pressed)
        {
            if (consume_click(field, input)) active_text_input_ = id;
            else if (active) active_text_input_ = -1;
        }
        bool changed{};
        if (active_text_input_ == id && !input.frame_characters.empty())
        {
            for (const wchar_t character : input.frame_characters)
            {
                if (character == L'\b')
                {
                    if (!value.empty())
                    {
                        value.pop_back();
                        changed = true;
                    }
                }
                else if (character == L'\r' || character == 27)
                {
                    active_text_input_ = -1;
                }
                else if (character >= 32 && value.size() < maximum)
                {
                    value.push_back(character);
                    changed = true;
                }
            }
        }
        y += 64.0F;
        return changed;
    }

    bool Overlay::keybind(const std::wstring& label, std::uint32_t& value, const BindingTarget target,
        const float x, float& y, InputState& input)
    {
        const Rect row{x, y, x + content_width_, y + 40.0F};
        const bool hovered = contains(row, input.frame_mouse_x, input.frame_mouse_y);
        if (hovered) fill(row, {surface_hover.r, surface_hover.g, surface_hover.b, 0.45F});
        text(label, {x + 2.0F, y, x + content_width_ - 225.0F, y + 40.0F}, text_primary, 13.0F);
        const bool waiting = binding_target_ == target;
        const Rect bind_rect{x + content_width_ - 200.0F, y + 4.0F,
            x + content_width_ - 10.0F, y + 36.0F};
        if (button(waiting ? L"Press a key..." : key_name(value), bind_rect, waiting, input))
        {
            binding_target_ = target;
            input.captured_key.store(0, std::memory_order_relaxed);
            input.binding_capture.store(true, std::memory_order_release);
        }
        y += 46.0F;
        return waiting;
    }

    void Overlay::process_binding_capture(Settings& settings, InputState& input)
    {
        if (binding_target_ == BindingTarget::none) return;
        const auto captured = static_cast<std::uint32_t>(input.captured_key.exchange(0, std::memory_order_acq_rel));
        if (captured == 0) return;
        input.binding_capture.store(false, std::memory_order_release);
        if (captured == VK_ESCAPE)
        {
            binding_target_ = BindingTarget::none;
            binding_feature_id_.clear();
            toast_ = L"Rebind cancelled";
            toast_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            return;
        }

        std::uint32_t* target{};
        FeatureBinding* feature_binding{};
        switch (binding_target_)
        {
        case BindingTarget::menu: target = &settings.menu_key; break;
        case BindingTarget::unload: target = &settings.unload_key; break;
        case BindingTarget::aim: target = &settings.aim_key; break;
        case BindingTarget::dino_aim: target = &settings.dino_aim_key; break;
        case BindingTarget::feature:
            feature_binding = settings.find_feature_binding(binding_feature_id_);
            if (feature_binding == nullptr && !binding_feature_id_.empty())
            {
                settings.feature_bindings.push_back({binding_feature_id_, 0U, 0, true});
                feature_binding = &settings.feature_bindings.back();
            }
            if (feature_binding != nullptr) target = &feature_binding->key;
            break;
        case BindingTarget::freecam: target = &settings.freecam_key; break;
        case BindingTarget::esp_toggle: target = &settings.esp_toggle_key; break;
        case BindingTarget::panic: target = &settings.panic_key; break;
        default: break;
        }
        // Feature binds may intentionally share a key (for example a raid preset),
        // but lifecycle controls remain reserved so a feature cannot hide the menu,
        // unload the payload or trigger panic on the same press.
        const bool conflict = target != &settings.menu_key && settings.menu_key == captured ||
            target != &settings.unload_key && settings.unload_key == captured;
        const bool panic_conflict = target != &settings.panic_key && settings.panic_key == captured;
        if (target != nullptr && !conflict && !panic_conflict)
        {
            *target = captured;
            toast_ = L"Binding updated: " + key_name(captured);
        }
        else
        {
            toast_ = L"Binding is already in use";
        }
        binding_target_ = BindingTarget::none;
        binding_feature_id_.clear();
        toast_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    }

    bool Overlay::consume_click(const Rect& rect, InputState& input) const
    {
        if (input.frame_click_consumed || !input.frame_left_pressed ||
            !contains(rect, input.frame_mouse_x, input.frame_mouse_y)) return false;
        if (open_combo_ != -1 && active_combo_rect_valid_ &&
            contains(active_combo_rect_, input.frame_mouse_x, input.frame_mouse_y)) return false;
        input.frame_click_consumed = true;
        return true;
    }

    bool Overlay::consume_right_click(const Rect& rect, InputState& input) const
    {
        if (input.frame_right_click_consumed || !input.frame_right_pressed ||
            !contains(rect, input.frame_mouse_x, input.frame_mouse_y)) return false;
        input.frame_right_click_consumed = true;
        return true;
    }

    bool Overlay::contains(const Rect& rect, const int x, const int y)
    {
        return static_cast<float>(x) >= rect.left && static_cast<float>(x) <= rect.right &&
            static_cast<float>(y) >= rect.top && static_cast<float>(y) <= rect.bottom;
    }

    std::wstring Overlay::key_name(const std::uint32_t key)
    {
        switch (key)
        {
        case VK_LBUTTON: return L"Mouse 1";
        case VK_RBUTTON: return L"Mouse 2";
        case VK_MBUTTON: return L"Mouse 3";
        case VK_XBUTTON1: return L"Mouse 4";
        case VK_XBUTTON2: return L"Mouse 5";
        case VK_LCONTROL: return L"Left Ctrl";
        case VK_RCONTROL: return L"Right Ctrl";
        case VK_LMENU: return L"Left Alt";
        case VK_RMENU: return L"Right Alt";
        case VK_LSHIFT: return L"Left Shift";
        case VK_RSHIFT: return L"Right Shift";
        default: break;
        }
        wchar_t name[64]{};
        UINT scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC);
        if (key == VK_LEFT || key == VK_UP || key == VK_RIGHT || key == VK_DOWN ||
            key == VK_PRIOR || key == VK_NEXT || key == VK_END || key == VK_HOME ||
            key == VK_INSERT || key == VK_DELETE || key == VK_DIVIDE || key == VK_NUMLOCK)
            scan |= 0x100;
        if (GetKeyNameTextW(static_cast<LONG>(scan << 16), name, static_cast<int>(std::size(name))) > 0)
            return name;
        std::wostringstream stream;
        stream << L"VK 0x" << std::uppercase << std::hex << key;
        return stream.str();
    }

    std::uint32_t Overlay::pack(const Color& color)
    {
        const auto channel = [](const float value) {
            return static_cast<std::uint32_t>(std::clamp(value, 0.0F, 1.0F) * 255.0F + 0.5F);
        };
        return channel(color.r) | (channel(color.g) << 8) |
            (channel(color.b) << 16) | (channel(color.a) << 24);
    }

    std::size_t Overlay::glyph_index(const wchar_t character)
    {
        if (character >= 32 && character <= 126) return 1 + static_cast<std::size_t>(character - 32);
        if (character >= 0x400 && character <= 0x4FF) return 96 + static_cast<std::size_t>(character - 0x400);
        return 1 + static_cast<std::size_t>(L'?' - 32);
    }
}
