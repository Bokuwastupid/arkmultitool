#include "kopt/config.hpp"

#include <algorithm>
#include <array>
#include <cwchar>
#include <sstream>
#include <string>

namespace
{
    constexpr std::uint32_t settings_schema_version = 22U;

    bool read_bool(const std::filesystem::path& path, const wchar_t* section,
        const wchar_t* key, const bool fallback)
    {
        return GetPrivateProfileIntW(section, key, fallback ? 1 : 0, path.c_str()) != 0;
    }

    std::uint32_t read_uint(const std::filesystem::path& path, const wchar_t* section,
        const wchar_t* key, const std::uint32_t fallback)
    {
        std::array<wchar_t, 64> fallback_text{};
        std::swprintf(fallback_text.data(), fallback_text.size(), L"%u", fallback);
        std::array<wchar_t, 64> value{};
        GetPrivateProfileStringW(section, key, fallback_text.data(), value.data(),
            static_cast<DWORD>(value.size()), path.c_str());
        wchar_t* end{};
        const auto parsed = std::wcstoul(value.data(), &end, 0);
        return end != value.data() ? static_cast<std::uint32_t>(parsed) : fallback;
    }

    float read_float(const std::filesystem::path& path, const wchar_t* section,
        const wchar_t* key, const float fallback)
    {
        std::array<wchar_t, 64> fallback_text{};
        std::swprintf(fallback_text.data(), fallback_text.size(), L"%.4f", fallback);
        std::array<wchar_t, 64> value{};
        GetPrivateProfileStringW(section, key, fallback_text.data(), value.data(),
            static_cast<DWORD>(value.size()), path.c_str());
        wchar_t* end{};
        const float parsed = std::wcstof(value.data(), &end);
        return end != value.data() ? parsed : fallback;
    }

    std::wstring read_string(const std::filesystem::path& path, const wchar_t* section,
        const wchar_t* key)
    {
        // Feature bindings are serialized into one INI value. Keep enough room for
        // a complete catalog instead of silently truncating larger configurations.
        std::array<wchar_t, 16384> value{};
        GetPrivateProfileStringW(section, key, L"", value.data(), static_cast<DWORD>(value.size()), path.c_str());
        return value.data();
    }

    void write_value(const std::filesystem::path& path, const wchar_t* section,
        const wchar_t* key, const std::wstring& value)
    {
        WritePrivateProfileStringW(section, key, value.c_str(), path.c_str());
    }

    void write_bool(const std::filesystem::path& path, const wchar_t* section,
        const wchar_t* key, const bool value)
    {
        write_value(path, section, key, value ? L"1" : L"0");
    }

    void write_uint(const std::filesystem::path& path, const wchar_t* section,
        const wchar_t* key, const std::uint32_t value)
    {
        wchar_t text[32]{};
        std::swprintf(text, std::size(text), L"0x%X", value);
        write_value(path, section, key, text);
    }

    void write_float(const std::filesystem::path& path, const wchar_t* section,
        const wchar_t* key, const float value)
    {
        wchar_t text[32]{};
        std::swprintf(text, std::size(text), L"%.4f", value);
        write_value(path, section, key, text);
    }

    kopt::Color read_color(const std::filesystem::path& path, const wchar_t* name,
        const kopt::Color& fallback)
    {
        const std::wstring prefix{name};
        return {
            read_float(path, L"Colors", (prefix + L"R").c_str(), fallback.r),
            read_float(path, L"Colors", (prefix + L"G").c_str(), fallback.g),
            read_float(path, L"Colors", (prefix + L"B").c_str(), fallback.b),
            read_float(path, L"Colors", (prefix + L"A").c_str(), fallback.a)};
    }

    void write_color(const std::filesystem::path& path, const wchar_t* name, const kopt::Color& value)
    {
        const std::wstring prefix{name};
        write_float(path, L"Colors", (prefix + L"R").c_str(), value.r);
        write_float(path, L"Colors", (prefix + L"G").c_str(), value.g);
        write_float(path, L"Colors", (prefix + L"B").c_str(), value.b);
        write_float(path, L"Colors", (prefix + L"A").c_str(), value.a);
    }
}

namespace kopt
{
    void Settings::normalize()
    {
        menu_width = std::clamp(menu_width, 760.0F, 1800.0F);
        menu_height = std::clamp(menu_height, 480.0F, 1400.0F);
        ui_scale = std::clamp(ui_scale, 0.75F, 1.50F);
        active_layout = std::clamp(active_layout, 0, 3);
        for (UiLayout& layout : ui_layouts)
        {
            layout.menu_width = std::clamp(layout.menu_width, 760.0F, 1800.0F);
            layout.menu_height = std::clamp(layout.menu_height, 480.0F, 1400.0F);
            layout.menu_x = std::clamp(layout.menu_x, 0.0F, 1.0F);
            layout.menu_y = std::clamp(layout.menu_y, 0.0F, 1.0F);
            layout.ui_scale = std::clamp(layout.ui_scale, 0.75F, 1.50F);
            layout.hotkey_x = std::clamp(layout.hotkey_x, 0.05F, 0.95F);
            layout.hotkey_y = std::clamp(layout.hotkey_y, 0.05F, 0.95F);
            layout.radar_x = std::clamp(layout.radar_x, 0.05F, 0.95F);
            layout.radar_y = std::clamp(layout.radar_y, 0.05F, 0.95F);
        }
        aim_fov = std::clamp(aim_fov, 1.0F, 90.0F);
        aim_distance_m = std::clamp(aim_distance_m, 10.0F, 2500.0F);
        aim_smoothing = std::clamp(aim_smoothing, 1.0F, 40.0F);
        aim_angle_boost = std::clamp(aim_angle_boost, 0.0F, 4.0F);
        aim_activation_mode = std::clamp(aim_activation_mode, 0, 2);
        dino_aim_activation_mode = std::clamp(dino_aim_activation_mode, 0, 2);
        hotkey_list_x = std::clamp(hotkey_list_x, 0.05F, 0.95F);
        hotkey_list_y = std::clamp(hotkey_list_y, 0.05F, 0.95F);
        std::erase_if(feature_bindings, [](const FeatureBinding& binding) {
            return binding.id.empty() || binding.id.size() > 96 || binding.key > 0xFFU;
        });
        std::vector<std::wstring> seen_bindings;
        std::erase_if(feature_bindings, [&](FeatureBinding& binding) {
            binding.mode = std::clamp(binding.mode, 0, 1);
            if (std::find(seen_bindings.begin(), seen_bindings.end(), binding.id) != seen_bindings.end()) return true;
            seen_bindings.push_back(binding.id);
            return false;
        });
        aim_hitbox_mode = std::clamp(aim_hitbox_mode, 0, 1);
        aim_hitbox = std::clamp(aim_hitbox, 0, 4);
        aim_point_method = std::clamp(aim_point_method, 0, 2);
        aim_hitbox_mask &= 0xFFU;
        if (aim_hitbox_mask == 0) aim_hitbox_mask = 0xFFU;
        aim_priority = std::clamp(aim_priority, 0, 3);
        mounted_aim_fov = std::clamp(mounted_aim_fov, 1.0F, 90.0F);
        mounted_aim_smoothing = std::clamp(mounted_aim_smoothing, 1.0F, 40.0F);
        projectile_velocity_mps = std::clamp(projectile_velocity_mps, 10.0F, 2500.0F);
        projectile_gravity_mps2 = std::clamp(projectile_gravity_mps2, 0.0F, 50.0F);
        prediction_latency_ms = std::clamp(prediction_latency_ms, 0.0F, 500.0F);
        random_shot_chance = std::clamp(random_shot_chance, 0.0F, 1.0F);
        esp_distance_m = std::clamp(esp_distance_m, 25.0F, 5000.0F);
        esp_detail_distance_m = std::clamp(esp_detail_distance_m, 10.0F, 5000.0F);
        drop_distance_m = std::clamp(drop_distance_m, 10.0F, 5000.0F);
        refresh_interval_ms = std::clamp(refresh_interval_ms, 16.0F, 1000.0F);
        discovery_interval_ms = std::clamp(discovery_interval_ms, 250.0F, 5000.0F);
        discovery_budget_ms = std::clamp(discovery_budget_ms, 1.0F, 20.0F);
        alert_radius_m = std::clamp(alert_radius_m, 25.0F, 2000.0F);
        alert_noglin_radius_m = std::clamp(alert_noglin_radius_m, 10.0F, 1000.0F);
        alert_approach_speed_mps = std::clamp(alert_approach_speed_mps, 1.0F, 100.0F);
        alert_lifetime_s = std::clamp(alert_lifetime_s, 2.0F, 20.0F);
        alert_cooldown_s = std::clamp(alert_cooldown_s, 2.0F, 120.0F);
        local_chams_style = std::clamp(local_chams_style, 0, 2);
        esp_box_style = std::clamp(esp_box_style, 0, 1);
        player_color_source = std::clamp(player_color_source, 0, 1);
        esp_label_side = std::clamp(esp_label_side, 0, 3);
        esp_health_side = std::clamp(esp_health_side, 0, 3);
        esp_torpor_side = std::clamp(esp_torpor_side, 0, 3);
        esp_status_side = std::clamp(esp_status_side, 0, 3);
        world_box_style = std::clamp(world_box_style, 0, 1);
        world_label_side = std::clamp(world_label_side, 0, 3);
        world_health_side = std::clamp(world_health_side, 0, 3);
        world_torpor_side = std::clamp(world_torpor_side, 0, 3);
        turret_target_filter = std::clamp(turret_target_filter, -1, 5);
        esp_opacity = std::clamp(esp_opacity, 0.15F, 1.0F);
        esp_box_thickness = std::clamp(esp_box_thickness, 0.5F, 4.0F);
        esp_skeleton_thickness = std::clamp(esp_skeleton_thickness, 0.5F, 3.0F);
        esp_label_size = std::clamp(esp_label_size, 10.0F, 22.0F);
        esp_icon_size = std::clamp(esp_icon_size, 18.0F, 48.0F);
        player_visibility_grace_ms = std::clamp(player_visibility_grace_ms, 50.0F, 500.0F);
        radar_size = std::clamp(radar_size, 120.0F, 360.0F);
        radar_range_m = std::clamp(radar_range_m, 50.0F, 1500.0F);
        threat_distance_m = std::clamp(threat_distance_m, 25.0F, 2000.0F);
        radar_x = std::clamp(radar_x, 0.05F, 0.95F);
        radar_y = std::clamp(radar_y, 0.05F, 0.95F);
        structure_group_radius_m = std::clamp(structure_group_radius_m, 2.0F, 50.0F);
        freecam_speed = std::clamp(freecam_speed, 100.0F, 10000.0F);
        freecam_sprint_multiplier = std::clamp(freecam_sprint_multiplier, 1.0F, 10.0F);
        freecam_vertical_multiplier = std::clamp(freecam_vertical_multiplier, 0.1F, 5.0F);
        freecam_smoothing = std::clamp(freecam_smoothing, 0.0F, 0.5F);
        freecam_mouse_sensitivity = std::clamp(freecam_mouse_sensitivity, 0.01F, 0.5F);
        camera_fov = std::clamp(camera_fov, 50.0F, 170.0F);
        chams_distance_m = std::clamp(chams_distance_m, 25.0F, 1500.0F);
        chams_budget = std::clamp(chams_budget, 8.0F, 512.0F);
        share_interval_ms = std::clamp(share_interval_ms, 250.0F, 10000.0F);
        const auto normalize_color = [](Color& color) {
            color.r = std::clamp(color.r, 0.0F, 1.0F);
            color.g = std::clamp(color.g, 0.0F, 1.0F);
            color.b = std::clamp(color.b, 0.0F, 1.0F);
            color.a = std::clamp(color.a, 0.10F, 1.0F);
        };
        normalize_color(own_color);
        normalize_color(ally_color);
        normalize_color(enemy_color);
        normalize_color(player_awake_color);
        normalize_color(player_sleeping_color);
        normalize_color(player_knocked_out_color);
        normalize_color(player_dead_color);
        normalize_color(player_occluded_color);
        normalize_color(wild_color);
        normalize_color(structure_color);
        normalize_color(health_color);
        normalize_color(torpor_color);
        normalize_color(menu_accent_color);
        normalize_color(local_chams_color);
        // Exact ShooterGame.pdb profile: AController::ControlRotation is this+0x490.
        // Do not retain the old 0x4A0 value: it overwrites adjacent controller input state.
        control_rotation_offset = 0x490U;
        std::erase(allied_teams, 0);
        std::sort(allied_teams.begin(), allied_teams.end());
        allied_teams.erase(std::unique(allied_teams.begin(), allied_teams.end()), allied_teams.end());
    }

    bool Settings::load(const std::filesystem::path& path)
    {
        if (!std::filesystem::exists(path))
        {
            normalize();
            return save(path);
        }

        const auto loaded_schema_version = read_uint(path, L"Meta", L"SchemaVersion", 0U);
        menu_width = read_float(path, L"Menu", L"Width", menu_width);
        menu_height = read_float(path, L"Menu", L"Height", menu_height);
        ui_scale = read_float(path, L"Menu", L"Scale", ui_scale);
        active_layout = static_cast<std::int32_t>(read_uint(path, L"Menu", L"ActiveLayout", active_layout));
        favorite_features = read_string(path, L"Menu", L"Favorites");
        for (std::size_t index = 0; index < ui_layouts.size(); ++index)
        {
            const std::wstring section = L"Layout" + std::to_wstring(index + 1);
            UiLayout& layout = ui_layouts[index];
            layout.menu_width = read_float(path, section.c_str(), L"MenuWidth", layout.menu_width);
            layout.menu_height = read_float(path, section.c_str(), L"MenuHeight", layout.menu_height);
            layout.menu_x = read_float(path, section.c_str(), L"MenuX", layout.menu_x);
            layout.menu_y = read_float(path, section.c_str(), L"MenuY", layout.menu_y);
            layout.ui_scale = read_float(path, section.c_str(), L"Scale", layout.ui_scale);
            layout.hotkey_x = read_float(path, section.c_str(), L"HotkeyX", layout.hotkey_x);
            layout.hotkey_y = read_float(path, section.c_str(), L"HotkeyY", layout.hotkey_y);
            layout.radar_x = read_float(path, section.c_str(), L"RadarX", layout.radar_x);
            layout.radar_y = read_float(path, section.c_str(), L"RadarY", layout.radar_y);
        }
        player_aim = read_bool(path, L"Aim", L"PlayerAim", player_aim);
        dino_aim = read_bool(path, L"Aim", L"DinoAim", dino_aim);
        aim_target_enemies = read_bool(path, L"Aim", L"TargetEnemies", aim_target_enemies);
        aim_target_allies = read_bool(path, L"Aim", L"TargetAllies", aim_target_allies);
        visibility_check = read_bool(path, L"Aim", L"VisibilityCheck", visibility_check);
        random_hitbox = read_bool(path, L"Aim", L"RandomHitbox", random_hitbox);
        aim_lock = read_bool(path, L"Aim", L"LockTarget", aim_lock);
        aim_draw_fov = read_bool(path, L"Aim", L"DrawFov", aim_draw_fov);
        aim_activation_mode = static_cast<std::int32_t>(read_uint(path, L"Aim", L"ActivationMode", aim_activation_mode));
        dino_aim_activation_mode = static_cast<std::int32_t>(
            read_uint(path, L"Aim", L"DinoActivationMode", dino_aim_activation_mode));
        aim_hitbox_mode = static_cast<std::int32_t>(read_uint(path, L"Aim", L"HitboxMode", aim_hitbox_mode));
        aim_hitbox = static_cast<std::int32_t>(read_uint(path, L"Aim", L"Hitbox", aim_hitbox));
        aim_point_method = static_cast<std::int32_t>(read_uint(path, L"Aim", L"PointMethod", aim_point_method));
        aim_hitbox_mask = read_uint(path, L"Aim", L"HitboxMask", aim_hitbox_mask);
        aim_priority = static_cast<std::int32_t>(read_uint(path, L"Aim", L"Priority", aim_priority));
        aim_fov = read_float(path, L"Aim", L"Fov", aim_fov);
        aim_distance_m = read_float(path, L"Aim", L"DistanceM", aim_distance_m);
        aim_smoothing = read_float(path, L"Aim", L"Smoothing", aim_smoothing);
        aim_angle_boost = read_float(path, L"Aim", L"AngleBoost", aim_angle_boost);
        mounted_aim_fov = read_float(path, L"Aim", L"MountedFov", mounted_aim_fov);
        mounted_aim_smoothing = read_float(path, L"Aim", L"MountedSmoothing", mounted_aim_smoothing);
        aim_prediction = read_bool(path, L"Aim", L"Prediction", aim_prediction);
        aim_intercept_solver = read_bool(path, L"Aim", L"InterceptSolver", aim_intercept_solver);
        projectile_velocity_mps = read_float(path, L"Aim", L"ProjectileVelocityMps", projectile_velocity_mps);
        projectile_gravity_mps2 = read_float(path, L"Aim", L"ProjectileGravityMps2", projectile_gravity_mps2);
        prediction_latency_ms = read_float(path, L"Aim", L"PredictionLatencyMs", prediction_latency_ms);
        random_shot_chance = read_float(path, L"Aim", L"RandomChance", random_shot_chance);
        aim_key = read_uint(path, L"Aim", L"Key", aim_key);
        dino_aim_key = read_uint(path, L"Aim", L"DinoKey", dino_aim_key);
        aim_bind_show = read_bool(path, L"Aim", L"ShowPlayerBind", aim_bind_show);
        dino_aim_bind_show = read_bool(path, L"Aim", L"ShowDinoBind", dino_aim_bind_show);

        esp_enabled = read_bool(path, L"ESP", L"Enabled", esp_enabled);
        player_esp = read_bool(path, L"ESP", L"Players", player_esp);
        enemy_dino_esp = read_bool(path, L"ESP", L"EnemyDinos", enemy_dino_esp);
        wild_dino_esp = read_bool(path, L"ESP", L"WildDinos", wild_dino_esp);
        structure_esp = read_bool(path, L"ESP", L"Structures", structure_esp);
        turret_esp = read_bool(path, L"ESP", L"Turrets", turret_esp);
        drop_esp = read_bool(path, L"ESP", L"Drops", drop_esp);
        death_cache_esp = read_bool(path, L"ESP", L"DeathCaches", death_cache_esp);
        player_item_cache_esp = read_bool(path, L"ESP", L"PlayerItemCaches", player_item_cache_esp);
        dino_item_cache_esp = read_bool(path, L"ESP", L"DinoItemCaches", dino_item_cache_esp);
        show_names = read_bool(path, L"ESP", L"Names", show_names);
        show_tribes = read_bool(path, L"ESP", L"Tribes", show_tribes);
        show_distance = read_bool(path, L"ESP", L"Distance", show_distance);
        show_health = read_bool(path, L"ESP", L"Health", show_health);
        show_torpor = read_bool(path, L"ESP", L"Torpor", show_torpor);
        show_vital_values = read_bool(path, L"ESP", L"VitalValues", show_vital_values);
        show_boxes = read_bool(path, L"ESP", L"Boxes", show_boxes);
        show_skeleton = read_bool(path, L"ESP", L"Skeleton", show_skeleton);
        show_dino_skeleton = read_bool(path, L"ESP", L"DinoSkeleton", show_dino_skeleton);
        show_held_items = read_bool(path, L"ESP", L"HeldItems", show_held_items);
        show_equipment = read_bool(path, L"ESP", L"Equipment", show_equipment);
        show_turret_details = read_bool(path, L"ESP", L"TurretDetails", show_turret_details);
        show_drop_quantity = read_bool(path, L"ESP", L"DropQuantity", show_drop_quantity);
        show_radar = read_bool(path, L"ESP", L"Radar", show_radar);
        show_threat_panel = read_bool(path, L"ESP", L"ThreatPanel", show_threat_panel);
        structure_grouping = read_bool(path, L"ESP", L"StructureGrouping", structure_grouping);
        structure_whitelist_enabled = read_bool(path, L"ESP", L"StructureWhitelistEnabled",
            structure_whitelist_enabled);
        show_tracers = read_bool(path, L"ESP", L"Tracers", show_tracers);
        offscreen_arrows = read_bool(path, L"ESP", L"OffscreenArrows", offscreen_arrows);
        esp_show_enemies = read_bool(path, L"ESP", L"ShowEnemies", esp_show_enemies);
        esp_show_allies = read_bool(path, L"ESP", L"ShowAllies", esp_show_allies);
        esp_own_players = read_bool(path, L"ESP", L"OwnPlayers", esp_own_players);
        esp_allied_players = read_bool(path, L"ESP", L"AlliedPlayers", esp_allied_players);
        esp_enemy_players = read_bool(path, L"ESP", L"EnemyPlayers", esp_enemy_players);
        esp_own_dinos = read_bool(path, L"ESP", L"OwnDinos", esp_own_dinos);
        esp_allied_dinos = read_bool(path, L"ESP", L"AlliedDinos", esp_allied_dinos);
        esp_enemy_dinos = read_bool(path, L"ESP", L"EnemyTamedDinos", esp_enemy_dinos);
        esp_own_structures = read_bool(path, L"ESP", L"OwnStructures", esp_own_structures);
        esp_allied_structures = read_bool(path, L"ESP", L"AlliedStructures", esp_allied_structures);
        esp_enemy_structures = read_bool(path, L"ESP", L"EnemyStructures", esp_enemy_structures);
        esp_neutral_structures = read_bool(path, L"ESP", L"NeutralStructures", esp_neutral_structures);
        show_awake_players = read_bool(path, L"ESP", L"AwakePlayers", show_awake_players);
        show_sleeping_players = read_bool(path, L"ESP", L"SleepingPlayers", show_sleeping_players);
        show_knocked_out_players = read_bool(path, L"ESP", L"KnockedOutPlayers", show_knocked_out_players);
        show_dead_players = read_bool(path, L"ESP", L"DeadPlayers", show_dead_players);
        show_player_status = read_bool(path, L"ESP", L"PlayerStatus", show_player_status);
        player_occluded_color_enabled = read_bool(path, L"ESP", L"PlayerOccludedColorEnabled",
            player_occluded_color_enabled);
        show_player_labels = read_bool(path, L"ESP", L"PlayerLabels", show_player_labels);
        show_dino_labels = read_bool(path, L"ESP", L"DinoLabels", show_dino_labels);
        show_structure_labels = read_bool(path, L"ESP", L"StructureLabels", show_structure_labels);
        show_player_health = read_bool(path, L"ESP", L"PlayerHealth", show_player_health);
        show_dino_health = read_bool(path, L"ESP", L"DinoHealth", show_dino_health);
        show_structure_health = read_bool(path, L"ESP", L"StructureHealth", show_structure_health);
        show_player_torpor = read_bool(path, L"ESP", L"PlayerTorpor", show_player_torpor);
        show_dino_torpor = read_bool(path, L"ESP", L"DinoTorpor", show_dino_torpor);
        compact_labels = read_bool(path, L"ESP", L"CompactLabels", compact_labels);
        turret_show_ammo = read_bool(path, L"ESP", L"TurretAmmo", turret_show_ammo);
        turret_show_state = read_bool(path, L"ESP", L"TurretState", turret_show_state);
        turret_show_power = read_bool(path, L"ESP", L"TurretPower", turret_show_power);
        turret_show_range = read_bool(path, L"ESP", L"TurretRange", turret_show_range);
        turret_show_target_mode = read_bool(path, L"ESP", L"TurretTargetMode", turret_show_target_mode);
        turret_show_target_state = read_bool(path, L"ESP", L"TurretTargetState", turret_show_target_state);
        turret_show_warning = read_bool(path, L"ESP", L"TurretWarning", turret_show_warning);
        turret_hide_nonmatching = read_bool(path, L"ESP", L"TurretHideNonmatching", turret_hide_nonmatching);
        turret_target_filter = static_cast<std::int32_t>(read_uint(path, L"ESP", L"TurretTargetFilter",
            static_cast<std::uint32_t>(turret_target_filter)));
        battle_mode = read_bool(path, L"ESP", L"BattleMode", battle_mode);
        smart_declutter = read_bool(path, L"ESP", L"SmartDeclutter", smart_declutter);
        show_structure_summary = read_bool(path, L"ESP", L"StructureSummary", show_structure_summary);
        show_player_summary = read_bool(path, L"ESP", L"PlayerSummary", show_player_summary);
        show_dino_summary = read_bool(path, L"ESP", L"DinoSummary", show_dino_summary);
        summary_uses_filters = read_bool(path, L"ESP", L"SummaryUsesFilters", summary_uses_filters);
        esp_box_style = static_cast<std::int32_t>(read_uint(path, L"ESP", L"BoxStyle", esp_box_style));
        player_color_source = static_cast<std::int32_t>(read_uint(path, L"ESP", L"PlayerColorSource", player_color_source));
        esp_label_side = static_cast<std::int32_t>(read_uint(path, L"ESP", L"LabelSide", esp_label_side));
        esp_health_side = static_cast<std::int32_t>(read_uint(path, L"ESP", L"HealthSide", esp_health_side));
        esp_torpor_side = static_cast<std::int32_t>(read_uint(path, L"ESP", L"TorporSide", esp_torpor_side));
        esp_status_side = static_cast<std::int32_t>(read_uint(path, L"ESP", L"StatusSide", esp_status_side));
        world_box_style = static_cast<std::int32_t>(read_uint(path, L"ESP", L"WorldBoxStyle", world_box_style));
        world_label_side = static_cast<std::int32_t>(read_uint(path, L"ESP", L"WorldLabelSide", world_label_side));
        world_health_side = static_cast<std::int32_t>(read_uint(path, L"ESP", L"WorldHealthSide", world_health_side));
        world_torpor_side = static_cast<std::int32_t>(read_uint(path, L"ESP", L"WorldTorporSide", world_torpor_side));
        esp_opacity = read_float(path, L"ESP", L"Opacity", esp_opacity);
        esp_box_thickness = read_float(path, L"ESP", L"BoxThickness", esp_box_thickness);
        esp_skeleton_thickness = read_float(path, L"ESP", L"SkeletonThickness", esp_skeleton_thickness);
        esp_label_size = read_float(path, L"ESP", L"LabelSize", esp_label_size);
        esp_icon_size = read_float(path, L"ESP", L"IconSize", esp_icon_size);
        player_visibility_grace_ms = read_float(path, L"ESP", L"PlayerVisibilityGraceMs",
            player_visibility_grace_ms);
        radar_size = read_float(path, L"ESP", L"RadarSize", radar_size);
        radar_range_m = read_float(path, L"ESP", L"RadarRangeM", radar_range_m);
        threat_distance_m = read_float(path, L"ESP", L"ThreatDistanceM", threat_distance_m);
        radar_x = read_float(path, L"ESP", L"RadarX", radar_x);
        radar_y = read_float(path, L"ESP", L"RadarY", radar_y);
        structure_group_radius_m = read_float(path, L"ESP", L"StructureGroupRadiusM", structure_group_radius_m);
        esp_distance_m = read_float(path, L"ESP", L"DistanceM", esp_distance_m);
        esp_detail_distance_m = read_float(path, L"ESP", L"DetailDistanceM", esp_detail_distance_m);
        drop_distance_m = read_float(path, L"ESP", L"DropDistanceM", drop_distance_m);
        refresh_interval_ms = read_float(path, L"ESP", L"RefreshMs", refresh_interval_ms);
        discovery_interval_ms = read_float(path, L"ESP", L"DiscoveryMs", discovery_interval_ms);
        discovery_budget_ms = read_float(path, L"ESP", L"DiscoveryBudgetMs", discovery_budget_ms);
        esp_search = read_string(path, L"ESP", L"Search");
        hidden_tribes = read_string(path, L"ESP", L"HiddenTribes");
        hidden_dino_types = read_string(path, L"ESP", L"HiddenDinoTypes");
        hidden_structure_types = read_string(path, L"ESP", L"HiddenStructureTypes");
        grouped_structure_types = read_string(path, L"ESP", L"GroupedStructureTypes");
        selected_structure_types = read_string(path, L"ESP", L"SelectedStructureTypes");
        known_structure_types = read_string(path, L"ESP", L"KnownStructureTypes");
        alerts_enabled = read_bool(path, L"Alerts", L"Enabled", alerts_enabled);
        alert_new_player = read_bool(path, L"Alerts", L"NewPlayer", alert_new_player);
        alert_approach = read_bool(path, L"Alerts", L"Approach", alert_approach);
        alert_sleep = read_bool(path, L"Alerts", L"Sleep", alert_sleep);
        alert_death = read_bool(path, L"Alerts", L"Death", alert_death);
        alert_noglin = read_bool(path, L"Alerts", L"Noglin", alert_noglin);
        alert_turret = read_bool(path, L"Alerts", L"Turret", alert_turret);
        alert_enemy_group = read_bool(path, L"Alerts", L"EnemyGroup", alert_enemy_group);
        alert_sound = read_bool(path, L"Alerts", L"Sound", alert_sound);
        alert_radius_m = read_float(path, L"Alerts", L"RadiusM", alert_radius_m);
        alert_noglin_radius_m = read_float(path, L"Alerts", L"NoglinRadiusM", alert_noglin_radius_m);
        alert_approach_speed_mps = read_float(path, L"Alerts", L"ApproachSpeedMps", alert_approach_speed_mps);
        alert_lifetime_s = read_float(path, L"Alerts", L"LifetimeS", alert_lifetime_s);
        alert_cooldown_s = read_float(path, L"Alerts", L"CooldownS", alert_cooldown_s);

        freecam = read_bool(path, L"Camera", L"Freecam", freecam);
        freecam_speed = read_float(path, L"Camera", L"FreecamSpeed", freecam_speed);
        freecam_sprint_multiplier = read_float(path, L"Camera", L"SprintMultiplier", freecam_sprint_multiplier);
        freecam_vertical_multiplier = read_float(path, L"Camera", L"VerticalMultiplier", freecam_vertical_multiplier);
        freecam_smoothing = read_float(path, L"Camera", L"Smoothing", freecam_smoothing);
        freecam_mouse_sensitivity = read_float(path, L"Camera", L"MouseSensitivity", freecam_mouse_sensitivity);
        fov_override = read_bool(path, L"Camera", L"FovOverride", fov_override);
        camera_fov = read_float(path, L"Camera", L"Fov", camera_fov);
        no_recoil = read_bool(path, L"Weapon", L"NoRecoil", no_recoil);
        no_sway = read_bool(path, L"Weapon", L"NoSway", no_sway);
        local_chams = read_bool(path, L"Chams", L"Local", local_chams);
        local_chams_style = static_cast<std::int32_t>(read_uint(path, L"Chams", L"LocalStyle", local_chams_style));
        enemy_chams = read_bool(path, L"Chams", L"Enemies", enemy_chams);
        chams_players = read_bool(path, L"Chams", L"Players", chams_players);
        chams_dinos = read_bool(path, L"Chams", L"Dinos", chams_dinos);
        chams_distance_m = read_float(path, L"Chams", L"DistanceM", chams_distance_m);
        chams_budget = read_float(path, L"Chams", L"Budget", chams_budget);
        own_color = read_color(path, L"Own", own_color);
        ally_color = read_color(path, L"Ally", ally_color);
        enemy_color = read_color(path, L"Enemy", enemy_color);
        player_awake_color = read_color(path, L"PlayerAwake", player_awake_color);
        player_sleeping_color = read_color(path, L"PlayerSleeping", player_sleeping_color);
        player_knocked_out_color = read_color(path, L"PlayerKnockedOut", player_knocked_out_color);
        player_dead_color = read_color(path, L"PlayerDead", player_dead_color);
        player_occluded_color = read_color(path, L"PlayerOccluded", player_occluded_color);
        wild_color = read_color(path, L"Wild", wild_color);
        structure_color = read_color(path, L"Structure", structure_color);
        health_color = read_color(path, L"Health", health_color);
        torpor_color = read_color(path, L"Torpor", torpor_color);
        menu_accent_color = read_color(path, L"MenuAccent", menu_accent_color);
        local_chams_color = read_color(path, L"LocalChams", local_chams_color);

        debug_panel = read_bool(path, L"Runtime", L"DebugPanel", debug_panel);
        aim_lab_recording = read_bool(path, L"Runtime", L"AimLabRecording", aim_lab_recording);
        show_hotkey_list = read_bool(path, L"Hotkeys", L"ShowList", show_hotkey_list);
        hotkey_list_x = read_float(path, L"Hotkeys", L"PositionX", hotkey_list_x);
        hotkey_list_y = read_float(path, L"Hotkeys", L"PositionY", hotkey_list_y);
        feature_bindings.clear();
        std::wstringstream serialized_bindings(read_string(path, L"Hotkeys", L"FeatureBindings"));
        std::wstring serialized_binding;
        while (std::getline(serialized_bindings, serialized_binding, L';'))
        {
            if (serialized_binding.empty()) continue;
            std::wstringstream fields(serialized_binding);
            std::wstring id, key_text, mode_text, show_text;
            if (!std::getline(fields, id, L',') || !std::getline(fields, key_text, L',') ||
                !std::getline(fields, mode_text, L',') || !std::getline(fields, show_text, L',')) continue;
            wchar_t* key_end{};
            const auto key = std::wcstoul(key_text.c_str(), &key_end, 0);
            feature_bindings.push_back({id, static_cast<std::uint32_t>(key),
                static_cast<std::int32_t>(std::wcstol(mode_text.c_str(), nullptr, 10)), show_text != L"0"});
        }
        control_rotation_offset = read_uint(path, L"Runtime", L"ControlRotationOffset", control_rotation_offset);
        menu_key = read_uint(path, L"Bindings", L"Menu", menu_key);
        unload_key = read_uint(path, L"Bindings", L"Unload", unload_key);
        freecam_key = read_uint(path, L"Bindings", L"Freecam", freecam_key);
        esp_toggle_key = read_uint(path, L"Bindings", L"EspToggle", esp_toggle_key);
        panic_key = read_uint(path, L"Bindings", L"Panic", panic_key);
        share_enabled = read_bool(path, L"Share", L"Enabled", share_enabled);
        share_endpoint = read_string(path, L"Share", L"Endpoint");
        if (share_endpoint.empty()) share_endpoint = L"https://127.0.0.1:8443/v1/share";
        share_interval_ms = read_float(path, L"Share", L"IntervalMs", share_interval_ms);
        allied_teams.clear();
        std::wstringstream teams(read_string(path, L"Relations", L"AlliedTeams"));
        std::wstring team_text;
        while (std::getline(teams, team_text, L','))
        {
            wchar_t* end{};
            const long value = std::wcstol(team_text.c_str(), &end, 10);
            if (end != team_text.c_str() && value > 0 && value <= INT32_MAX)
                allied_teams.push_back(static_cast<std::int32_t>(value));
        }
        // Version 9 makes visibility a hard aim eligibility rule. Older profiles
        // contained the unused flag as false, so migrate them to the safe behavior.
        if (loaded_schema_version < 9U) visibility_check = true;
        // Version 13 replaces the legacy Head/Neck/Chest/Pelvis/Random list with
        // Minimal categories plus an Advanced per-limb selection mask.
        if (loaded_schema_version < 13U)
        {
            // Legacy zones were Head/Neck/Chest/Pelvis/Random. Preserve the
            // practical intent while moving to the five-category limb model.
            if (aim_hitbox <= 1) aim_hitbox = 0;
            else if (aim_hitbox == 2) aim_hitbox = 1;
            else aim_hitbox = 3;
            aim_hitbox_mode = 0;
            aim_point_method = 2;
            aim_hitbox_mask = 0xFFU;
            random_hitbox = false;
        }
        if (loaded_schema_version < 21U)
        {
            UiLayout& layout = ui_layouts[static_cast<std::size_t>(std::clamp(active_layout, 0, 3))];
            layout.menu_width = menu_width;
            layout.menu_height = menu_height;
            layout.ui_scale = ui_scale;
            layout.hotkey_x = hotkey_list_x;
            layout.hotkey_y = hotkey_list_y;
            layout.radar_x = radar_x;
            layout.radar_y = radar_y;
        }
        normalize();
        if (loaded_schema_version < settings_schema_version)
        {
            auto backup = path;
            backup += L".legacy.bak";
            std::error_code backup_error;
            if (!std::filesystem::exists(backup))
                std::filesystem::copy_file(path, backup, std::filesystem::copy_options::none, backup_error);
            return save(path);
        }
        return true;
    }

    bool Settings::save(const std::filesystem::path& path) const
    {
        write_uint(path, L"Meta", L"SchemaVersion", settings_schema_version);
        write_float(path, L"Menu", L"Width", menu_width);
        write_float(path, L"Menu", L"Height", menu_height);
        write_float(path, L"Menu", L"Scale", ui_scale);
        write_uint(path, L"Menu", L"ActiveLayout", static_cast<std::uint32_t>(active_layout));
        write_value(path, L"Menu", L"Favorites", favorite_features);
        for (std::size_t index = 0; index < ui_layouts.size(); ++index)
        {
            const std::wstring section = L"Layout" + std::to_wstring(index + 1);
            const UiLayout& layout = ui_layouts[index];
            write_float(path, section.c_str(), L"MenuWidth", layout.menu_width);
            write_float(path, section.c_str(), L"MenuHeight", layout.menu_height);
            write_float(path, section.c_str(), L"MenuX", layout.menu_x);
            write_float(path, section.c_str(), L"MenuY", layout.menu_y);
            write_float(path, section.c_str(), L"Scale", layout.ui_scale);
            write_float(path, section.c_str(), L"HotkeyX", layout.hotkey_x);
            write_float(path, section.c_str(), L"HotkeyY", layout.hotkey_y);
            write_float(path, section.c_str(), L"RadarX", layout.radar_x);
            write_float(path, section.c_str(), L"RadarY", layout.radar_y);
        }
        write_bool(path, L"Aim", L"PlayerAim", player_aim);
        write_bool(path, L"Aim", L"DinoAim", dino_aim);
        write_bool(path, L"Aim", L"TargetEnemies", aim_target_enemies);
        write_bool(path, L"Aim", L"TargetAllies", aim_target_allies);
        write_bool(path, L"Aim", L"VisibilityCheck", visibility_check);
        write_bool(path, L"Aim", L"RandomHitbox", random_hitbox);
        write_bool(path, L"Aim", L"LockTarget", aim_lock);
        write_bool(path, L"Aim", L"DrawFov", aim_draw_fov);
        write_uint(path, L"Aim", L"ActivationMode", static_cast<std::uint32_t>(aim_activation_mode));
        write_uint(path, L"Aim", L"DinoActivationMode", static_cast<std::uint32_t>(dino_aim_activation_mode));
        write_uint(path, L"Aim", L"HitboxMode", static_cast<std::uint32_t>(aim_hitbox_mode));
        write_uint(path, L"Aim", L"Hitbox", static_cast<std::uint32_t>(aim_hitbox));
        write_uint(path, L"Aim", L"PointMethod", static_cast<std::uint32_t>(aim_point_method));
        write_uint(path, L"Aim", L"HitboxMask", aim_hitbox_mask);
        write_uint(path, L"Aim", L"Priority", static_cast<std::uint32_t>(aim_priority));
        write_float(path, L"Aim", L"Fov", aim_fov);
        write_float(path, L"Aim", L"DistanceM", aim_distance_m);
        write_float(path, L"Aim", L"Smoothing", aim_smoothing);
        write_float(path, L"Aim", L"AngleBoost", aim_angle_boost);
        write_float(path, L"Aim", L"MountedFov", mounted_aim_fov);
        write_float(path, L"Aim", L"MountedSmoothing", mounted_aim_smoothing);
        write_bool(path, L"Aim", L"Prediction", aim_prediction);
        write_bool(path, L"Aim", L"InterceptSolver", aim_intercept_solver);
        write_float(path, L"Aim", L"ProjectileVelocityMps", projectile_velocity_mps);
        write_float(path, L"Aim", L"ProjectileGravityMps2", projectile_gravity_mps2);
        write_float(path, L"Aim", L"PredictionLatencyMs", prediction_latency_ms);
        write_float(path, L"Aim", L"RandomChance", random_shot_chance);
        write_uint(path, L"Aim", L"Key", aim_key);
        write_uint(path, L"Aim", L"DinoKey", dino_aim_key);
        write_bool(path, L"Aim", L"ShowPlayerBind", aim_bind_show);
        write_bool(path, L"Aim", L"ShowDinoBind", dino_aim_bind_show);

        write_bool(path, L"ESP", L"Enabled", esp_enabled);
        write_bool(path, L"ESP", L"Players", player_esp);
        write_bool(path, L"ESP", L"EnemyDinos", enemy_dino_esp);
        write_bool(path, L"ESP", L"WildDinos", wild_dino_esp);
        write_bool(path, L"ESP", L"Structures", structure_esp);
        write_bool(path, L"ESP", L"Turrets", turret_esp);
        write_bool(path, L"ESP", L"Drops", drop_esp);
        write_bool(path, L"ESP", L"DeathCaches", death_cache_esp);
        write_bool(path, L"ESP", L"PlayerItemCaches", player_item_cache_esp);
        write_bool(path, L"ESP", L"DinoItemCaches", dino_item_cache_esp);
        write_bool(path, L"ESP", L"Names", show_names);
        write_bool(path, L"ESP", L"Tribes", show_tribes);
        write_bool(path, L"ESP", L"Distance", show_distance);
        write_bool(path, L"ESP", L"Health", show_health);
        write_bool(path, L"ESP", L"Torpor", show_torpor);
        write_bool(path, L"ESP", L"VitalValues", show_vital_values);
        write_bool(path, L"ESP", L"Boxes", show_boxes);
        write_bool(path, L"ESP", L"Skeleton", show_skeleton);
        write_bool(path, L"ESP", L"DinoSkeleton", show_dino_skeleton);
        write_bool(path, L"ESP", L"HeldItems", show_held_items);
        write_bool(path, L"ESP", L"Equipment", show_equipment);
        write_bool(path, L"ESP", L"TurretDetails", show_turret_details);
        write_bool(path, L"ESP", L"DropQuantity", show_drop_quantity);
        write_bool(path, L"ESP", L"Radar", show_radar);
        write_bool(path, L"ESP", L"ThreatPanel", show_threat_panel);
        write_bool(path, L"ESP", L"StructureGrouping", structure_grouping);
        write_bool(path, L"ESP", L"StructureWhitelistEnabled", structure_whitelist_enabled);
        write_bool(path, L"ESP", L"Tracers", show_tracers);
        write_bool(path, L"ESP", L"OffscreenArrows", offscreen_arrows);
        write_bool(path, L"ESP", L"ShowEnemies", esp_show_enemies);
        write_bool(path, L"ESP", L"ShowAllies", esp_show_allies);
        write_bool(path, L"ESP", L"OwnPlayers", esp_own_players);
        write_bool(path, L"ESP", L"AlliedPlayers", esp_allied_players);
        write_bool(path, L"ESP", L"EnemyPlayers", esp_enemy_players);
        write_bool(path, L"ESP", L"OwnDinos", esp_own_dinos);
        write_bool(path, L"ESP", L"AlliedDinos", esp_allied_dinos);
        write_bool(path, L"ESP", L"EnemyTamedDinos", esp_enemy_dinos);
        write_bool(path, L"ESP", L"OwnStructures", esp_own_structures);
        write_bool(path, L"ESP", L"AlliedStructures", esp_allied_structures);
        write_bool(path, L"ESP", L"EnemyStructures", esp_enemy_structures);
        write_bool(path, L"ESP", L"NeutralStructures", esp_neutral_structures);
        write_bool(path, L"ESP", L"AwakePlayers", show_awake_players);
        write_bool(path, L"ESP", L"SleepingPlayers", show_sleeping_players);
        write_bool(path, L"ESP", L"KnockedOutPlayers", show_knocked_out_players);
        write_bool(path, L"ESP", L"DeadPlayers", show_dead_players);
        write_bool(path, L"ESP", L"PlayerStatus", show_player_status);
        write_bool(path, L"ESP", L"PlayerOccludedColorEnabled", player_occluded_color_enabled);
        write_bool(path, L"ESP", L"PlayerLabels", show_player_labels);
        write_bool(path, L"ESP", L"DinoLabels", show_dino_labels);
        write_bool(path, L"ESP", L"StructureLabels", show_structure_labels);
        write_bool(path, L"ESP", L"PlayerHealth", show_player_health);
        write_bool(path, L"ESP", L"DinoHealth", show_dino_health);
        write_bool(path, L"ESP", L"StructureHealth", show_structure_health);
        write_bool(path, L"ESP", L"PlayerTorpor", show_player_torpor);
        write_bool(path, L"ESP", L"DinoTorpor", show_dino_torpor);
        write_bool(path, L"ESP", L"CompactLabels", compact_labels);
        write_bool(path, L"ESP", L"TurretAmmo", turret_show_ammo);
        write_bool(path, L"ESP", L"TurretState", turret_show_state);
        write_bool(path, L"ESP", L"TurretPower", turret_show_power);
        write_bool(path, L"ESP", L"TurretRange", turret_show_range);
        write_bool(path, L"ESP", L"TurretTargetMode", turret_show_target_mode);
        write_bool(path, L"ESP", L"TurretTargetState", turret_show_target_state);
        write_bool(path, L"ESP", L"TurretWarning", turret_show_warning);
        write_bool(path, L"ESP", L"TurretHideNonmatching", turret_hide_nonmatching);
        write_uint(path, L"ESP", L"TurretTargetFilter", static_cast<std::uint32_t>(turret_target_filter));
        write_bool(path, L"ESP", L"BattleMode", battle_mode);
        write_bool(path, L"ESP", L"SmartDeclutter", smart_declutter);
        write_bool(path, L"ESP", L"StructureSummary", show_structure_summary);
        write_bool(path, L"ESP", L"PlayerSummary", show_player_summary);
        write_bool(path, L"ESP", L"DinoSummary", show_dino_summary);
        write_bool(path, L"ESP", L"SummaryUsesFilters", summary_uses_filters);
        write_uint(path, L"ESP", L"BoxStyle", static_cast<std::uint32_t>(esp_box_style));
        write_uint(path, L"ESP", L"PlayerColorSource", static_cast<std::uint32_t>(player_color_source));
        write_uint(path, L"ESP", L"LabelSide", static_cast<std::uint32_t>(esp_label_side));
        write_uint(path, L"ESP", L"HealthSide", static_cast<std::uint32_t>(esp_health_side));
        write_uint(path, L"ESP", L"TorporSide", static_cast<std::uint32_t>(esp_torpor_side));
        write_uint(path, L"ESP", L"StatusSide", static_cast<std::uint32_t>(esp_status_side));
        write_uint(path, L"ESP", L"WorldBoxStyle", static_cast<std::uint32_t>(world_box_style));
        write_uint(path, L"ESP", L"WorldLabelSide", static_cast<std::uint32_t>(world_label_side));
        write_uint(path, L"ESP", L"WorldHealthSide", static_cast<std::uint32_t>(world_health_side));
        write_uint(path, L"ESP", L"WorldTorporSide", static_cast<std::uint32_t>(world_torpor_side));
        write_float(path, L"ESP", L"Opacity", esp_opacity);
        write_float(path, L"ESP", L"BoxThickness", esp_box_thickness);
        write_float(path, L"ESP", L"SkeletonThickness", esp_skeleton_thickness);
        write_float(path, L"ESP", L"LabelSize", esp_label_size);
        write_float(path, L"ESP", L"IconSize", esp_icon_size);
        write_float(path, L"ESP", L"PlayerVisibilityGraceMs", player_visibility_grace_ms);
        write_float(path, L"ESP", L"RadarSize", radar_size);
        write_float(path, L"ESP", L"RadarRangeM", radar_range_m);
        write_float(path, L"ESP", L"ThreatDistanceM", threat_distance_m);
        write_float(path, L"ESP", L"RadarX", radar_x);
        write_float(path, L"ESP", L"RadarY", radar_y);
        write_float(path, L"ESP", L"StructureGroupRadiusM", structure_group_radius_m);
        write_float(path, L"ESP", L"DistanceM", esp_distance_m);
        write_float(path, L"ESP", L"DetailDistanceM", esp_detail_distance_m);
        write_float(path, L"ESP", L"DropDistanceM", drop_distance_m);
        write_float(path, L"ESP", L"RefreshMs", refresh_interval_ms);
        write_float(path, L"ESP", L"DiscoveryMs", discovery_interval_ms);
        write_float(path, L"ESP", L"DiscoveryBudgetMs", discovery_budget_ms);
        write_value(path, L"ESP", L"Search", esp_search);
        write_value(path, L"ESP", L"HiddenTribes", hidden_tribes);
        write_value(path, L"ESP", L"HiddenDinoTypes", hidden_dino_types);
        write_value(path, L"ESP", L"HiddenStructureTypes", hidden_structure_types);
        write_value(path, L"ESP", L"GroupedStructureTypes", grouped_structure_types);
        write_value(path, L"ESP", L"SelectedStructureTypes", selected_structure_types);
        write_value(path, L"ESP", L"KnownStructureTypes", known_structure_types);
        write_bool(path, L"Alerts", L"Enabled", alerts_enabled);
        write_bool(path, L"Alerts", L"NewPlayer", alert_new_player);
        write_bool(path, L"Alerts", L"Approach", alert_approach);
        write_bool(path, L"Alerts", L"Sleep", alert_sleep);
        write_bool(path, L"Alerts", L"Death", alert_death);
        write_bool(path, L"Alerts", L"Noglin", alert_noglin);
        write_bool(path, L"Alerts", L"Turret", alert_turret);
        write_bool(path, L"Alerts", L"EnemyGroup", alert_enemy_group);
        write_bool(path, L"Alerts", L"Sound", alert_sound);
        write_float(path, L"Alerts", L"RadiusM", alert_radius_m);
        write_float(path, L"Alerts", L"NoglinRadiusM", alert_noglin_radius_m);
        write_float(path, L"Alerts", L"ApproachSpeedMps", alert_approach_speed_mps);
        write_float(path, L"Alerts", L"LifetimeS", alert_lifetime_s);
        write_float(path, L"Alerts", L"CooldownS", alert_cooldown_s);

        write_bool(path, L"Camera", L"Freecam", freecam);
        write_float(path, L"Camera", L"FreecamSpeed", freecam_speed);
        write_float(path, L"Camera", L"SprintMultiplier", freecam_sprint_multiplier);
        write_float(path, L"Camera", L"VerticalMultiplier", freecam_vertical_multiplier);
        write_float(path, L"Camera", L"Smoothing", freecam_smoothing);
        write_float(path, L"Camera", L"MouseSensitivity", freecam_mouse_sensitivity);
        write_bool(path, L"Camera", L"FovOverride", fov_override);
        write_float(path, L"Camera", L"Fov", camera_fov);
        write_bool(path, L"Weapon", L"NoRecoil", no_recoil);
        write_bool(path, L"Weapon", L"NoSway", no_sway);
        write_bool(path, L"Chams", L"Local", local_chams);
        write_uint(path, L"Chams", L"LocalStyle", static_cast<std::uint32_t>(local_chams_style));
        write_bool(path, L"Chams", L"Enemies", enemy_chams);
        write_bool(path, L"Chams", L"Players", chams_players);
        write_bool(path, L"Chams", L"Dinos", chams_dinos);
        write_float(path, L"Chams", L"DistanceM", chams_distance_m);
        write_float(path, L"Chams", L"Budget", chams_budget);
        write_color(path, L"Own", own_color);
        write_color(path, L"Ally", ally_color);
        write_color(path, L"Enemy", enemy_color);
        write_color(path, L"PlayerAwake", player_awake_color);
        write_color(path, L"PlayerSleeping", player_sleeping_color);
        write_color(path, L"PlayerKnockedOut", player_knocked_out_color);
        write_color(path, L"PlayerDead", player_dead_color);
        write_color(path, L"PlayerOccluded", player_occluded_color);
        write_color(path, L"Wild", wild_color);
        write_color(path, L"Structure", structure_color);
        write_color(path, L"Health", health_color);
        write_color(path, L"Torpor", torpor_color);
        write_color(path, L"MenuAccent", menu_accent_color);
        write_color(path, L"LocalChams", local_chams_color);

        write_bool(path, L"Runtime", L"DebugPanel", debug_panel);
        write_bool(path, L"Runtime", L"AimLabRecording", aim_lab_recording);
        write_bool(path, L"Hotkeys", L"ShowList", show_hotkey_list);
        write_float(path, L"Hotkeys", L"PositionX", hotkey_list_x);
        write_float(path, L"Hotkeys", L"PositionY", hotkey_list_y);
        std::wstring serialized_bindings;
        for (const FeatureBinding& binding : feature_bindings)
        {
            if (binding.id.empty() || binding.key == 0) continue;
            if (!serialized_bindings.empty()) serialized_bindings += L';';
            serialized_bindings += binding.id + L"," + std::to_wstring(binding.key) + L"," +
                std::to_wstring(binding.mode) + L"," + (binding.show_in_list ? L"1" : L"0");
        }
        write_value(path, L"Hotkeys", L"FeatureBindings", serialized_bindings);
        write_uint(path, L"Runtime", L"ControlRotationOffset", control_rotation_offset);
        write_uint(path, L"Bindings", L"Menu", menu_key);
        write_uint(path, L"Bindings", L"Unload", unload_key);
        write_uint(path, L"Bindings", L"Freecam", freecam_key);
        write_uint(path, L"Bindings", L"EspToggle", esp_toggle_key);
        write_uint(path, L"Bindings", L"Panic", panic_key);
        write_bool(path, L"Share", L"Enabled", share_enabled);
        write_value(path, L"Share", L"Endpoint", share_endpoint);
        write_float(path, L"Share", L"IntervalMs", share_interval_ms);
        std::wstring teams;
        for (const auto team : allied_teams)
        {
            if (!teams.empty()) teams += L',';
            teams += std::to_wstring(team);
        }
        write_value(path, L"Relations", L"AlliedTeams", teams);
        return GetLastError() == ERROR_SUCCESS || std::filesystem::exists(path);
    }

    bool Settings::is_allied(const std::int32_t team) const
    {
        return team != 0 && std::binary_search(allied_teams.begin(), allied_teams.end(), team);
    }

    void Settings::set_allied(const std::int32_t team, const bool allied)
    {
        if (team == 0) return;
        const auto found = std::lower_bound(allied_teams.begin(), allied_teams.end(), team);
        if (allied && (found == allied_teams.end() || *found != team)) allied_teams.insert(found, team);
        else if (!allied && found != allied_teams.end() && *found == team) allied_teams.erase(found);
    }

    FeatureBinding* Settings::find_feature_binding(const std::wstring& id)
    {
        const auto found = std::find_if(feature_bindings.begin(), feature_bindings.end(), [&](const auto& binding) {
            return binding.id == id;
        });
        return found == feature_bindings.end() ? nullptr : &*found;
    }

    const FeatureBinding* Settings::find_feature_binding(const std::wstring& id) const
    {
        const auto found = std::find_if(feature_bindings.begin(), feature_bindings.end(), [&](const auto& binding) {
            return binding.id == id;
        });
        return found == feature_bindings.end() ? nullptr : &*found;
    }
}
