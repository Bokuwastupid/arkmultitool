#pragma once

#include "kopt/config.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace kopt
{
    struct Vec2 { float x{}; float y{}; };
    struct Vec3 { float x{}; float y{}; float z{}; };

    enum class ActorKind : std::uint8_t
    {
        other,
        dino,
        structure,
        player,
        drop,
        death_cache
    };

    struct Actor
    {
        std::uintptr_t address{};
        std::uintptr_t root_component{};
        std::uint64_t linked_player_data_id{};
        ActorKind kind{ActorKind::other};
        Vec3 position{};
        Vec3 bounds_origin{};
        Vec3 bounds_extent{};
        Vec3 velocity{};
        std::int32_t team{};
        float health{};
        float max_health{};
        float torpor{};
        float max_torpor{};
        double last_render_time{};
        bool dead{};
        bool sleeping{};
        bool turret{};
        std::int32_t quantity{1};
        std::int32_t turret_ammo{-1};
        std::uint8_t turret_range{};
        std::uint8_t turret_targeting{};
        std::uint8_t turret_warning{};
        bool turret_powered{};
        bool turret_active{};
        bool turret_targeting_actor{};
        std::wstring class_name;
        std::wstring name;
        std::wstring tribe;
        std::wstring held_item;
        std::array<std::int32_t, 5> armor_types{};
        std::array<float, 5> armor_ratios{};
        std::array<Vec3, 23> bones{};
        std::int32_t bone_count{};
        bool bounds_valid{};
        float stale_seconds{};
        float refresh_elapsed{};
    };

    [[nodiscard]] inline bool actor_is_dead(const Actor& actor) noexcept
    {
        return actor.dead || (actor.max_health > 0.0F && actor.health <= 0.0F);
    }

    enum class PlayerEspState : std::uint8_t
    {
        awake,
        sleeping,
        knocked_out,
        dead
    };

    // Shared by overlay.cpp (ESP label/color) and relay_client's sighting
    // export -- both need the same "what is this player doing right now"
    // classification, and it was drifting out of sync as two copies before.
    [[nodiscard]] inline PlayerEspState player_esp_state(const Actor& actor) noexcept
    {
        if (actor_is_dead(actor)) return PlayerEspState::dead;
        const float torpor_ratio = actor.max_torpor > 0.0F ?
            std::clamp(actor.torpor / actor.max_torpor, 0.0F, 1.0F) : 0.0F;
        if (torpor_ratio >= 0.95F) return PlayerEspState::knocked_out;
        if (actor.sleeping) return PlayerEspState::sleeping;
        return PlayerEspState::awake;
    }

    struct Camera
    {
        Vec3 location{};
        Vec3 rotation{};
        float fov{};
        bool valid{};
    };

    struct AimTelemetry
    {
        std::uint64_t sequence{};
        double world_time{};
        bool active{};
        bool target_valid{};
        bool target_locked{};
        bool prediction{};
        bool intercept_solver{};
        bool visible{};
        std::uintptr_t target{};
        std::int32_t target_team{};
        std::int32_t bone_slot{-1};
        std::array<wchar_t, 64> target_name{};
        Vec3 camera{};
        Vec3 raw_bone{};
        Vec3 final_point{};
        Vec3 velocity{};
        float distance_m{};
        float angular_error{};
        float response{};
        float flight_seconds{};
    };

    enum class AlertKind : std::uint8_t
    {
        death,
        approach,
        new_player,
        sleep,
        noglin,
        turret,
        enemy_group
    };

    struct Alert
    {
        std::uint64_t id{};
        AlertKind kind{};
        std::wstring title;
        std::wstring name;
        std::wstring tribe;
        float distance_m{};
        float value{};
        float remaining_s{};
        // Абсолютная позиция цели, породившей событие. distance_m одного
        // достаточно для локальной карточки (та же точка отсчёта, что у
        // игрока, который её видит), но бесполезно на другом конце канала
        // шеринга без своей точки отсчёта -- см. kopt::share::Notification.
        // У групповых тревог (AlertKind::enemy_group) единой цели нет --
        // остаётся нулевым, это не баг, а честное отсутствие точки.
        Vec3 position{};
    };

    struct Snapshot
    {
        std::vector<Actor> actors;
        std::vector<Alert> alerts;
        std::uintptr_t world_address{};
        std::uint64_t world_generation{};
        Camera camera{};
        Vec3 local_position{};
        std::uintptr_t local_controller{};
        std::uintptr_t local_pawn{};
        std::uintptr_t local_character{};
        // Игровой account-id локального игрока (linked_player_data_id) --
        // тот же id, что уже кладётся в Actor::linked_player_data_id для
        // остальных игроков. Нужен отдельным полем, а не поиском по
        // local_character в actors: значение "липкое" (см. update()) и
        // остаётся известным даже в кадрах, где local_character временно не
        // резолвится, а kopt::share нужен именно устойчивый id для тега
        // reported_by и дедупликации своих же отчётов на приёме.
        std::uint64_t local_stable_id{};
        // Собственные имя/трайб -- читаются тем же offsets_.player_name/
        // tribe_name, что и для любого другого Actor::kind == player (см.
        // read_actor() в runtime.cpp), просто по local_character вместо
        // чужого адреса. Нужны как отдельные поля (не через actors -- self
        // туда никогда не попадает, см. share.hpp::build_self_sighting):
        // ark_relay's Entity.Validate() требует непустой label для
        // Category player/dino, так что пустая строка здесь -- не
        // "красивее", а разрывает всё QUIC-соединение отправителя.
        std::wstring local_name;
        std::wstring local_tribe;
        // Реальный адрес игрового сервера ("ip:port"), прочитанный из
        // UWorld->NetDriver->ServerConnection->URL (см. Offsets::net_driver
        // и read_remote_server_ip() в runtime.cpp) -- закрывает пробел из
        // плана DTO-шеринга §4: раньше server_ip для kopt::share был только
        // ручным значением из kopt_internal.ini, теперь клиент узнаёт его
        // сам при подключении к серверу, как и было задумано изначально.
        // Липко, как local_stable_id -- не затирается пустым значением на
        // кадре, где чтение временно не удалось. Пусто, пока не резолвится
        // (загрузочный экран/меню, ещё не в игре).
        std::wstring remote_server_ip;
        std::uintptr_t camera_manager{};
        std::int32_t local_team{};
        bool local_mounted{};
        bool local_valid{};
        bool aim_armed{};
        bool aim_active{};
        bool player_aim_active{};
        bool dino_aim_active{};
        std::uintptr_t aim_target{};
        AimTelemetry aim_debug{};
        double world_time{};
        std::uint64_t captures{};
        std::uint32_t rejected_reads{};
        float runtime_update_ms{};
        float discovery_ms{};
        float refresh_ms{};
        float oldest_actor_age_s{};
    };

    class ArkRuntime
    {
    public:
        bool initialize();
        void update(Settings& settings, float delta_seconds);
        void on_game_camera_tick(Settings& settings, std::uintptr_t camera_manager, float delta_seconds);
        void queue_freecam_mouse_delta(long x, long y) noexcept;
        void restore_transient_state();
        void clear_alert_history();
        [[nodiscard]] const Snapshot& snapshot() const noexcept { return snapshot_; }
        [[nodiscard]] bool world_to_screen(const Vec3& world, float width, float height, Vec2& screen) const;
        [[nodiscard]] const std::wstring& status() const noexcept { return status_; }
        [[nodiscard]] const std::wstring& chams_status() const noexcept { return chams_status_; }
        [[nodiscard]] std::size_t aim_trace_size() const noexcept { return aim_trace_count_; }
        [[nodiscard]] const AimTelemetry& aim_trace_sample(std::size_t index) const noexcept;
        void clear_aim_trace() noexcept;
        bool export_aim_trace(const std::filesystem::path& path) const;

    private:
        struct ClassMeta
        {
            ActorKind kind{ActorKind::other};
            bool turret{};
            std::wstring name;
        };

        struct Offsets
        {
            std::uintptr_t world_persistent_level{0xF8};
            std::uintptr_t world_levels{0x268};
            std::uintptr_t world_time_seconds{0xAE0};
            std::uintptr_t level_actors{0x88};
            std::uintptr_t actor_root_component{0x250};
            std::uintptr_t actor_last_render_time{0x288};
            std::uintptr_t component_to_world{0xE0};
            std::uintptr_t transform_translation{0x10};
            std::uintptr_t component_bounds{0x118};
            std::uintptr_t object_class{0x10};
            std::uintptr_t object_name{0x18};
            std::uintptr_t super_struct{0x30};
            std::uintptr_t targeting_team{0x218};
            std::uintptr_t owning_game_instance{0x290};
            std::uintptr_t local_players{0x38};
            std::uintptr_t player_controller{0x30};
            std::uintptr_t controller_pawn{0x488};
            std::uintptr_t acknowledged_pawn{0x4D0};
            std::uintptr_t camera_manager{0x4F0};
            std::uintptr_t camera_cache{0x4D0};
            std::uintptr_t camera_pov{0x8};
            std::uintptr_t current_health{0x92C};
            std::uintptr_t max_health{0x930};
            std::uintptr_t is_dead{0x898};
            std::uintptr_t is_sleeping{0x884};
            std::uintptr_t descriptive_name{0xBE8};
            std::uintptr_t tribe_name{0x790};
            std::uintptr_t player_name{0x14B0};
            std::uintptr_t linked_player_data_id{0x1720};
            std::uintptr_t structure_name{0x4E8};
            std::uintptr_t status_component{0xCD0};
            std::uintptr_t status_current_values{0x818};
            std::uintptr_t status_max_values{0xD8};
            std::uintptr_t character_mesh{0x4F8};
            std::uintptr_t mesh_space_bases{0x688};
            // UWorld->NetDriver->ServerConnection->URL.Host/Port -- нашли
            // живым сканом по имени класса (см. историю ArkRuntime::
            // scan_net_connection в git-логе), не документированы Wildcard
            // нигде. NetDriver/ServerConnection на этой сборке резолвятся в
            // SteamNetDriver/SteamNetConnection (ARK поверх Steam
            // Networking Sockets, не голый UE4 IpNetDriver) -- если
            // Wildcard когда-нибудь сменит транспорт, эти четыре оффсета
            // нужно переоткрывать заново тем же приёмом, не гадать.
            std::uintptr_t net_driver{0x108};
            std::uintptr_t net_driver_server_connection{0x78};
            std::uintptr_t connection_url_host{0xF0};
            std::uintptr_t connection_url_port{0x100};
        } offsets_{};

        bool resolve_globals();
        bool capture();
        bool refresh_known(float delta_seconds);
        bool refresh_actor_dynamic(Actor& actor, float elapsed_seconds);
        bool read_local();
        bool read_actor(std::uintptr_t address, Actor& actor);
        bool read_player_bones(std::uintptr_t address, const Vec3& actor_position, Actor& actor);
        void read_player_equipment(std::uintptr_t address, Actor& actor);
        float read_item_stat(std::uintptr_t item, int stat_index) const;
        ClassMeta class_meta(std::uintptr_t class_address);
        std::wstring object_name(std::uintptr_t object_address);
        // Читает UWorld->NetDriver->ServerConnection->URL.Host/Port через
        // Offsets::net_driver/net_driver_server_connection/
        // connection_url_host/connection_url_port (см. их doc-комментарий).
        // Возвращает пусто, если любое из чтений по цепочке не удалось --
        // snapshot_.remote_server_ip остаётся прежним значением (липко),
        // не затирается.
        std::wstring read_remote_server_ip(std::uintptr_t world);
        std::wstring resolve_name(std::int32_t index, std::int32_t number);
        std::wstring read_fstring(std::uintptr_t address, std::size_t cap = 256);
        bool read_vec3(std::uintptr_t address, Vec3& value) const;
        bool read_component_bounds(std::uintptr_t component, Vec3& origin, Vec3& extent) const;
        bool engine_object_live(std::uintptr_t address) const;
        bool engine_actor_live(const Actor& actor) const;
        void run_aim(Settings& settings, float delta_seconds);
        void record_aim_sample(const Settings& settings, float delta_seconds);
        void run_camera(Settings& settings, float delta_seconds);
        void update_no_recoil(const Settings& settings);
        void restore_no_recoil() noexcept;
        void update_no_sway(const Settings& settings);
        void restore_no_sway() noexcept;
        void update_alerts(const Settings& settings);
        void abandon_alerts() noexcept;
        void update_chams(const Settings& settings);
        void restore_chams();
        void abandon_chams() noexcept;

        template <typename T>
        bool read(std::uintptr_t address, T& value) const
        {
            SIZE_T bytes{};
            return address >= 0x10000 &&
                ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                    &value, sizeof(T), &bytes) != FALSE && bytes == sizeof(T);
        }

        template <typename T>
        bool write(std::uintptr_t address, const T& value) const
        {
            SIZE_T bytes{};
            return address >= 0x10000 &&
                WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address),
                    &value, sizeof(T), &bytes) != FALSE && bytes == sizeof(T);
        }

        std::uintptr_t module_base_{};
        std::size_t module_size_{};
        std::uintptr_t g_world_{};
        std::uintptr_t g_names_{};
        std::uintptr_t active_world_{};
        std::uintptr_t pending_world_{};
        std::chrono::steady_clock::time_point pending_world_since_{};
        std::uint64_t world_generation_{};
        Snapshot snapshot_{};
        std::unordered_map<std::uintptr_t, ClassMeta> class_cache_;
        std::unordered_map<std::int32_t, std::wstring> name_cache_;
        std::chrono::steady_clock::time_point last_capture_{};
        std::chrono::steady_clock::time_point last_live_refresh_{};
        std::chrono::steady_clock::time_point chams_ready_after_{};
        std::chrono::steady_clock::time_point local_pawn_ready_after_{};
        std::uintptr_t observed_local_pawn_{};
        std::uint64_t local_player_data_id_{};
        std::vector<std::uintptr_t> discovery_candidates_;
        std::size_t discovery_cursor_{};
        float discovery_budget_ms_{8.0F};
        std::wstring status_{L"Waiting for ARK runtime"};
        struct ChamState
        {
            bool render_custom_depth{};
            bool force_wireframe{};
            std::int32_t stencil{};
            std::int32_t applied_stencil{};
            bool applied_custom_depth{};
            bool applied_wireframe{};
            std::uintptr_t owner{};
            std::uint64_t world_generation{};
        };
        std::unordered_map<std::uintptr_t, ChamState> cham_states_;
        std::wstring chams_status_{L"First-person chams are disabled"};
        struct AlertTracker
        {
            Vec3 position{};
            std::chrono::steady_clock::time_point motion_sample{};
            std::chrono::steady_clock::time_point last_seen{};
            std::array<std::chrono::steady_clock::time_point, 7> last_alert{};
            bool sleeping{};
            bool knocked_out{};
            bool dead{};
            bool noglin_inside{};
            bool turret_threat{};
        };
        struct ActiveAlert
        {
            Alert value;
            std::chrono::steady_clock::time_point expires{};
        };
        std::unordered_map<std::uintptr_t, AlertTracker> alert_trackers_;
        std::vector<ActiveAlert> active_alerts_;
        std::chrono::steady_clock::time_point alerts_ready_after_{};
        std::uint64_t next_alert_id_{1};
        int previous_enemy_group_count_{};
        std::chrono::steady_clock::time_point last_group_alert_{};
        bool initialized_{};
        bool no_recoil_applied_{};
        struct SwayState
        {
            std::uintptr_t pawn{};
            std::uintptr_t weapon{};
            std::uint64_t world_generation{};
            float current_bob_speed{};
            float applied_bob{};
            Vec3 bob_magnitudes{};
            Vec3 bob_offsets{};
            Vec3 targeting_bob_magnitudes{};
            Vec3 targeting_bob_offsets{};
            float aim_drift_yaw{};
            float aim_drift_pitch{};
            bool active{};
        } sway_state_;
        bool freecam_was_enabled_{};
        bool need_player_bones_{};
        bool need_equipment_{};
        bool need_held_items_{};
        bool aim_toggle_active_{};
        bool aim_key_was_down_{};
        bool aim_key_armed_{};
        bool dino_aim_toggle_active_{};
        bool dino_aim_key_was_down_{};
        bool dino_aim_key_armed_{};
        std::uint32_t last_aim_key_{};
        std::uint32_t last_dino_aim_key_{};
        std::int32_t last_aim_activation_mode_{-1};
        std::int32_t last_dino_aim_activation_mode_{-1};
        bool last_player_aim_{};
        bool last_dino_aim_{};
        bool last_menu_open_{true};
        std::uintptr_t locked_target_{};
        float locked_target_occluded_seconds_{};
        std::array<AimTelemetry, 600> aim_trace_{};
        std::size_t aim_trace_head_{};
        std::size_t aim_trace_count_{};
        std::uint64_t aim_trace_sequence_{};
        float aim_trace_elapsed_{};
        float profiler_age_elapsed_{};
        Vec3 freecam_position_{};
        Vec3 freecam_rotation_{};
        Vec3 freecam_target_rotation_{};
        std::atomic<long> freecam_mouse_x_{};
        std::atomic<long> freecam_mouse_y_{};
        Vec3 freecam_restore_position_{};
        Vec3 freecam_restore_rotation_{};
        float freecam_restore_fov_{};
        std::uintptr_t freecam_pov_{};
    };
}
