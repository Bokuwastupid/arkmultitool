#pragma once

#include "kopt/config.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
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
        death_cache,
        horde_crate,
        element_node,
        explorer_note
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
        std::int32_t horde_wave{-1};
        std::int32_t horde_tier{};
        std::int32_t element_reward{-1};
        bool reward_exact{};
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
        std::wstring reward_preview;
        // Why a crate's loot readout ended up exact or "pending": which property
        // resolved, what the inventory pointer was, how many slots came back.
        // Logged once per actor so a failing path can be read after the fact.
        std::wstring reward_diagnostic;
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
    };

    // One past contact, kept long after its alert expired. Alerts answer "what is
    // happening"; the journal answers "what happened while I was away", so it is
    // retained by wall-clock age rather than by the alert lifetime.
    //
    // Shaped for eventual sharing between players through a backend, which is why
    // it stores absolute facts rather than only what one observer saw: the world
    // position of the contact (another player's "180 m away" is not yours), the
    // event kind as a stable enum rather than a display string, an absolute
    // timestamp, and the world generation the sighting belongs to. distance and
    // bearing stay as observer-relative conveniences for the local panel.
    struct JournalEntry
    {
        std::chrono::system_clock::time_point time{};
        AlertKind kind{};
        std::wstring title;
        std::wstring name;
        std::wstring tribe;
        Vec3 position{};
        std::uint64_t world_generation{};
        float distance_m{};
        // Compass bearing from the local player to the contact, degrees, or NaN
        // when the event has no single actor behind it (an enemy-group alert).
        float bearing_deg{};
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
        std::uintptr_t camera_manager{};
        std::int32_t local_team{};
        std::wstring local_pawn_class_name;
        std::wstring controller_pawn_class_name;
        bool local_mounted{};
        bool controller_reports_riding{};
        bool managarmr_safe_aim{};
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
        [[nodiscard]] const std::vector<JournalEntry>& journal() const noexcept { return journal_; }
        void clear_journal() noexcept { journal_.clear(); }
        [[nodiscard]] const Snapshot& snapshot() const noexcept { return snapshot_; }
        [[nodiscard]] bool world_to_screen(const Vec3& world, float width, float height, Vec2& screen) const;
        [[nodiscard]] const std::wstring& status() const noexcept { return status_; }
        [[nodiscard]] const std::wstring& chams_status() const noexcept { return chams_status_; }
        [[nodiscard]] std::size_t aim_trace_size() const noexcept { return aim_trace_count_; }
        [[nodiscard]] const AimTelemetry& aim_trace_sample(std::size_t index) const noexcept;
        void clear_aim_trace() noexcept;
        bool export_aim_trace(const std::filesystem::path& path) const;

        // Time series kept per actor so features can reason about change rather
        // than a single instant: torpor drain, movement trails, contact history.
        // Sampled on its own cadence, not every refresh, and only for actors the
        // features actually care about - keeping this for every tracked actor
        // would cost memory and frame time for nothing.
        struct ActorSample
        {
            double world_time{};
            Vec3 position{};
            float torpor{};
            float health{};
        };
        struct ActorHistory
        {
            static constexpr std::size_t capacity = 64;
            std::array<ActorSample, capacity> samples{};
            std::size_t next{};
            std::size_t filled{};
            std::uint64_t world_generation{};
            void push(const ActorSample& sample)
            {
                samples[next] = sample;
                next = (next + 1) % capacity;
                filled = std::min(filled + 1, capacity);
            }
            // Oldest first, so callers can walk a trail forwards in time.
            [[nodiscard]] const ActorSample& at(const std::size_t index) const
            {
                const std::size_t start = filled == capacity ? next : 0;
                return samples[(start + index) % capacity];
            }
        };
        [[nodiscard]] const ActorHistory* actor_history(std::uintptr_t address) const;

    private:
        void sample_actor_history();
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
        struct PropertyMeta
        {
            std::uintptr_t field{};
            std::int32_t offset{-1};
        };
        PropertyMeta property_meta(std::uintptr_t class_address, const wchar_t* property_name);
        bool read_bool_property(std::uintptr_t instance, std::uintptr_t class_address,
            const wchar_t* property_name, bool& value);
        void read_horde_details(std::uintptr_t class_address, Actor& actor);
        ClassMeta class_meta(std::uintptr_t class_address);
        std::wstring object_name(std::uintptr_t object_address);
        std::wstring resolve_name(std::int32_t index, std::int32_t number);
        std::wstring read_fstring(std::uintptr_t address, std::size_t cap = 256);
        bool read_vec3(std::uintptr_t address, Vec3& value) const;
        bool read_component_bounds(std::uintptr_t component, Vec3& origin, Vec3& extent) const;
        bool engine_object_live(std::uintptr_t address) const;
        bool engine_actor_live(const Actor& actor) const;
        void run_aim(Settings& settings, float delta_seconds);
        void restore_managarmr_aim_offset() noexcept;
        void record_aim_sample(const Settings& settings, float delta_seconds);
        void run_camera(Settings& settings, float delta_seconds);
        void restore_freecam_near_clip() noexcept;
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
        std::unordered_map<std::wstring, PropertyMeta> property_cache_;
        std::unordered_map<std::int32_t, std::wstring> name_cache_;
        std::unordered_map<std::uintptr_t, ActorHistory> actor_history_;
        std::chrono::steady_clock::time_point last_history_sample_{};
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
        std::vector<JournalEntry> journal_;
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
        std::uintptr_t managarmr_aim_offset_pawn_{};
        bool managarmr_aim_offset_dirty_{};
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
        float freecam_restore_near_clip_{};
        bool freecam_near_clip_applied_{};
    };
}
