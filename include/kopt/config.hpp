#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace kopt
{
    struct Color
    {
        float r{};
        float g{};
        float b{};
        float a{1.0F};
    };

    struct FeatureBinding
    {
        std::wstring id;
        std::uint32_t key{};
        std::int32_t mode{}; // 0 = Hold, 1 = Toggle
        bool show_in_list{true};
    };

    struct UiLayout
    {
        float menu_width{780.0F};
        float menu_height{600.0F};
        float menu_x{0.50F};
        float menu_y{0.50F};
        float ui_scale{1.0F};
        float hotkey_x{0.82F};
        float hotkey_y{0.18F};
        float radar_x{0.86F};
        float radar_y{0.20F};
    };

    struct Settings
    {
        bool menu_open{true};
        float menu_width{780.0F};
        float menu_height{600.0F};
        float ui_scale{1.0F};
        std::int32_t active_layout{};
        std::array<UiLayout, 4> ui_layouts{};
        std::wstring favorite_features;

        bool player_aim{false};
        bool dino_aim{false};
        bool aim_target_enemies{true};
        bool aim_target_allies{false};
        bool visibility_check{true};
        bool random_hitbox{false};
        bool aim_lock{true};
        bool aim_draw_fov{true};
        std::int32_t aim_activation_mode{};
        std::int32_t dino_aim_activation_mode{};
        std::int32_t aim_hitbox_mode{};
        std::int32_t aim_hitbox{};
        std::int32_t aim_point_method{};
        std::uint32_t aim_hitbox_mask{0xFFU};
        std::int32_t aim_priority{};
        float aim_fov{12.0F};
        float aim_distance_m{500.0F};
        float aim_smoothing{5.0F};
        float aim_angle_boost{1.75F};
        float mounted_aim_fov{14.0F};
        float mounted_aim_smoothing{6.0F};
        bool aim_prediction{};
        bool aim_intercept_solver{true};
        float projectile_velocity_mps{300.0F};
        float projectile_gravity_mps2{9.81F};
        float prediction_latency_ms{};
        float random_shot_chance{0.35F};
        std::uint32_t aim_key{VK_RBUTTON};
        std::uint32_t dino_aim_key{VK_RBUTTON};
        bool aim_bind_show{true};
        bool dino_aim_bind_show{true};

        bool esp_enabled{true};
        bool player_esp{true};
        bool enemy_dino_esp{true};
        bool wild_dino_esp{false};
        bool structure_esp{true};
        bool turret_esp{true};
        bool drop_esp{false};
        bool death_cache_esp{false};
        bool player_item_cache_esp{true};
        bool dino_item_cache_esp{true};
        bool show_names{true};
        bool show_tribes{true};
        bool show_distance{true};
        bool show_health{true};
        bool show_torpor{true};
        bool show_vital_values{true};
        bool show_boxes{true};
        bool show_skeleton{false};
        bool show_dino_skeleton{false};
        bool show_held_items{true};
        bool show_equipment{true};
        bool show_turret_details{true};
        bool show_drop_quantity{true};
        bool show_radar{};
        bool show_threat_panel{true};
        bool structure_grouping{true};
        bool structure_whitelist_enabled{};
        bool show_tracers{false};
        bool offscreen_arrows{true};
        bool esp_show_enemies{true};
        bool esp_show_allies{false};
        bool esp_own_players{};
        bool esp_allied_players{};
        bool esp_enemy_players{true};
        bool esp_own_dinos{};
        bool esp_allied_dinos{};
        bool esp_enemy_dinos{true};
        bool esp_own_structures{};
        bool esp_allied_structures{};
        bool esp_enemy_structures{true};
        bool esp_neutral_structures{true};
        bool show_awake_players{true};
        bool show_sleeping_players{true};
        bool show_knocked_out_players{true};
        bool show_dead_players{};
        bool show_player_status{true};
        bool player_occluded_color_enabled{true};
        bool show_player_labels{true};
        bool show_dino_labels{true};
        bool show_structure_labels{true};
        bool show_player_health{true};
        bool show_dino_health{true};
        bool show_structure_health{true};
        bool show_player_torpor{true};
        bool show_dino_torpor{true};
        bool compact_labels{true};
        bool turret_show_ammo{true};
        bool turret_show_state{true};
        bool turret_show_power{true};
        bool turret_show_range{true};
        bool turret_show_target_mode{true};
        bool turret_show_target_state{true};
        bool turret_show_warning{};
        bool turret_hide_nonmatching{};
        std::int32_t turret_target_filter{-1};
        bool battle_mode{};
        bool smart_declutter{};
        bool show_structure_summary{};
        bool show_player_summary{};
        bool show_dino_summary{};
        bool summary_uses_filters{true};
        std::int32_t esp_box_style{};
        std::int32_t player_color_source{};
        std::int32_t esp_label_side{};
        std::int32_t esp_health_side{1};
        std::int32_t esp_torpor_side{2};
        std::int32_t esp_status_side{3};
        std::int32_t world_box_style{};
        std::int32_t world_label_side{};
        std::int32_t world_health_side{1};
        std::int32_t world_torpor_side{2};
        float esp_opacity{1.0F};
        float esp_box_thickness{1.5F};
        float esp_skeleton_thickness{1.2F};
        float esp_label_size{13.0F};
        float esp_icon_size{30.0F};
        float player_visibility_grace_ms{180.0F};
        float radar_size{180.0F};
        float radar_range_m{300.0F};
        float threat_distance_m{300.0F};
        float radar_x{0.86F};
        float radar_y{0.20F};
        float structure_group_radius_m{12.0F};
        float esp_distance_m{700.0F};
        float esp_detail_distance_m{5000.0F};
        float drop_distance_m{5000.0F};
        float refresh_interval_ms{33.0F};
        float discovery_interval_ms{1000.0F};
        float discovery_budget_ms{8.0F};
        std::wstring esp_search;
        std::wstring hidden_tribes;
        std::wstring hidden_dino_types;
        std::wstring hidden_structure_types;
        std::wstring grouped_structure_types;
        std::wstring selected_structure_types;
        std::wstring known_structure_types;

        bool alerts_enabled{};
        bool alert_new_player{true};
        bool alert_approach{true};
        bool alert_sleep{true};
        bool alert_death{true};
        bool alert_noglin{true};
        bool alert_turret{true};
        bool alert_enemy_group{true};
        bool alert_sound{};
        float alert_radius_m{250.0F};
        float alert_noglin_radius_m{120.0F};
        float alert_approach_speed_mps{10.0F};
        float alert_lifetime_s{7.0F};
        float alert_cooldown_s{8.0F};

        bool freecam{false};
        float freecam_speed{1200.0F};
        float freecam_sprint_multiplier{3.0F};
        float freecam_vertical_multiplier{1.0F};
        float freecam_smoothing{0.12F};
        float freecam_mouse_sensitivity{0.08F};
        bool fov_override{false};
        float camera_fov{112.5F};
        bool no_recoil{false};
        bool no_sway{false};

        bool local_chams{};
        std::int32_t local_chams_style{};
        bool enemy_chams{};
        bool chams_players{true};
        bool chams_dinos{true};
        float chams_distance_m{400.0F};
        float chams_budget{128.0F};

        bool debug_panel{true};
        bool aim_lab_recording{};
        bool show_hotkey_list{true};
        float hotkey_list_x{0.82F};
        float hotkey_list_y{0.18F};
        std::uint32_t control_rotation_offset{0x490};
        std::uint32_t menu_key{VK_HOME};
        std::uint32_t unload_key{VK_END};
        std::uint32_t freecam_key{VK_F6};
        std::uint32_t esp_toggle_key{VK_F7};
        std::uint32_t panic_key{VK_F12};

        Color own_color{0.29F, 0.90F, 0.62F, 1.0F};
        Color ally_color{0.29F, 0.68F, 1.0F, 1.0F};
        Color enemy_color{1.0F, 0.31F, 0.38F, 1.0F};
        Color player_awake_color{0.29F, 0.90F, 0.62F, 1.0F};
        Color player_sleeping_color{0.38F, 0.62F, 1.0F, 1.0F};
        Color player_knocked_out_color{1.0F, 0.72F, 0.30F, 1.0F};
        Color player_dead_color{0.92F, 0.22F, 0.34F, 1.0F};
        Color player_occluded_color{0.58F, 0.31F, 0.92F, 1.0F};
        Color wild_color{1.0F, 0.72F, 0.30F, 1.0F};
        Color structure_color{0.35F, 0.78F, 1.0F, 1.0F};
        Color health_color{0.28F, 0.92F, 0.42F, 1.0F};
        Color torpor_color{0.30F, 0.66F, 1.0F, 1.0F};
        Color menu_accent_color{0.545F, 0.361F, 0.965F, 1.0F};
        Color local_chams_color{0.64F, 0.24F, 1.0F, 1.0F};

        std::vector<std::int32_t> allied_teams;
        std::vector<FeatureBinding> feature_bindings;

        // Шеринг sightings/notifications с остальной командой поверх
        // голого QUIC (backend/backend_go/internal/quicserver -- НЕ
        // HTTP/3, у Go-стороны нет HTTP-семантики вообще) -- see
        // kopt::Publisher (publisher.hpp). Переименовано из relay_*: смысл
        // поля расширился с "WS-релей игроков/дино" до общего
        // двустороннего шеринга (структуры/турели/уведомления, QUIC-
        // транспорт вместо WinHTTP WS), и держать старое имя значило бы
        // врать о том, что внутри. Старые конфиги с секцией [Relay] молча
        // получат значения по умолчанию (шер выключен) -- осознанная цена
        // полной замены транспорта, не тихая потеря данных: умолчание
        // безопасной стороны.
        //
        // Токен авторизации намеренно не поле здесь -- kopt_injector.exe
        // --share-token публикует его в именованную shared-memory секцию
        // (Kopt_ShareToken_<pid>), которую payload читает один раз при
        // старте worker() (см. payload.cpp::read_share_token) -- та же
        // политика "только в памяти", что и у токенов загрузчика, чтобы он
        // никогда не оказался в этом текстовом ini.
        //
        // share_group_id/share_server_ip: то же самое "launch parameter,
        // не аккаунт-система" по духу, но это не секреты (группа и адрес
        // сервера, не учётные данные) -- живут в ini как есть, пока не
        // появится настоящий источник (аккаунт знает свои группы; ip:port
        // сервера клиент должен узнавать сам при подключении к игре --
        // отдельная, ещё не решённая задача).
        bool share_enabled{false};
        std::wstring share_endpoint{L"127.0.0.1:8443"};
        std::wstring share_group_id;
        std::wstring share_server_ip;
        float share_interval_ms{1000.0F};

        // Скан / отрисовка / отправка -- три независимые оси, не один флаг
        // на функцию: то, что клиент прочитал из памяти игры (скан), то,
        // что показывается в ESP-оверлее (отрисовка), и то, что уходит по
        // сети тиммейтам (отправка) -- разные решения пользователя. Пример:
        // просканировали сущность, отправить нужно, а рисовать локально --
        // нет. Или наоборот: не отправлять координаты своих же тиммейтов,
        // но продолжать видеть их в ESP.
        //
        // share_send_self_position -- первый экземпляр этого паттерна:
        // включает/выключает ТОЛЬКО отправку собственных координат (см.
        // share::build_self_sighting), на отрисовку никак не влияет (self
        // и так никогда не попадает в snapshot.actors и не рисуется ESP).
        // По умолчанию включено -- реальная настройка с дефолтом true, не
        // хардкод "всегда отправлять": пока без своего тумблера в
        // оверлее (Diagnostics-таб) -- это отдельный, более поздний шаг.
        // Остальные оси (например, "не отправлять чужих игроков, но видеть
        // их в ESP") добавляются позже тем же способом -- отдельное булево
        // поле в [Share], без переделки существующих.
        bool share_send_self_position{true};

        void normalize();
        bool load(const std::filesystem::path& path);
        bool save(const std::filesystem::path& path) const;
        [[nodiscard]] bool is_allied(std::int32_t team) const;
        void set_allied(std::int32_t team, bool allied);
        FeatureBinding* find_feature_binding(const std::wstring& id);
        [[nodiscard]] const FeatureBinding* find_feature_binding(const std::wstring& id) const;
    };
}
