#include "kopt/runtime.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <limits>
#include <mmsystem.h>
#include <numbers>
#include <unordered_set>

namespace
{
    using namespace kopt;

    constexpr float max_coordinate = 10'000'000.0F;
    constexpr float radians_to_degrees = 180.0F / std::numbers::pi_v<float>;

    struct Pattern
    {
        std::vector<std::uint8_t> bytes;
        std::vector<bool> exact;
    };

    struct NativeTransform
    {
        float rotation_x{}, rotation_y{}, rotation_z{}, rotation_w{};
        float translation_x{}, translation_y{}, translation_z{}, translation_padding{};
        float scale_x{}, scale_y{}, scale_z{}, scale_padding{};

        [[nodiscard]] bool valid() const
        {
            const float norm = rotation_x * rotation_x + rotation_y * rotation_y +
                rotation_z * rotation_z + rotation_w * rotation_w;
            return std::isfinite(norm) && norm > 0.5F && norm < 1.5F &&
                std::isfinite(translation_x) && std::isfinite(translation_y) && std::isfinite(translation_z) &&
                scale_x > 0.01F && scale_x < 100.0F && scale_y > 0.01F && scale_y < 100.0F &&
                scale_z > 0.01F && scale_z < 100.0F;
        }

        [[nodiscard]] Vec3 transform_position(const Vec3& point) const
        {
            const Vec3 scaled{point.x * scale_x, point.y * scale_y, point.z * scale_z};
            const float dot = rotation_x * scaled.x + rotation_y * scaled.y + rotation_z * scaled.z;
            const float vector_length = rotation_x * rotation_x + rotation_y * rotation_y + rotation_z * rotation_z;
            const Vec3 cross{
                rotation_y * scaled.z - rotation_z * scaled.y,
                rotation_z * scaled.x - rotation_x * scaled.z,
                rotation_x * scaled.y - rotation_y * scaled.x};
            return {
                2.0F * dot * rotation_x + (rotation_w * rotation_w - vector_length) * scaled.x +
                    2.0F * rotation_w * cross.x + translation_x,
                2.0F * dot * rotation_y + (rotation_w * rotation_w - vector_length) * scaled.y +
                    2.0F * rotation_w * cross.y + translation_y,
                2.0F * dot * rotation_z + (rotation_w * rotation_w - vector_length) * scaled.z +
                    2.0F * rotation_w * cross.z + translation_z};
        }
    };

    static_assert(sizeof(NativeTransform) == 0x30);

    Pattern parse_pattern(const char* value)
    {
        Pattern result;
        while (*value != '\0')
        {
            while (*value == ' ') ++value;
            if (*value == '\0') break;
            if (*value == '?')
            {
                result.bytes.push_back(0);
                result.exact.push_back(false);
                while (*value == '?') ++value;
            }
            else
            {
                char token[3]{value[0], value[1], '\0'};
                result.bytes.push_back(static_cast<std::uint8_t>(std::strtoul(token, nullptr, 16)));
                result.exact.push_back(true);
                value += 2;
            }
            while (*value != '\0' && *value != ' ') ++value;
        }
        return result;
    }

    bool readable(const DWORD protection)
    {
        if ((protection & PAGE_GUARD) != 0 || (protection & PAGE_NOACCESS) != 0) return false;
        const DWORD base = protection & 0xFF;
        return base == PAGE_READONLY || base == PAGE_READWRITE || base == PAGE_WRITECOPY ||
            base == PAGE_EXECUTE_READ || base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
    }

    std::uintptr_t scan(std::uintptr_t start, const std::size_t size, const Pattern& pattern)
    {
        if (pattern.bytes.empty() || size < pattern.bytes.size()) return 0;
        const std::uintptr_t end = start + size;
        std::uintptr_t cursor = start;
        while (cursor < end)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi)) == 0) break;
            const auto region_start = std::max(cursor, reinterpret_cast<std::uintptr_t>(mbi.BaseAddress));
            const auto region_end = std::min(end,
                reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize);
            if (mbi.State == MEM_COMMIT && readable(mbi.Protect) && region_end > region_start)
            {
                std::vector<std::uint8_t> buffer(region_end - region_start);
                SIZE_T bytes{};
                if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(region_start),
                        buffer.data(), buffer.size(), &bytes) != FALSE && bytes >= pattern.bytes.size())
                {
                    for (std::size_t i = 0; i + pattern.bytes.size() <= bytes; ++i)
                    {
                        bool match = true;
                        for (std::size_t j = 0; j < pattern.bytes.size(); ++j)
                        {
                            if (pattern.exact[j] && buffer[i + j] != pattern.bytes[j])
                            {
                                match = false;
                                break;
                            }
                        }
                        if (match) return region_start + i;
                    }
                }
            }
            if (region_end <= cursor) break;
            cursor = region_end;
        }
        return 0;
    }

    float distance(const Vec3& a, const Vec3& b)
    {
        const float x = a.x - b.x;
        const float y = a.y - b.y;
        const float z = a.z - b.z;
        return std::sqrt(x * x + y * y + z * z);
    }

    float normalize_angle(float angle)
    {
        while (angle > 180.0F) angle -= 360.0F;
        while (angle < -180.0F) angle += 360.0F;
        return angle;
    }

    float intercept_time(const Vec3& relative, const Vec3& velocity, const float projectile_speed)
    {
        const float distance_squared = relative.x * relative.x + relative.y * relative.y + relative.z * relative.z;
        if (!std::isfinite(distance_squared) || distance_squared <= 0.0F ||
            !std::isfinite(projectile_speed) || projectile_speed <= 1.0F)
        {
            return 0.0F;
        }

        const float fallback = std::sqrt(distance_squared) / projectile_speed;
        const float velocity_squared = velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z;
        const float a = velocity_squared - projectile_speed * projectile_speed;
        const float b = 2.0F * (relative.x * velocity.x + relative.y * velocity.y + relative.z * velocity.z);
        const float c = distance_squared;
        constexpr float epsilon = 0.001F;

        if (std::abs(a) <= epsilon)
        {
            if (std::abs(b) > epsilon)
            {
                const float linear_time = -c / b;
                if (std::isfinite(linear_time) && linear_time > 0.0F) return linear_time;
            }
            return fallback;
        }

        const float discriminant = b * b - 4.0F * a * c;
        if (!std::isfinite(discriminant) || discriminant < 0.0F) return fallback;
        const float root = std::sqrt(discriminant);
        const float denominator = 2.0F * a;
        const float first = (-b - root) / denominator;
        const float second = (-b + root) / denominator;
        float result = std::numeric_limits<float>::max();
        if (std::isfinite(first) && first > 0.0F) result = first;
        if (std::isfinite(second) && second > 0.0F) result = std::min(result, second);
        return result == std::numeric_limits<float>::max() ? fallback : result;
    }

    std::wstring lower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](const wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return value;
    }

    std::wstring friendly_name(std::wstring value)
    {
        const std::array suffixes{L"_Character_BP_C", L"_BP_C", L"_C"};
        for (const auto* suffix : suffixes)
        {
            const std::wstring ending(suffix);
            if (value.size() >= ending.size() &&
                _wcsicmp(value.c_str() + value.size() - ending.size(), ending.c_str()) == 0)
            {
                value.resize(value.size() - ending.size());
                break;
            }
        }
        std::replace(value.begin(), value.end(), L'_', L' ');
        return value.empty() ? L"Unknown" : value;
    }
}

namespace kopt
{
    void ArkRuntime::clear_aim_trace() noexcept
    {
        aim_trace_head_ = 0;
        aim_trace_count_ = 0;
        aim_trace_elapsed_ = 0.0F;
    }

    const AimTelemetry& ArkRuntime::aim_trace_sample(const std::size_t index) const noexcept
    {
        static const AimTelemetry empty{};
        if (index >= aim_trace_count_) return empty;
        return aim_trace_[(aim_trace_head_ + index) % aim_trace_.size()];
    }

    bool ArkRuntime::export_aim_trace(const std::filesystem::path& path) const
    {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) return false;
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output << "sequence,world_time,active,target_valid,target_locked,prediction,intercept,visible,"
            "target,team,bone_slot,distance_m,angular_error,response,flight_seconds,"
            "camera_x,camera_y,camera_z,raw_x,raw_y,raw_z,final_x,final_y,final_z,"
            "velocity_x,velocity_y,velocity_z\n";
        output.setf(std::ios::fixed);
        output.precision(5);
        for (std::size_t index = 0; index < aim_trace_count_; ++index)
        {
            const AimTelemetry& sample = aim_trace_sample(index);
            output << sample.sequence << ',' << sample.world_time << ',' << sample.active << ','
                << sample.target_valid << ',' << sample.target_locked << ',' << sample.prediction << ','
                << sample.intercept_solver << ',' << sample.visible << ',' << sample.target << ','
                << sample.target_team << ',' << sample.bone_slot << ',' << sample.distance_m << ','
                << sample.angular_error << ',' << sample.response << ',' << sample.flight_seconds << ','
                << sample.camera.x << ',' << sample.camera.y << ',' << sample.camera.z << ','
                << sample.raw_bone.x << ',' << sample.raw_bone.y << ',' << sample.raw_bone.z << ','
                << sample.final_point.x << ',' << sample.final_point.y << ',' << sample.final_point.z << ','
                << sample.velocity.x << ',' << sample.velocity.y << ',' << sample.velocity.z << '\n';
        }
        return output.good();
    }

    void ArkRuntime::record_aim_sample(const Settings& settings, const float delta_seconds)
    {
        if (!settings.aim_lab_recording)
        {
            aim_trace_elapsed_ = 0.0F;
            return;
        }
        aim_trace_elapsed_ += std::clamp(delta_seconds, 0.0F, 0.10F);
        if (aim_trace_elapsed_ < (1.0F / 30.0F)) return;
        aim_trace_elapsed_ = std::fmod(aim_trace_elapsed_, 1.0F / 30.0F);
        AimTelemetry sample = snapshot_.aim_debug;
        sample.sequence = ++aim_trace_sequence_;
        if (aim_trace_count_ < aim_trace_.size())
        {
            aim_trace_[(aim_trace_head_ + aim_trace_count_) % aim_trace_.size()] = sample;
            ++aim_trace_count_;
        }
        else
        {
            aim_trace_[aim_trace_head_] = sample;
            aim_trace_head_ = (aim_trace_head_ + 1) % aim_trace_.size();
        }
    }

    void ArkRuntime::queue_freecam_mouse_delta(const long x, const long y) noexcept
    {
        freecam_mouse_x_.fetch_add(x, std::memory_order_relaxed);
        freecam_mouse_y_.fetch_add(y, std::memory_order_relaxed);
    }

    bool ArkRuntime::initialize()
    {
        const HMODULE module = GetModuleHandleW(nullptr);
        if (module == nullptr)
        {
            status_ = L"Main module is unavailable";
            return false;
        }
        module_base_ = reinterpret_cast<std::uintptr_t>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module_base_);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        {
            status_ = L"Invalid DOS header";
            return false;
        }
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module_base_ + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64)
        {
            status_ = L"Payload requires ShooterGame Win64";
            return false;
        }
        module_size_ = nt->OptionalHeader.SizeOfImage;
        initialized_ = resolve_globals();
        if (initialized_)
        {
            status_ = L"ARK globals resolved";
            capture();
        }
        return initialized_;
    }

    bool ArkRuntime::resolve_globals()
    {
        static const Pattern world_pattern = parse_pattern(
            "48 89 4C 24 ?? 48 81 EC ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 8C 24 ?? ?? ?? ?? "
            "E8 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 8B 80 ?? ?? ?? ?? 48 83 78 ?? ?? 74 2C 48 8B 05 ??");
        static const Pattern names_pattern = parse_pattern(
            "E8 ?? ?? ?? ?? 48 89 1D ?? ?? ?? ?? 48 8B C3 48 8B 5C 24 ?? 48 83 C4 28 C3 48 8B 5C 24 ??");

        const auto world_match = scan(module_base_, module_size_, world_pattern);
        if (world_match == 0)
        {
            status_ = L"GWorld signature was not found (wrong ASE build)";
            return false;
        }
        std::int32_t displacement{};
        if (!read(world_match + 76, displacement)) return false;
        g_world_ = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(world_match + 80) + displacement);

        const auto names_match = scan(module_base_, module_size_, names_pattern);
        if (names_match == 0)
        {
            status_ = L"GNames signature was not found (wrong ASE build)";
            return false;
        }
        if (!read(names_match + 8, displacement)) return false;
        const auto names_global = static_cast<std::uintptr_t>(
            static_cast<std::intptr_t>(names_match + 12) + displacement);
        std::uintptr_t candidate{};
        if (read(names_global, candidate) && candidate >= 0x10000)
        {
            g_names_ = candidate;
            if (resolve_name(0, 0) != L"None") g_names_ = names_global;
        }
        else
        {
            g_names_ = names_global;
        }
        if (resolve_name(0, 0) != L"None")
        {
            status_ = L"GNames failed the index-0 validation";
            return false;
        }
        return true;
    }

    void ArkRuntime::update(Settings& settings, const float delta_seconds)
    {
        const auto update_started = std::chrono::steady_clock::now();
        if (!initialized_)
        {
            initialize();
            snapshot_.runtime_update_ms = std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - update_started).count();
            return;
        }

        // Aim hit zones use the same compact player-skeleton cache as Skeleton ESP.
        // Keeping this tied only to show_skeleton made aim silently fall back to rough
        // height offsets whenever the visual skeleton was disabled.
        need_player_bones_ = settings.show_skeleton || settings.player_aim;
        need_equipment_ = settings.show_equipment ||
            (settings.player_aim && settings.aim_hitbox_mode == 1 && settings.aim_point_method != 2);
        need_held_items_ = settings.show_held_items;
        discovery_budget_ms_ = settings.discovery_budget_ms;
        read_local();
        const auto now = std::chrono::steady_clock::now();
        const bool discovery_due = last_capture_.time_since_epoch().count() == 0 ||
            std::chrono::duration<float, std::milli>(now - last_capture_).count() >= settings.discovery_interval_ms;
        if (discovery_due)
        {
            const auto discovery_started = std::chrono::steady_clock::now();
            if (capture())
            {
                last_capture_ = now;
                last_live_refresh_ = now;
            }
            snapshot_.discovery_ms = std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - discovery_started).count();
        }
        else if (last_live_refresh_.time_since_epoch().count() == 0 ||
            std::chrono::duration<float, std::milli>(now - last_live_refresh_).count() >= settings.refresh_interval_ms)
        {
            const auto refresh_started = std::chrono::steady_clock::now();
            const float elapsed = last_live_refresh_.time_since_epoch().count() == 0 ?
                settings.refresh_interval_ms * 0.001F : std::chrono::duration<float>(now - last_live_refresh_).count();
            refresh_known(std::clamp(elapsed, 0.0F, 0.5F));
            last_live_refresh_ = now;
            snapshot_.refresh_ms = std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - refresh_started).count();
        }
        update_alerts(settings);
        profiler_age_elapsed_ += std::clamp(delta_seconds, 0.0F, 0.10F);
        if (profiler_age_elapsed_ >= 1.0F)
        {
            profiler_age_elapsed_ = std::fmod(profiler_age_elapsed_, 1.0F);
            snapshot_.oldest_actor_age_s = 0.0F;
            for (const Actor& actor : snapshot_.actors)
                snapshot_.oldest_actor_age_s = std::max(snapshot_.oldest_actor_age_s, actor.stale_seconds);
        }
        snapshot_.runtime_update_ms = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - update_started).count();
    }

    bool ArkRuntime::capture()
    {
        // read_local() selects and validates the active gameplay UWorld once per
        // Present. Do not sample GWorld again here: ARK temporarily points that
        // global at auxiliary worlds while rendering/loading, which used to mix
        // actors from one world with the local state of another.
        const std::uintptr_t world = active_world_;
        if (world < 0x10000 || snapshot_.world_address != world)
        {
            status_ = L"Waiting for UWorld";
            return false;
        }
        std::erase_if(snapshot_.actors, [&](const Actor& actor) {
            return actor.address == snapshot_.local_pawn || actor.address == snapshot_.local_character ||
                (actor.kind == ActorKind::player && local_player_data_id_ != 0 &&
                    actor.linked_player_data_id == local_player_data_id_);
        });

        std::vector<std::uintptr_t> levels;
        std::uintptr_t level_data{};
        std::int32_t level_count{};
        if (read(world + offsets_.world_levels, level_data) &&
            read(world + offsets_.world_levels + 8, level_count) && level_count > 0 && level_count <= 256)
        {
            levels.resize(static_cast<std::size_t>(level_count));
            SIZE_T bytes{};
            if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(level_data),
                    levels.data(), levels.size() * sizeof(std::uintptr_t), &bytes) == FALSE)
            {
                levels.clear();
            }
        }
        std::uintptr_t persistent{};
        if (read(world + offsets_.world_persistent_level, persistent) && persistent >= 0x10000)
        {
            levels.push_back(persistent);
        }

        std::unordered_set<std::uintptr_t> unique_levels;
        std::unordered_set<std::uintptr_t> unique_actors;
        std::vector<std::uintptr_t> candidates;
        candidates.reserve(discovery_candidates_.capacity() > 0 ? discovery_candidates_.capacity() : 8192);
        std::uint32_t rejected{};
        bool enumeration_complete = true;

        for (const auto level : levels)
        {
            if (level < 0x10000 || !unique_levels.insert(level).second) continue;
            std::uintptr_t actor_data{};
            std::int32_t actor_count{};
            if (!read(level + offsets_.level_actors, actor_data) ||
                !read(level + offsets_.level_actors + 8, actor_count) ||
                actor_count < 0 || actor_count > 200000)
            {
                ++rejected;
                enumeration_complete = false;
                continue;
            }
            std::vector<std::uintptr_t> addresses(static_cast<std::size_t>(actor_count));
            SIZE_T bytes{};
            if (actor_count > 0 && ReadProcessMemory(GetCurrentProcess(),
                    reinterpret_cast<const void*>(actor_data), addresses.data(),
                    addresses.size() * sizeof(std::uintptr_t), &bytes) == FALSE)
            {
                ++rejected;
                enumeration_complete = false;
                continue;
            }
            for (const auto address : addresses)
            {
                if (address < 0x10000 || address == snapshot_.local_pawn || address == snapshot_.local_character ||
                    !unique_actors.insert(address).second) continue;
                candidates.push_back(address);
            }
        }
        if (enumeration_complete)
        {
            std::erase_if(snapshot_.actors, [&](const Actor& actor) { return !unique_actors.contains(actor.address); });
        }
        discovery_candidates_ = std::move(candidates);
        if (discovery_candidates_.empty())
        {
            discovery_cursor_ = 0;
            snapshot_.rejected_reads = rejected;
            ++snapshot_.captures;
            status_ = L"Live: no discoverable actors";
            return true;
        }
        discovery_cursor_ %= discovery_candidates_.size();
        std::unordered_map<std::uintptr_t, std::size_t> existing;
        existing.reserve(snapshot_.actors.size());
        for (std::size_t index = 0; index < snapshot_.actors.size(); ++index)
            existing.emplace(snapshot_.actors[index].address, index);

        const auto scan_started = std::chrono::steady_clock::now();
        std::size_t scanned{};
        const std::size_t total = discovery_candidates_.size();
        while (scanned < total)
        {
            const std::uintptr_t address = discovery_candidates_[discovery_cursor_];
            discovery_cursor_ = (discovery_cursor_ + 1) % total;
            ++scanned;
            const auto known = existing.find(address);
            if (known != existing.end() && (scanned % 64) != 0)
            {
                if (scanned >= 32 && std::chrono::duration<float, std::milli>(
                    std::chrono::steady_clock::now() - scan_started).count() >= discovery_budget_ms_) break;
                continue;
            }
            Actor actor;
            if (read_actor(address, actor))
            {
                if (actor.address == snapshot_.local_character ||
                    (actor.kind == ActorKind::player && local_player_data_id_ != 0 &&
                        actor.linked_player_data_id == local_player_data_id_)) continue;
                if (known == existing.end())
                {
                    existing.emplace(address, snapshot_.actors.size());
                    snapshot_.actors.push_back(std::move(actor));
                }
                else
                {
                    actor.velocity = snapshot_.actors[known->second].velocity;
                    actor.stale_seconds = snapshot_.actors[known->second].stale_seconds;
                    actor.refresh_elapsed = snapshot_.actors[known->second].refresh_elapsed;
                    snapshot_.actors[known->second] = std::move(actor);
                }
            }
            else
            {
                ++rejected;
            }
            if (scanned >= 32 && std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - scan_started).count() >= discovery_budget_ms_) break;
        }
        snapshot_.rejected_reads = rejected;
        ++snapshot_.captures;
        status_ = L"Live: " + std::to_wstring(snapshot_.actors.size()) + L" tracked, discovery " +
            std::to_wstring(scanned) + L"/" + std::to_wstring(total) + L" this slice";
        return true;
    }

    bool ArkRuntime::refresh_known(const float delta_seconds)
    {
        if (!read_local()) return false;
        for (Actor& actor : snapshot_.actors)
        {
            actor.refresh_elapsed += delta_seconds;
            const float cadence = actor.kind == ActorKind::player || actor.kind == ActorKind::dino ? 0.0F :
                actor.kind == ActorKind::drop || actor.kind == ActorKind::death_cache ? 0.10F : 0.50F;
            if (actor.refresh_elapsed < cadence) continue;
            const float attempted_elapsed = actor.refresh_elapsed;
            actor.refresh_elapsed = 0.0F;
            if (refresh_actor_dynamic(actor, attempted_elapsed)) actor.stale_seconds = 0.0F;
            else actor.stale_seconds += attempted_elapsed;
        }
        std::erase_if(snapshot_.actors, [](const Actor& actor) {
            const float retention = actor.kind == ActorKind::player || actor.kind == ActorKind::dino ? 1.2F : 2.5F;
            return actor.stale_seconds > retention;
        });
        status_ = L"Live 30 Hz: " + std::to_wstring(snapshot_.actors.size()) + L" tracked actors";
        return true;
    }

    bool ArkRuntime::refresh_actor_dynamic(Actor& actor, const float elapsed_seconds)
    {
        std::uintptr_t root{};
        Vec3 position{};
        if (!read(actor.address + offsets_.actor_root_component, root) || root < 0x10000 ||
            !read_vec3(root + offsets_.component_to_world + offsets_.transform_translation, position))
            return false;
        actor.root_component = root;
        actor.bounds_valid = read_component_bounds(root, actor.bounds_origin, actor.bounds_extent) &&
            distance(actor.bounds_origin, position) <=
                std::sqrt(actor.bounds_extent.x * actor.bounds_extent.x +
                    actor.bounds_extent.y * actor.bounds_extent.y +
                    actor.bounds_extent.z * actor.bounds_extent.z) * 4.0F + 5000.0F;
        read(actor.address + offsets_.actor_last_render_time, actor.last_render_time);
        if (!std::isfinite(actor.last_render_time) || actor.last_render_time < 0.0)
            actor.last_render_time = 0.0;
        if (elapsed_seconds > 0.001F)
        {
            const Vec3 sample{(position.x - actor.position.x) / elapsed_seconds,
                (position.y - actor.position.y) / elapsed_seconds,
                (position.z - actor.position.z) / elapsed_seconds};
            const float speed_squared = sample.x * sample.x + sample.y * sample.y + sample.z * sample.z;
            if (std::isfinite(speed_squared) && speed_squared <= 200'000.0F * 200'000.0F)
            {
                constexpr float sample_weight = 0.38F;
                actor.velocity.x += (sample.x - actor.velocity.x) * sample_weight;
                actor.velocity.y += (sample.y - actor.velocity.y) * sample_weight;
                actor.velocity.z += (sample.z - actor.velocity.z) * sample_weight;
            }
            else actor.velocity = {};
        }
        actor.position = position;
        read(actor.address + offsets_.targeting_team, actor.team);

        actor.health = actor.max_health = actor.torpor = actor.max_torpor = 0.0F;
        std::uintptr_t status{};
        if (read(actor.address + offsets_.status_component, status) && status >= 0x10000)
        {
            read(status + offsets_.status_current_values, actor.health);
            read(status + offsets_.status_max_values, actor.max_health);
            read(status + offsets_.status_current_values + 2 * sizeof(float), actor.torpor);
            read(status + offsets_.status_max_values + 2 * sizeof(float), actor.max_torpor);
            const auto validate = [](float& current, float& maximum) {
                if (!std::isfinite(current) || !std::isfinite(maximum) || maximum <= 0.0F ||
                    maximum > 100'000'000.0F || current < -1.0F || current > maximum * 2.0F)
                {
                    current = maximum = 0.0F;
                    return;
                }
                current = std::clamp(current, 0.0F, maximum);
            };
            validate(actor.health, actor.max_health);
            validate(actor.torpor, actor.max_torpor);
        }
        if (actor.max_health <= 0.0F)
        {
            read(actor.address + offsets_.current_health, actor.health);
            read(actor.address + offsets_.max_health, actor.max_health);
        }
        std::uint8_t state{};
        if (read(actor.address + offsets_.is_dead, state)) actor.dead = (state & 0x20) != 0;
        if (read(actor.address + offsets_.is_sleeping, state)) actor.sleeping = (state & 0x01) != 0;
        actor.bone_count = 0;
        if (actor.kind == ActorKind::player && need_player_bones_)
            read_player_bones(actor.address, actor.position, actor);
        return true;
    }

    bool ArkRuntime::read_local()
    {
        std::uintptr_t world{};
        std::uintptr_t game_instance{};
        std::uintptr_t players_data{};
        std::int32_t players_count{};
        std::uintptr_t local_player{};
        std::uintptr_t controller{};
        std::uintptr_t pawn{};
        std::uintptr_t possessed_pawn{};
        std::uintptr_t root{};
        const auto invalidate_local = [&]() {
            if (observed_local_pawn_ != 0)
            {
                observed_local_pawn_ = 0;
                local_pawn_ready_after_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
                locked_target_ = 0;
                locked_target_occluded_seconds_ = 0.0F;
                aim_toggle_active_ = false;
                dino_aim_toggle_active_ = false;
                sway_state_ = {};
                freecam_was_enabled_ = false;
                freecam_pov_ = 0;
                abandon_chams();
            }
            snapshot_.local_valid = false;
            snapshot_.camera = {};
            snapshot_.local_controller = 0;
            snapshot_.local_pawn = 0;
            snapshot_.local_character = 0;
            snapshot_.camera_manager = 0;
            snapshot_.local_team = 0;
            snapshot_.local_mounted = false;
        };
        if (!read(g_world_, world) || world < 0x10000)
        {
            pending_world_ = 0;
            pending_world_since_ = {};
            if (active_world_ != 0)
            {
                active_world_ = 0;
                snapshot_.world_address = 0;
                snapshot_.world_generation = ++world_generation_;
                snapshot_.actors.clear();
                discovery_candidates_.clear();
                discovery_cursor_ = 0;
                abandon_chams();
                abandon_alerts();
                freecam_was_enabled_ = false;
                freecam_pov_ = 0;
                locked_target_ = 0;
                locked_target_occluded_seconds_ = 0.0F;
            }
            invalidate_local();
            return false;
        }
        Vec3 local_position{};
        if (!read(world + offsets_.owning_game_instance, game_instance) ||
            !read(game_instance + offsets_.local_players, players_data) ||
            !read(game_instance + offsets_.local_players + 8, players_count) || players_count < 1 || players_count > 16 ||
            !read(players_data, local_player) || !read(local_player + offsets_.player_controller, controller) ||
            !read(controller + offsets_.acknowledged_pawn, pawn) || !read(pawn + offsets_.actor_root_component, root) ||
            !read_vec3(root + offsets_.component_to_world + offsets_.transform_translation, local_position))
        {
            // GWorld can briefly reference a preview/loading world which has no
            // valid local-player chain. Preserve the confirmed gameplay world for
            // a short grace period, but stop using stale local state if a real
            // travel persists.
            if (active_world_ != 0 && world != active_world_)
            {
                const auto now = std::chrono::steady_clock::now();
                if (pending_world_ != world)
                {
                    pending_world_ = world;
                    pending_world_since_ = now;
                    return snapshot_.local_valid;
                }
                if (now - pending_world_since_ < std::chrono::milliseconds(250))
                    return snapshot_.local_valid;
            }
            invalidate_local();
            return false;
        }
        pending_world_ = 0;
        pending_world_since_ = {};
        if (world != active_world_)
        {
            // Only a UWorld with a complete local-player chain may become active.
            // This prevents auxiliary worlds from churning generations and cached
            // UObject pointers during GC/seamless travel.
            active_world_ = world;
            snapshot_.world_address = world;
            snapshot_.world_generation = ++world_generation_;
            snapshot_.actors.clear();
            discovery_candidates_.clear();
            discovery_cursor_ = 0;
            class_cache_.clear();
            local_player_data_id_ = 0;
            abandon_chams();
            abandon_alerts();
            freecam_was_enabled_ = false;
            freecam_pov_ = 0;
            locked_target_ = 0;
            locked_target_occluded_seconds_ = 0.0F;
            chams_ready_after_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        }
        if (pawn != observed_local_pawn_)
        {
            observed_local_pawn_ = pawn;
            local_pawn_ready_after_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
            locked_target_ = 0;
            locked_target_occluded_seconds_ = 0.0F;
            aim_toggle_active_ = false;
            aim_key_was_down_ = false;
            aim_key_armed_ = false;
            dino_aim_toggle_active_ = false;
            dino_aim_key_was_down_ = false;
            dino_aim_key_armed_ = false;
            sway_state_ = {};
            freecam_was_enabled_ = false;
            freecam_pov_ = 0;
            abandon_chams();
        }
        snapshot_.local_valid = true;
        snapshot_.local_position = local_position;
        snapshot_.local_controller = controller;
        snapshot_.local_pawn = pawn;
        snapshot_.local_character = 0;
        read(controller + offsets_.controller_pawn, possessed_pawn);
        read(pawn + offsets_.targeting_team, snapshot_.local_team);
        snapshot_.local_mounted = false;
        std::uintptr_t pawn_class{};
        if (read(pawn + offsets_.object_class, pawn_class) && pawn_class >= 0x10000)
            snapshot_.local_mounted = class_meta(pawn_class).kind == ActorKind::dino;
        if (!snapshot_.local_mounted)
        {
            snapshot_.local_character = pawn;
        }
        else if (possessed_pawn >= 0x10000)
        {
            std::uintptr_t possessed_class{};
            if (read(possessed_pawn + offsets_.object_class, possessed_class) && possessed_class >= 0x10000 &&
                class_meta(possessed_class).kind == ActorKind::player)
                snapshot_.local_character = possessed_pawn;
        }
        if (snapshot_.local_character >= 0x10000)
        {
            std::uint64_t linked_id{};
            if (read(snapshot_.local_character + offsets_.linked_player_data_id, linked_id) && linked_id != 0)
                local_player_data_id_ = linked_id;
        }
        if (snapshot_.local_character == 0 && local_player_data_id_ != 0)
        {
            const auto local_actor = std::find_if(snapshot_.actors.begin(), snapshot_.actors.end(), [&](const Actor& actor) {
                return actor.kind == ActorKind::player && actor.linked_player_data_id == local_player_data_id_;
            });
            if (local_actor != snapshot_.actors.end()) snapshot_.local_character = local_actor->address;
        }
        snapshot_.local_stable_id = local_player_data_id_;
        read(world + offsets_.world_time_seconds, snapshot_.world_time);
        if (!std::isfinite(snapshot_.world_time) || snapshot_.world_time < 0.0) snapshot_.world_time = 0.0;

        std::uintptr_t manager{};
        if (read(controller + offsets_.camera_manager, manager) && manager >= 0x10000)
        {
            snapshot_.camera_manager = manager;
            const auto pov = manager + offsets_.camera_cache + offsets_.camera_pov;
            float fov{};
            if (read_vec3(pov, snapshot_.camera.location) &&
                read_vec3(pov + 0xC, snapshot_.camera.rotation) && read(pov + 0x28, fov) &&
                fov >= 10.0F && fov <= 170.0F)
            {
                snapshot_.camera.fov = fov;
                snapshot_.camera.valid = true;
            }
        }
        return snapshot_.local_valid;
    }

    void ArkRuntime::on_game_camera_tick(Settings& settings, const std::uintptr_t camera_manager,
        const float delta_seconds)
    {
        if (camera_manager < 0x10000 || camera_manager != snapshot_.camera_manager) return;
        const auto pov = camera_manager + offsets_.camera_cache + offsets_.camera_pov;
        float fov{};
        if (read_vec3(pov, snapshot_.camera.location) && read_vec3(pov + 0xC, snapshot_.camera.rotation) &&
            read(pov + 0x28, fov) && fov >= 10.0F && fov <= 170.0F)
        {
            snapshot_.camera.fov = fov;
            snapshot_.camera.valid = true;
        }
        if (std::chrono::steady_clock::now() < local_pawn_ready_after_)
        {
            snapshot_.aim_active = false;
            snapshot_.aim_target = 0;
            locked_target_ = 0;
            locked_target_occluded_seconds_ = 0.0F;
            update_no_recoil(settings);
            snapshot_.aim_debug = {};
            snapshot_.aim_debug.world_time = snapshot_.world_time;
            record_aim_sample(settings, delta_seconds);
            return;
        }
        run_camera(settings, delta_seconds);
        update_no_recoil(settings);
        update_no_sway(settings);
        run_aim(settings, delta_seconds);
        record_aim_sample(settings, delta_seconds);
        update_chams(settings);
    }

    void ArkRuntime::update_chams(const Settings& settings)
    {
        constexpr std::uintptr_t set_stencil_rva = 0x2ADB5A0;
        static constexpr std::array<std::uint8_t, 6> prologue{0x40, 0x53, 0x48, 0x83, 0xEC, 0x20};
        if (std::memcmp(reinterpret_cast<const void*>(module_base_ + set_stencil_rva), prologue.data(), prologue.size()) != 0)
        {
            chams_status_ = L"Unavailable: render-state symbol does not match this ARK build";
            abandon_chams();
            return;
        }
        if (active_world_ == 0 || snapshot_.world_address != active_world_ ||
            !snapshot_.local_valid || !snapshot_.camera.valid)
        {
            chams_status_ = L"First-person chams paused during world transition";
            return;
        }
        if (std::chrono::steady_clock::now() < chams_ready_after_)
        {
            chams_status_ = L"First-person chams waiting for the new world to stabilize";
            return;
        }
        if (!settings.local_chams)
        {
            restore_chams();
            chams_status_ = L"First-person chams are disabled";
            return;
        }
        std::unordered_map<std::uintptr_t, std::int32_t> desired;
        const auto component_ready = [&](const std::uintptr_t component) {
            void* vtable{};
            std::uint32_t object_flags{}, component_flags{}, render_flags{};
            if (component < 0x10000 || !read(component, vtable) ||
                !read(component + 0x8, object_flags) || !read(component + 0xA8, component_flags) ||
                !read(component + 0xC8, render_flags)) return false;
            const auto address = reinterpret_cast<std::uintptr_t>(vtable);
            return address >= module_base_ && address < module_base_ + module_size_ &&
                (object_flags & 0x00018000U) == 0 &&
                (component_flags & 1U) != 0 && (component_flags & (1U << 13)) != 0 &&
                (render_flags & 1U) != 0;
        };
        std::unordered_map<std::uintptr_t, std::uintptr_t> desired_owners;
        const auto add_component = [&](const std::uintptr_t component, const std::int32_t stencil,
            const std::uintptr_t owner) {
            if (component_ready(component))
            {
                desired[component] = stencil;
                desired_owners[component] = owner;
            }
        };
        if (snapshot_.local_pawn >= 0x10000)
        {
            // ShooterGame 358.26 PDB profile: AShooterCharacter::Mesh1P is the
            // first-person arms component. Never include Character::Mesh (the
            // third-person body) in local chams.
            std::uintptr_t first_person_arms{};
            if (read(snapshot_.local_pawn + 0x14C8, first_person_arms))
                add_component(first_person_arms, 252, snapshot_.local_pawn);
            std::uintptr_t weapon{};
            if (read(snapshot_.local_pawn + 0x1708, weapon) && weapon >= 0x10000)
            {
                // AShooterWeapon::Mesh1P only. Mesh3P intentionally stays untouched.
                std::uintptr_t first_person_weapon{};
                if (read(weapon + 0x8A8, first_person_weapon))
                    add_component(first_person_weapon, 252, snapshot_.local_pawn);
            }
        }

        using SetStencilFn = void(__fastcall*)(void*, int);
        const auto set_stencil = reinterpret_cast<SetStencilFn>(module_base_ + set_stencil_rva);
        // ForceWireframe is also the render-thread marker for the DrawIndexed
        // solid-color pass. Solid replaces the marked draw; Combined draws the
        // replacement and then the original wireframe.
        const bool desired_wireframe = true;
        constexpr std::uintptr_t skinned_mesh_flags_offset = 0x72C;
        constexpr std::uint32_t force_wireframe_mask = 1U << 1;
        for (const auto& [component, stencil] : desired)
        {
            if (!component_ready(component)) continue;
            auto state = cham_states_.find(component);
            if (state == cham_states_.end())
            {
                std::uint32_t flags{}, skinned_mesh_flags{};
                std::int32_t original_stencil{};
                if (!read(component + 0x1F4, flags) || !read(component + 0x1F8, original_stencil) ||
                    !read(component + skinned_mesh_flags_offset, skinned_mesh_flags)) continue;
                state = cham_states_.emplace(component,
                    ChamState{(flags & (1U << 19)) != 0,
                        (skinned_mesh_flags & force_wireframe_mask) != 0, original_stencil, stencil,
                        (flags & (1U << 19)) != 0,
                        (skinned_mesh_flags & force_wireframe_mask) != 0,
                        desired_owners[component], snapshot_.world_generation}).first;
                if (desired_wireframe) skinned_mesh_flags |= force_wireframe_mask;
                else skinned_mesh_flags &= ~force_wireframe_mask;
                write(component + skinned_mesh_flags_offset, skinned_mesh_flags);
                set_stencil(reinterpret_cast<void*>(component), stencil);
                set_stencil(reinterpret_cast<void*>(component), original_stencil);
                state->second.applied_stencil = original_stencil;
                state->second.applied_wireframe = desired_wireframe;
            }
            else
            {
                const bool wireframe_changed = state->second.applied_wireframe != desired_wireframe;
                if (wireframe_changed)
                {
                    std::uint32_t skinned_mesh_flags{};
                    if (read(component + skinned_mesh_flags_offset, skinned_mesh_flags))
                    {
                        if (desired_wireframe) skinned_mesh_flags |= force_wireframe_mask;
                        else skinned_mesh_flags &= ~force_wireframe_mask;
                        write(component + skinned_mesh_flags_offset, skinned_mesh_flags);
                    }
                }
                if (state->second.applied_stencil != stencil)
                {
                    set_stencil(reinterpret_cast<void*>(component), stencil);
                    state->second.applied_stencil = stencil;
                }
                // A render-state update is required after changing bForceWireframe.
                // Changing the stencil through the engine is the safe game-thread
                // path and avoids calling MarkRenderStateDirty on stale components.
                else if (wireframe_changed)
                {
                    set_stencil(reinterpret_cast<void*>(component), stencil == 255 ? 254 : stencil + 1);
                    set_stencil(reinterpret_cast<void*>(component), stencil);
                }
                state->second.applied_wireframe = desired_wireframe;
            }
        }
        for (auto iterator = cham_states_.begin(); iterator != cham_states_.end();)
        {
            if (desired.contains(iterator->first))
            {
                ++iterator;
                continue;
            }
            const bool owner_live = iterator->second.world_generation == snapshot_.world_generation &&
                ((iterator->second.owner == snapshot_.local_pawn && snapshot_.local_valid) ||
                    std::any_of(snapshot_.actors.begin(), snapshot_.actors.end(), [&](const Actor& actor) {
                        return actor.address == iterator->second.owner && actor.stale_seconds <= 0.0F && !actor.dead;
                    }));
            if (owner_live && component_ready(iterator->first))
            {
                std::uint32_t skinned_mesh_flags{};
                if (read(iterator->first + skinned_mesh_flags_offset, skinned_mesh_flags))
                {
                    if (iterator->second.force_wireframe) skinned_mesh_flags |= force_wireframe_mask;
                    else skinned_mesh_flags &= ~force_wireframe_mask;
                    write(iterator->first + skinned_mesh_flags_offset, skinned_mesh_flags);
                }
                const int dirty_stencil = iterator->second.stencil == 255 ? 254 : iterator->second.stencil + 1;
                set_stencil(reinterpret_cast<void*>(iterator->first), dirty_stencil);
                set_stencil(reinterpret_cast<void*>(iterator->first), iterator->second.stencil);
            }
            iterator = cham_states_.erase(iterator);
        }
        static constexpr std::array<const wchar_t*, 3> styles{L"Solid", L"Wireframe", L"Solid + wireframe"};
        chams_status_ = std::wstring(styles[std::clamp(settings.local_chams_style, 0, 2)]) + L" active on " +
            std::to_wstring(cham_states_.size()) + L" first-person components";
    }

    void ArkRuntime::restore_chams()
    {
        if (cham_states_.empty() || module_base_ == 0 || active_world_ == 0 ||
            snapshot_.world_address != active_world_ || !snapshot_.local_valid)
        {
            abandon_chams();
            return;
        }
        using SetStencilFn = void(__fastcall*)(void*, int);
        const auto set_stencil = reinterpret_cast<SetStencilFn>(module_base_ + 0x2ADB5A0);
        constexpr std::uintptr_t skinned_mesh_flags_offset = 0x72C;
        constexpr std::uint32_t force_wireframe_mask = 1U << 1;
        for (const auto& [component, state] : cham_states_)
        {
            void* vtable{};
            std::uint32_t object_flags{}, component_flags{}, render_flags{};
            const bool owner_live = state.world_generation == snapshot_.world_generation &&
                ((state.owner == snapshot_.local_pawn && snapshot_.local_valid) ||
                    std::any_of(snapshot_.actors.begin(), snapshot_.actors.end(), [&](const Actor& actor) {
                        return actor.address == state.owner && actor.stale_seconds <= 0.0F && !actor.dead;
                    }));
            if (!owner_live || !read(component, vtable) || !read(component + 0x8, object_flags) ||
                !read(component + 0xA8, component_flags) ||
                !read(component + 0xC8, render_flags) || (component_flags & 1U) == 0 ||
                (component_flags & (1U << 13)) == 0 || (render_flags & 1U) == 0 ||
                (object_flags & 0x00018000U) != 0 ||
                reinterpret_cast<std::uintptr_t>(vtable) < module_base_ ||
                reinterpret_cast<std::uintptr_t>(vtable) >= module_base_ + module_size_) continue;
            std::uint32_t skinned_mesh_flags{};
            if (read(component + skinned_mesh_flags_offset, skinned_mesh_flags))
            {
                if (state.force_wireframe) skinned_mesh_flags |= force_wireframe_mask;
                else skinned_mesh_flags &= ~force_wireframe_mask;
                write(component + skinned_mesh_flags_offset, skinned_mesh_flags);
            }
            const int dirty_stencil = state.stencil == 255 ? 254 : state.stencil + 1;
            set_stencil(reinterpret_cast<void*>(component), dirty_stencil);
            set_stencil(reinterpret_cast<void*>(component), state.stencil);
        }
        cham_states_.clear();
    }

    void ArkRuntime::abandon_chams() noexcept
    {
        cham_states_.clear();
        chams_status_ = L"First-person chams cache cleared for world transition";
    }

    void ArkRuntime::clear_alert_history()
    {
        active_alerts_.clear();
        snapshot_.alerts.clear();
    }

    void ArkRuntime::abandon_alerts() noexcept
    {
        alert_trackers_.clear();
        active_alerts_.clear();
        snapshot_.alerts.clear();
        previous_enemy_group_count_ = 0;
        last_group_alert_ = {};
        alerts_ready_after_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    }

    void ArkRuntime::update_alerts(const Settings& settings)
    {
        const auto now = std::chrono::steady_clock::now();
        active_alerts_.erase(std::remove_if(active_alerts_.begin(), active_alerts_.end(),
            [&](const ActiveAlert& alert) { return alert.expires <= now; }), active_alerts_.end());
        if (!settings.alerts_enabled || !snapshot_.local_valid || snapshot_.world_address != active_world_)
        {
            if (!settings.alerts_enabled)
            {
                active_alerts_.clear();
                alert_trackers_.clear();
            }
            snapshot_.alerts.clear();
            return;
        }

        const bool ready = now >= alerts_ready_after_;
        bool play_sound{};
        const auto append_alert = [&](Alert alert) {
            alert.id = next_alert_id_++;
            alert.remaining_s = settings.alert_lifetime_s;
            if (active_alerts_.size() >= 4) active_alerts_.erase(active_alerts_.begin());
            active_alerts_.push_back({std::move(alert),
                now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<float>(settings.alert_lifetime_s))});
            play_sound = true;
        };
        const auto emit_actor = [&](const Actor& actor, AlertTracker& tracker, const AlertKind kind,
            const std::wstring& title, const float value = 0.0F) {
            if (!ready) return;
            const auto index = static_cast<std::size_t>(kind);
            const auto last = tracker.last_alert[index];
            if (last.time_since_epoch().count() != 0 &&
                std::chrono::duration<float>(now - last).count() < settings.alert_cooldown_s) return;
            tracker.last_alert[index] = now;
            const std::wstring display_name = !actor.name.empty() ? actor.name :
                (actor.kind == ActorKind::player ? L"Unknown player" : L"Unknown actor");
            const std::wstring display_tribe = !actor.tribe.empty() ? actor.tribe :
                (actor.kind == ActorKind::player ? L"Unknown tribe" : L"");
            append_alert({0, kind, title, display_name, display_tribe,
                distance(actor.position, snapshot_.local_position) / 100.0F, value, 0.0F,
                actor.position});
        };

        int enemy_group_count{};
        std::unordered_set<std::uintptr_t> seen;
        seen.reserve(snapshot_.actors.size());
        for (const Actor& actor : snapshot_.actors)
        {
            if (actor.address < 0x10000 || actor.stale_seconds > 0.0F) continue;
            const bool allied = (snapshot_.local_team != 0 && actor.team == snapshot_.local_team) ||
                settings.is_allied(actor.team);
            const bool enemy_player = actor.kind == ActorKind::player && !allied;
            const bool dead = actor_is_dead(actor);
            const bool noglin = actor.kind == ActorKind::dino &&
                lower(actor.name).find(L"noglin") != std::wstring::npos && !allied;
            const bool hostile_turret = actor.turret && !allied;
            if (!enemy_player && !noglin && !hostile_turret) continue;

            seen.insert(actor.address);
            const float distance_m = distance(actor.position, snapshot_.local_position) / 100.0F;
            const float torpor_ratio = actor.max_torpor > 0.0F ?
                std::clamp(actor.torpor / actor.max_torpor, 0.0F, 1.0F) : 0.0F;
            const bool knocked_out = torpor_ratio >= 0.95F;
            const bool initially_noglin_inside = noglin && distance_m <= settings.alert_noglin_radius_m;
            const bool turret_threat = hostile_turret && actor.turret_active && actor.turret_powered &&
                actor.turret_targeting_actor && distance_m <= settings.alert_radius_m;
            const auto [iterator, inserted] = alert_trackers_.try_emplace(actor.address);
            AlertTracker& tracker = iterator->second;
            if (inserted)
            {
                tracker.position = actor.position;
                tracker.motion_sample = now;
                tracker.sleeping = actor.sleeping;
                tracker.knocked_out = knocked_out;
                tracker.dead = dead;
                tracker.noglin_inside = initially_noglin_inside;
                tracker.turret_threat = turret_threat;
                if (enemy_player && !dead && !actor.sleeping && distance_m <= settings.alert_radius_m &&
                    settings.alert_new_player)
                    emit_actor(actor, tracker, AlertKind::new_player, L"New enemy player");
                if (initially_noglin_inside && settings.alert_noglin)
                    emit_actor(actor, tracker, AlertKind::noglin, L"Noglin in range");
                if (turret_threat && settings.alert_turret)
                    emit_actor(actor, tracker, AlertKind::turret, L"Turret targeting threat");
            }
            tracker.last_seen = now;
            const bool noglin_inside = noglin && distance_m <= settings.alert_noglin_radius_m *
                (tracker.noglin_inside ? 1.20F : 1.0F);

            if (enemy_player)
            {
                if (!dead && distance_m <= settings.alert_radius_m) ++enemy_group_count;
                const float sample_seconds = std::chrono::duration<float>(now - tracker.motion_sample).count();
                if (!dead && !actor.sleeping && sample_seconds >= 0.25F)
                {
                    const float old_distance = distance(tracker.position, snapshot_.local_position);
                    const float closing_speed_mps = (old_distance - distance(actor.position, snapshot_.local_position)) /
                        sample_seconds / 100.0F;
                    if (settings.alert_approach && distance_m > 100.0F && distance_m <= settings.alert_radius_m &&
                        closing_speed_mps >= settings.alert_approach_speed_mps)
                        emit_actor(actor, tracker, AlertKind::approach, L"Enemy approaching", closing_speed_mps);
                    tracker.position = actor.position;
                    tracker.motion_sample = now;
                }
                if (!dead && settings.alert_sleep && knocked_out && !tracker.knocked_out)
                    emit_actor(actor, tracker, AlertKind::sleep, L"Enemy knocked out", torpor_ratio * 100.0F);
                else if (!dead && settings.alert_sleep && actor.sleeping && !tracker.sleeping && !knocked_out)
                    emit_actor(actor, tracker, AlertKind::sleep, L"Enemy fell asleep", torpor_ratio * 100.0F);
                if (settings.alert_death && dead && !tracker.dead)
                    emit_actor(actor, tracker, AlertKind::death, L"Enemy died");
            }
            if (settings.alert_noglin && noglin_inside && !tracker.noglin_inside)
                emit_actor(actor, tracker, AlertKind::noglin, L"Noglin in range");
            if (settings.alert_turret && turret_threat && !tracker.turret_threat)
                emit_actor(actor, tracker, AlertKind::turret, L"Turret targeting threat");
            tracker.sleeping = actor.sleeping;
            tracker.knocked_out = knocked_out;
            tracker.dead = dead;
            tracker.noglin_inside = noglin_inside;
            tracker.turret_threat = turret_threat;
        }

        if (settings.alert_enemy_group && ready && enemy_group_count >= 3 && previous_enemy_group_count_ < 3 &&
            (last_group_alert_.time_since_epoch().count() == 0 ||
                std::chrono::duration<float>(now - last_group_alert_).count() >= settings.alert_cooldown_s))
        {
            last_group_alert_ = now;
            append_alert({0, AlertKind::enemy_group, L"Enemy group nearby",
                std::to_wstring(enemy_group_count) + L" players", L"", 0.0F,
                static_cast<float>(enemy_group_count), 0.0F});
        }
        previous_enemy_group_count_ = enemy_group_count;

        for (auto iterator = alert_trackers_.begin(); iterator != alert_trackers_.end();)
        {
            if (!seen.contains(iterator->first) && iterator->second.last_seen.time_since_epoch().count() != 0 &&
                std::chrono::duration<float>(now - iterator->second.last_seen).count() > 5.0F)
                iterator = alert_trackers_.erase(iterator);
            else ++iterator;
        }

        snapshot_.alerts.clear();
        snapshot_.alerts.reserve(active_alerts_.size());
        for (ActiveAlert& active : active_alerts_)
        {
            active.value.remaining_s = std::max(0.0F, std::chrono::duration<float>(active.expires - now).count());
            snapshot_.alerts.push_back(active.value);
        }
        // SystemAsterisk is the Windows informational cue, not the error/critical
        // MessageBeep sound previously used by radius alerts.
        if (play_sound && settings.alert_sound)
            PlaySoundW(L"SystemAsterisk", nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
    }

    bool ArkRuntime::read_actor(const std::uintptr_t address, Actor& actor)
    {
        std::uintptr_t class_address{};
        std::uintptr_t root{};
        if (!read(address + offsets_.object_class, class_address) ||
            !read(address + offsets_.actor_root_component, root) || class_address < 0x10000 || root < 0x10000)
        {
            return false;
        }
        const ClassMeta metadata = class_meta(class_address);
        if (metadata.kind == ActorKind::other) return false;
        Vec3 position{};
        if (!read_vec3(root + offsets_.component_to_world + offsets_.transform_translation, position)) return false;

        actor.address = address;
        actor.root_component = root;
        actor.kind = metadata.kind;
        actor.position = position;
        actor.bounds_valid = read_component_bounds(root, actor.bounds_origin, actor.bounds_extent) &&
            distance(actor.bounds_origin, position) <=
                std::sqrt(actor.bounds_extent.x * actor.bounds_extent.x +
                    actor.bounds_extent.y * actor.bounds_extent.y +
                    actor.bounds_extent.z * actor.bounds_extent.z) * 4.0F + 5000.0F;
        actor.turret = metadata.turret;
        actor.class_name = metadata.name;
        actor.name = friendly_name(metadata.name);
        read(address + offsets_.actor_last_render_time, actor.last_render_time);
        if (!std::isfinite(actor.last_render_time) || actor.last_render_time < 0.0)
            actor.last_render_time = 0.0;
        if (actor.kind == ActorKind::player)
            read(address + offsets_.linked_player_data_id, actor.linked_player_data_id);
        read(address + offsets_.targeting_team, actor.team);
        std::uintptr_t status{};
        if (read(address + offsets_.status_component, status) && status >= 0x10000)
        {
            read(status + offsets_.status_current_values, actor.health);
            read(status + offsets_.status_max_values, actor.max_health);
            read(status + offsets_.status_current_values + 2 * sizeof(float), actor.torpor);
            read(status + offsets_.status_max_values + 2 * sizeof(float), actor.max_torpor);
            const auto valid_pair = [](float& current, float& maximum) {
                if (!std::isfinite(current) || !std::isfinite(maximum) || maximum <= 0.0F ||
                    maximum > 100'000'000.0F || current < -1.0F || current > maximum * 2.0F)
                {
                    current = maximum = 0.0F;
                    return;
                }
                current = std::clamp(current, 0.0F, maximum);
            };
            valid_pair(actor.health, actor.max_health);
            valid_pair(actor.torpor, actor.max_torpor);
        }
        if (actor.max_health <= 0.0F)
        {
            read(address + offsets_.current_health, actor.health);
            read(address + offsets_.max_health, actor.max_health);
        }
        std::uint8_t state{};
        if (read(address + offsets_.is_dead, state)) actor.dead = (state & 0x20) != 0;
        if (read(address + offsets_.is_sleeping, state)) actor.sleeping = (state & 0x01) != 0;

        std::wstring display;
        if (actor.kind == ActorKind::player)
        {
            display = read_fstring(address + offsets_.player_name);
            actor.tribe = read_fstring(address + offsets_.tribe_name);
        }
        else if (actor.kind == ActorKind::structure)
        {
            display = read_fstring(address + offsets_.structure_name);
            // PDB profile: APrimalStructure::OwnerName. This is the replicated tribe/owner
            // label used by structures and lets the alliance page resolve a name even when
            // no player from that team is inside the current actor snapshot.
            actor.tribe = read_fstring(address + 0x8A0);
            if (actor.tribe.empty()) actor.tribe = read_fstring(address + 0x7A8);
        }
        else if (actor.kind == ActorKind::dino)
        {
            display = read_fstring(address + offsets_.descriptive_name);
            actor.tribe = read_fstring(address + offsets_.tribe_name);
        }
        if (!display.empty()) actor.name = display;
        if (actor.turret)
        {
            std::int32_t ammo{};
            if (read(address + 0xEB0, ammo) && ammo >= 0 && ammo <= 10'000'000) actor.turret_ammo = ammo;
            std::uint32_t container_flags{};
            std::uint8_t power_flags{}, turret_flags{};
            read(address + 0xAE8, container_flags);
            read(address + 0xB6D, power_flags);
            read(address + 0xEA8, turret_flags);
            read(address + 0xEAA, actor.turret_range);
            read(address + 0xEAB, actor.turret_targeting);
            read(address + 0xEAC, actor.turret_warning);
            actor.turret_powered = (power_flags & (1U << 5)) != 0;
            actor.turret_active = (container_flags & (1U << 6)) != 0;
            actor.turret_targeting_actor = (turret_flags & (1U << 6)) != 0;
        }
        if (actor.kind == ActorKind::drop)
        {
            std::uintptr_t item{};
            if (read(address + 0x620, item) && item >= 0x10000)
            {
                const std::wstring item_name = read_fstring(item + 0x328);
                if (!item_name.empty()) actor.name = item_name;
                std::int32_t quantity{};
                if (read(item + 0x5F8, quantity) && quantity > 0 && quantity <= 1'000'000'000)
                    actor.quantity = quantity;
            }
        }
        if (actor.kind == ActorKind::player && need_player_bones_)
            read_player_bones(address, actor.position, actor);
        if (actor.kind == ActorKind::player && (need_equipment_ || need_held_items_))
            read_player_equipment(address, actor);
        return true;
    }

    bool ArkRuntime::read_player_bones(const std::uintptr_t address, const Vec3& actor_position, Actor& actor)
    {
        static constexpr std::array<std::int32_t, 23> selected{
            1, 2, 3, 4, 5, 6, 8, 32, 33, 36, 38, 58, 59, 62, 64, 82, 83, 84, 86, 88, 89, 90, 92};
        std::uintptr_t mesh{};
        std::uintptr_t transform_data{};
        std::int32_t transform_count{};
        NativeTransform component_to_world{};
        if (!read(address + offsets_.character_mesh, mesh) || mesh < 0x10000 ||
            !read(mesh + offsets_.mesh_space_bases, transform_data) ||
            !read(mesh + offsets_.mesh_space_bases + 8, transform_count) ||
            transform_data < 0x10000 || transform_count < 94 || transform_count > 256 ||
            !read(mesh + offsets_.component_to_world, component_to_world) || !component_to_world.valid())
            return false;

        std::array<NativeTransform, 94> transforms{};
        SIZE_T bytes{};
        if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(transform_data),
            transforms.data(), sizeof(transforms), &bytes) == FALSE || bytes != sizeof(transforms)) return false;
        for (std::size_t index = 0; index < selected.size(); ++index)
        {
            const NativeTransform& transform = transforms[static_cast<std::size_t>(selected[index])];
            if (!transform.valid())
            {
                actor.bone_count = 0;
                return false;
            }
            const Vec3 world = component_to_world.transform_position(
                {transform.translation_x, transform.translation_y, transform.translation_z});
            if (!std::isfinite(world.x) || !std::isfinite(world.y) || !std::isfinite(world.z) ||
                distance(world, actor_position) > 2000.0F)
            {
                actor.bone_count = 0;
                return false;
            }
            actor.bones[index] = world;
        }
        actor.bone_count = static_cast<std::int32_t>(selected.size());
        return true;
    }

    float ArkRuntime::read_item_stat(const std::uintptr_t item, const int stat_index) const
    {
        constexpr std::uintptr_t stat_info = 0x438;
        constexpr std::uintptr_t stat_values = 0x558;
        constexpr std::uintptr_t stat_stride = 0x24;
        const auto info = item + stat_info + static_cast<std::uintptr_t>(stat_index) * stat_stride;
        std::uint32_t flags{};
        float range_multiplier{}, state_scale{}, initial{}, absolute_maximum{};
        std::uint16_t raw{};
        if (!read(info, flags) || !read(info + 0x0C, range_multiplier) ||
            !read(info + 0x14, state_scale) || !read(info + 0x18, initial) ||
            !read(info + 0x20, absolute_maximum) ||
            !read(item + stat_values + static_cast<std::uintptr_t>(stat_index) * 2, raw)) return 0.0F;
        const float baseline = ((flags & 0x6U) != 0 ? 1.0F : 0.0F) + initial;
        if (!std::isfinite(range_multiplier) || !std::isfinite(state_scale) ||
            !std::isfinite(baseline) || baseline <= 0.0F) return 0.0F;
        float result = baseline + baseline * range_multiplier * state_scale * static_cast<float>(raw);
        if (std::isfinite(absolute_maximum) && absolute_maximum > 0.0F) result = std::min(result, absolute_maximum);
        return std::isfinite(result) && result > 0.0F ? result : 0.0F;
    }

    void ArkRuntime::read_player_equipment(const std::uintptr_t address, Actor& actor)
    {
        actor.held_item.clear();
        actor.armor_types.fill(0);
        actor.armor_ratios.fill(0.0F);
        if (need_held_items_)
        {
            std::uintptr_t weapon{}, weapon_class{};
            if (read(address + 0x1708, weapon) && weapon >= 0x10000 &&
                read(weapon + offsets_.object_class, weapon_class) && weapon_class >= 0x10000)
                actor.held_item = friendly_name(object_name(weapon_class));
        }
        if (!need_equipment_) return;

        std::uintptr_t inventory{}, items{};
        std::int32_t count{};
        if (!read(address + 0xCE0, inventory) || inventory < 0x10000 ||
            !read(inventory + 0x128, items) || !read(inventory + 0x130, count) ||
            items < 0x10000 || count <= 0 || count > 64) return;
        for (int index = 0; index < count; ++index)
        {
            std::uintptr_t item{};
            if (!read(items + static_cast<std::uintptr_t>(index) * sizeof(std::uintptr_t), item) || item < 0x10000) continue;
            const float maximum = read_item_stat(item, 2);
            float durability{};
            if (maximum <= 0.0F || !read(item + 0x57C, durability) || !std::isfinite(durability)) continue;
            std::wstring item_name = read_fstring(item + 0x328);
            if (item_name.empty())
            {
                std::uintptr_t item_class{};
                if (read(item + offsets_.object_class, item_class)) item_name = object_name(item_class);
            }
            const std::wstring name = lower(item_name);
            int part = name.find(L"helmet") != std::wstring::npos || name.find(L"head") != std::wstring::npos ||
                name.find(L"hat") != std::wstring::npos ? 0 :
                name.find(L"shirt") != std::wstring::npos || name.find(L"chest") != std::wstring::npos ||
                name.find(L"vest") != std::wstring::npos ? 1 :
                name.find(L"glove") != std::wstring::npos || name.find(L"gauntlet") != std::wstring::npos ||
                name.find(L"hand") != std::wstring::npos ? 2 :
                name.find(L"pant") != std::wstring::npos || name.find(L"legging") != std::wstring::npos ? 3 :
                name.find(L"boot") != std::wstring::npos || name.find(L"feet") != std::wstring::npos ? 4 : -1;
            if (part < 0) continue;
            int tier = 11;
            if (name.find(L"federation exo") != std::wstring::npos || name.find(L"gen24") != std::wstring::npos) tier = 12;
            else if (name.find(L"flak") != std::wstring::npos) tier = 4;
            else if (name.find(L"riot") != std::wstring::npos) tier = 5;
            else if (name.find(L"tek") != std::wstring::npos) tier = 6;
            actor.armor_types[static_cast<std::size_t>(part)] = tier;
            actor.armor_ratios[static_cast<std::size_t>(part)] = std::clamp(durability, 0.0F, maximum) / maximum;
        }
    }

    ArkRuntime::ClassMeta ArkRuntime::class_meta(const std::uintptr_t class_address)
    {
        const auto cached = class_cache_.find(class_address);
        if (cached != class_cache_.end()) return cached->second;

        ClassMeta result;
        result.name = object_name(class_address);
        std::uintptr_t current = class_address;
        std::unordered_set<std::uintptr_t> visited;
        for (int depth = 0; depth < 64 && current >= 0x10000 && visited.insert(current).second; ++depth)
        {
            const std::wstring name = lower(object_name(current));
            if (name.find(L"primalstructureturret") != std::wstring::npos) result.turret = true;
            if (name.find(L"primaldinocharacter") != std::wstring::npos)
            {
                result.kind = ActorKind::dino;
                break;
            }
            if (name.find(L"primalstructure") != std::wstring::npos)
            {
                result.kind = ActorKind::structure;
                break;
            }
            if (name.find(L"shootercharacter") != std::wstring::npos)
            {
                result.kind = ActorKind::player;
                break;
            }
            if (!read(current + offsets_.super_struct, current)) break;
        }

        const std::wstring class_lower = lower(result.name);
        if (class_lower.find(L"deathitemcache") != std::wstring::npos ||
            class_lower.find(L"deathcache") != std::wstring::npos)
            result.kind = ActorKind::death_cache;
        else if (class_lower.find(L"droppeditem") != std::wstring::npos ||
            class_lower.find(L"supplycrate") != std::wstring::npos)
            result.kind = ActorKind::drop;

        class_cache_.emplace(class_address, result);
        return result;
    }

    std::wstring ArkRuntime::object_name(const std::uintptr_t object_address)
    {
        std::int32_t index{};
        std::int32_t number{};
        if (!read(object_address + offsets_.object_name, index) ||
            !read(object_address + offsets_.object_name + 4, number)) return {};
        return resolve_name(index, number);
    }

    std::wstring ArkRuntime::resolve_name(const std::int32_t index, const std::int32_t number)
    {
        if (index < 0 || index > 100'000'000 || g_names_ < 0x10000) return {};
        std::wstring base;
        const auto cached = name_cache_.find(index);
        if (cached != name_cache_.end())
        {
            base = cached->second;
        }
        else
        {
            constexpr std::int32_t entries_per_chunk = 16384;
            std::uintptr_t chunk{};
            std::uintptr_t entry{};
            if (!read(g_names_ + static_cast<std::uintptr_t>(index / entries_per_chunk) * 8, chunk) ||
                !read(chunk + static_cast<std::uintptr_t>(index % entries_per_chunk) * 8, entry)) return {};
            std::int32_t header{};
            if (!read(entry, header)) return {};
            const bool wide = (header & std::numeric_limits<std::int32_t>::min()) != 0;
            if (wide)
            {
                std::array<wchar_t, 1024> value{};
                SIZE_T bytes{};
                if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(entry + 16),
                        value.data(), value.size() * sizeof(wchar_t), &bytes) == FALSE) return {};
                value.back() = L'\0';
                base.assign(value.data());
            }
            else
            {
                std::array<char, 1024> value{};
                SIZE_T bytes{};
                if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(entry + 16),
                        value.data(), value.size(), &bytes) == FALSE) return {};
                value.back() = '\0';
                const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), -1, nullptr, 0);
                if (length <= 1) return {};
                std::vector<wchar_t> wide_value(static_cast<std::size_t>(length));
                MultiByteToWideChar(CP_UTF8, 0, value.data(), -1, wide_value.data(), length);
                base.assign(wide_value.data());
            }
            if (base.empty()) return {};
            name_cache_.emplace(index, base);
        }
        if (number > 0) base += L"_" + std::to_wstring(number - 1);
        return base;
    }

    std::wstring ArkRuntime::read_fstring(const std::uintptr_t address, const std::size_t cap)
    {
        std::uintptr_t data{};
        std::int32_t count{};
        if (!read(address, data) || !read(address + 8, count) || count <= 0 ||
            static_cast<std::size_t>(count) > cap) return {};
        std::vector<wchar_t> value(static_cast<std::size_t>(count) + 1);
        SIZE_T bytes{};
        if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(data), value.data(),
                static_cast<std::size_t>(count) * sizeof(wchar_t), &bytes) == FALSE) return {};
        value.back() = L'\0';
        if (count > 0) value[static_cast<std::size_t>(count)] = L'\0';
        std::wstring result(value.data());
        result.erase(std::remove_if(result.begin(), result.end(),
            [](const wchar_t c) { return std::iswcntrl(c) != 0; }), result.end());
        if (result.size() > 96) result.resize(96);
        return result;
    }

    bool ArkRuntime::read_vec3(const std::uintptr_t address, Vec3& value) const
    {
        if (!read(address, value)) return false;
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
            std::abs(value.x) <= max_coordinate && std::abs(value.y) <= max_coordinate &&
            std::abs(value.z) <= max_coordinate;
    }

    bool ArkRuntime::read_component_bounds(const std::uintptr_t component, Vec3& origin, Vec3& extent) const
    {
        if (component < 0x10000 ||
            !read_vec3(component + offsets_.component_bounds, origin) ||
            !read_vec3(component + offsets_.component_bounds + sizeof(Vec3), extent)) return false;
        return extent.x > 0.0F && extent.y > 0.0F && extent.z > 0.0F &&
            extent.x < 2'000'000.0F && extent.y < 2'000'000.0F && extent.z < 2'000'000.0F;
    }

    bool ArkRuntime::engine_object_live(const std::uintptr_t address) const
    {
        std::uintptr_t vtable{};
        std::uint32_t object_flags{};
        return address >= 0x10000 && read(address, vtable) &&
            vtable >= module_base_ && vtable < module_base_ + module_size_ &&
            read(address + 0x8, object_flags) && (object_flags & 0x00018000U) == 0;
    }

    bool ArkRuntime::engine_actor_live(const Actor& actor) const
    {
        std::uintptr_t root{};
        return engine_object_live(actor.address) &&
            read(actor.address + offsets_.actor_root_component, root) &&
            root == actor.root_component && engine_object_live(root);
    }

    bool ArkRuntime::world_to_screen(const Vec3& world, const float width, const float height, Vec2& screen) const
    {
        if (!snapshot_.camera.valid || width <= 0.0F || height <= 0.0F) return false;
        const float pitch = snapshot_.camera.rotation.x / radians_to_degrees;
        const float yaw = snapshot_.camera.rotation.y / radians_to_degrees;
        const float roll = snapshot_.camera.rotation.z / radians_to_degrees;
        const float sp = std::sin(pitch), cp = std::cos(pitch);
        const float sy = std::sin(yaw), cy = std::cos(yaw);
        const float sr = std::sin(roll), cr = std::cos(roll);
        const Vec3 forward{cp * cy, cp * sy, sp};
        const Vec3 right{sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp};
        const Vec3 up{-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp};
        const Vec3 delta{world.x - snapshot_.camera.location.x,
            world.y - snapshot_.camera.location.y, world.z - snapshot_.camera.location.z};
        const auto dot = [](const Vec3& a, const Vec3& b) { return a.x*b.x + a.y*b.y + a.z*b.z; };
        const float depth = dot(delta, forward);
        if (depth <= 1.0F) return false;
        const float focal = (width * 0.5F) /
            std::tan(snapshot_.camera.fov * std::numbers::pi_v<float> / 360.0F);
        screen.x = width * 0.5F + dot(delta, right) * focal / depth;
        screen.y = height * 0.5F - dot(delta, up) * focal / depth;
        return std::isfinite(screen.x) && std::isfinite(screen.y);
    }

    void ArkRuntime::run_aim(Settings& settings, const float delta_seconds)
    {
        snapshot_.aim_debug = {};
        snapshot_.aim_debug.world_time = snapshot_.world_time;
        snapshot_.aim_debug.prediction = settings.aim_prediction;
        snapshot_.aim_debug.intercept_solver = settings.aim_intercept_solver;
        snapshot_.aim_active = false;
        snapshot_.player_aim_active = false;
        snapshot_.dino_aim_active = false;
        snapshot_.aim_target = 0;
        const bool player_aim_enable_changed = last_player_aim_ != settings.player_aim;
        const bool dino_aim_enable_changed = last_dino_aim_ != settings.dino_aim;
        const bool menu_just_closed = last_menu_open_ && !settings.menu_open;
        if (last_aim_key_ != settings.aim_key || last_aim_activation_mode_ != settings.aim_activation_mode ||
            player_aim_enable_changed || menu_just_closed)
        {
            last_aim_key_ = settings.aim_key;
            last_aim_activation_mode_ = settings.aim_activation_mode;
            aim_key_was_down_ = false;
            aim_key_armed_ = false;
            aim_toggle_active_ = false;
            locked_target_ = 0;
            locked_target_occluded_seconds_ = 0.0F;
        }
        if (last_dino_aim_key_ != settings.dino_aim_key ||
            last_dino_aim_activation_mode_ != settings.dino_aim_activation_mode ||
            dino_aim_enable_changed || menu_just_closed)
        {
            last_dino_aim_key_ = settings.dino_aim_key;
            last_dino_aim_activation_mode_ = settings.dino_aim_activation_mode;
            dino_aim_key_was_down_ = false;
            dino_aim_key_armed_ = false;
            dino_aim_toggle_active_ = false;
            locked_target_ = 0;
            locked_target_occluded_seconds_ = 0.0F;
        }
        last_player_aim_ = settings.player_aim;
        last_dino_aim_ = settings.dino_aim;
        last_menu_open_ = settings.menu_open;
        DWORD foreground_process{};
        const HWND foreground_window = GetForegroundWindow();
        if (foreground_window == nullptr ||
            GetWindowThreadProcessId(foreground_window, &foreground_process) == 0 ||
            foreground_process != GetCurrentProcessId())
        {
            aim_key_was_down_ = false;
            aim_key_armed_ = false;
            aim_toggle_active_ = false;
            dino_aim_key_was_down_ = false;
            dino_aim_key_armed_ = false;
            dino_aim_toggle_active_ = false;
            locked_target_ = 0;
            locked_target_occluded_seconds_ = 0.0F;
            snapshot_.aim_armed = false;
            return;
        }
        const auto activation = [](const bool enabled, const std::uint32_t key, const std::int32_t mode,
            bool& was_down, bool& armed, bool& toggle_active) {
            if (!enabled) return false;
            const bool down = (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
            if (!down) armed = true;
            const bool pressed = down && !was_down && armed;
            was_down = down;
            if (mode == 1 && pressed) toggle_active = !toggle_active;
            return mode == 0 ? down && armed : mode == 1 ? toggle_active : true;
        };
        const bool player_activated = activation(settings.player_aim, settings.aim_key,
            settings.aim_activation_mode, aim_key_was_down_, aim_key_armed_, aim_toggle_active_);
        const bool dino_activated = activation(settings.dino_aim, settings.dino_aim_key,
            settings.dino_aim_activation_mode, dino_aim_key_was_down_, dino_aim_key_armed_,
            dino_aim_toggle_active_);
        snapshot_.player_aim_active = player_activated;
        snapshot_.dino_aim_active = dino_activated;
        snapshot_.aim_armed = (settings.player_aim && aim_key_armed_) ||
            (settings.dino_aim && dino_aim_key_armed_);
        if ((!settings.player_aim && !settings.dino_aim) || settings.menu_open || !snapshot_.local_valid ||
            !snapshot_.camera.valid || (!player_activated && !dino_activated))
        {
            locked_target_ = 0;
            locked_target_occluded_seconds_ = 0.0F;
            if ((!settings.player_aim && !settings.dino_aim) || settings.menu_open || !snapshot_.local_valid)
            {
                aim_toggle_active_ = false;
                dino_aim_toggle_active_ = false;
                if (settings.menu_open)
                {
                    if ((GetAsyncKeyState(static_cast<int>(settings.aim_key)) & 0x8000) != 0)
                        aim_key_armed_ = false;
                    if ((GetAsyncKeyState(static_cast<int>(settings.dino_aim_key)) & 0x8000) != 0)
                        dino_aim_key_armed_ = false;
                }
            }
            return;
        }
        snapshot_.aim_active = true;
        snapshot_.aim_debug.active = true;

        const auto target_point = [&](const Actor& actor, const bool apply_prediction = true) {
            Vec3 point = actor.position;
            bool used_bone{};
            if (actor.kind == ActorKind::player && actor.bone_count > 0)
            {
                struct HitPoint
                {
                    int slot{};
                    int armor_part{};
                };
                // Compact player skeleton points:
                // 6 skull centre, 3 chest centre, 9/13 arms, 16/20 lower legs,
                // 18/22 feet. Paired limbs are one category in Minimal mode.
                static constexpr std::array<HitPoint, 8> points{{
                    {6, 0}, {3, 1}, {9, 2}, {13, 2},
                    {16, 3}, {20, 3}, {18, 4}, {22, 4}}};
                std::array<int, 8> candidates{};
                std::size_t candidate_count{};
                const auto add_point = [&](const int index) {
                    const int slot = points[static_cast<std::size_t>(index)].slot;
                    if (slot >= 0 && slot < actor.bone_count &&
                        distance(actor.bones[static_cast<std::size_t>(slot)], actor.position) < 300.0F)
                        candidates[candidate_count++] = index;
                };
                if (settings.aim_hitbox_mode == 0)
                {
                    switch (std::clamp(settings.aim_hitbox, 0, 4))
                    {
                    case 0: add_point(0); break;
                    case 1: add_point(1); break;
                    case 2: add_point(2); add_point(3); break;
                    case 3: add_point(4); add_point(5); break;
                    case 4: add_point(6); add_point(7); break;
                    default: break;
                    }
                }
                else
                {
                    for (int index = 0; index < static_cast<int>(points.size()); ++index)
                        if ((settings.aim_hitbox_mask & (1U << index)) != 0) add_point(index);
                }
                const auto point_angle = [&](const Vec3& candidate) {
                    const Vec3 delta{candidate.x - snapshot_.camera.location.x,
                        candidate.y - snapshot_.camera.location.y, candidate.z - snapshot_.camera.location.z};
                    const float flat = std::sqrt(delta.x * delta.x + delta.y * delta.y);
                    const float yaw = std::atan2(delta.y, delta.x) * radians_to_degrees;
                    const float pitch = std::atan2(delta.z, flat) * radians_to_degrees;
                    const float yaw_delta = normalize_angle(yaw - snapshot_.camera.rotation.y);
                    const float pitch_delta = normalize_angle(pitch - snapshot_.camera.rotation.x);
                    return std::sqrt(yaw_delta * yaw_delta + pitch_delta * pitch_delta);
                };
                float best_score = std::numeric_limits<float>::max();
                for (std::size_t candidate = 0; candidate < candidate_count; ++candidate)
                {
                    const HitPoint selected = points[static_cast<std::size_t>(candidates[candidate])];
                    const Vec3 candidate_point = actor.bones[static_cast<std::size_t>(selected.slot)];
                    const float angle = point_angle(candidate_point);
                    float score = angle;
                    if (settings.aim_hitbox_mode == 1)
                    {
                        const float durability = std::clamp(
                            actor.armor_ratios[static_cast<std::size_t>(selected.armor_part)], 0.0F, 1.0F);
                        if (settings.aim_point_method == 0)
                            score = angle + durability * 6.0F;
                        else if (settings.aim_point_method == 1)
                            score = durability * 100.0F + angle * 0.01F;
                    }
                    if (score < best_score)
                    {
                        best_score = score;
                        point = candidate_point;
                        used_bone = true;
                    }
                }
            }
            else if (actor.kind == ActorKind::dino && actor.bounds_valid)
            {
                // USceneComponent::Bounds is an already-computed POD snapshot. Reading it
                // avoids AActor::GetActorBounds' virtual dispatch through a component that
                // may have entered destruction between actor discovery and this camera tick.
                point = actor.bounds_origin;
                used_bone = true;
            }
            if (!used_bone)
            {
                // Last-resort category fallback while the compact mesh cache refreshes.
                // It is never used when an exact requested bone is available.
                static constexpr std::array<float, 5> player_heights{78.0F, 38.0F, 48.0F, 18.0F, 4.0F};
                int category = std::clamp(settings.aim_hitbox, 0, 4);
                if (settings.aim_hitbox_mode == 1)
                {
                    for (int index = 0; index < 8; ++index)
                    {
                        if ((settings.aim_hitbox_mask & (1U << index)) == 0) continue;
                        category = index == 0 ? 0 : index == 1 ? 1 : index <= 3 ? 2 : index <= 5 ? 3 : 4;
                        break;
                    }
                }
                point.z += actor.kind == ActorKind::player ? player_heights[category] : 80.0F;
            }
            if (settings.aim_prediction && apply_prediction)
            {
                const float projectile_speed = settings.projectile_velocity_mps * 100.0F;
                const Vec3 relative{point.x - snapshot_.camera.location.x,
                    point.y - snapshot_.camera.location.y, point.z - snapshot_.camera.location.z};
                float flight_seconds = distance(point, snapshot_.camera.location) / projectile_speed;
                if (settings.aim_intercept_solver)
                    flight_seconds = intercept_time(relative, actor.velocity, projectile_speed);
                const float travel_seconds = std::clamp(flight_seconds +
                    settings.prediction_latency_ms * 0.001F, 0.0F, 3.0F);
                point.x += actor.velocity.x * travel_seconds;
                point.y += actor.velocity.y * travel_seconds;
                point.z += actor.velocity.z * travel_seconds + 0.5F *
                    settings.projectile_gravity_mps2 * 100.0F * travel_seconds * travel_seconds;
            }
            return point;
        };
        const auto eligible = [&](const Actor& actor) {
            if (actor_is_dead(actor) || actor.stale_seconds > 0.0F ||
                (actor.kind == ActorKind::player && !player_activated) ||
                (actor.kind == ActorKind::dino && !dino_activated) ||
                (actor.kind != ActorKind::player && actor.kind != ActorKind::dino)) return false;
            const bool allied = (snapshot_.local_team != 0 && actor.team == snapshot_.local_team) ||
                settings.is_allied(actor.team);
            return !((allied && !settings.aim_target_allies) || (!allied && !settings.aim_target_enemies)) &&
                distance(actor.position, snapshot_.camera.location) <= settings.aim_distance_m * 100.0F;
        };
        const auto angular_distance = [&](const Vec3& point) {
            const Vec3 delta{point.x - snapshot_.camera.location.x,
                point.y - snapshot_.camera.location.y, point.z - snapshot_.camera.location.z};
            const float flat = std::sqrt(delta.x * delta.x + delta.y * delta.y);
            const float desired_yaw = std::atan2(delta.y, delta.x) * radians_to_degrees;
            const float desired_pitch = std::atan2(delta.z, flat) * radians_to_degrees;
            const float yaw_delta = normalize_angle(desired_yaw - snapshot_.camera.rotation.y);
            const float pitch_delta = normalize_angle(desired_pitch - snapshot_.camera.rotation.x);
            return std::sqrt(yaw_delta * yaw_delta + pitch_delta * pitch_delta);
        };

        // Exact ShooterGame 358.26 PDB profile:
        // AController::LineOfSightTo(AActor const*, FVector, bool) const.
        constexpr std::uintptr_t line_of_sight_rva = 0x29077D0;
        static constexpr std::array<std::uint8_t, 6> line_of_sight_prologue{
            0x40, 0x55, 0x56, 0x57, 0x41, 0x56};
        using LineOfSightFn = bool(__fastcall*)(const void*, const void*, Vec3, bool);
        LineOfSightFn line_of_sight{};
        if (settings.visibility_check)
        {
            if (std::memcmp(reinterpret_cast<const void*>(module_base_ + line_of_sight_rva),
                line_of_sight_prologue.data(), line_of_sight_prologue.size()) != 0)
            {
                locked_target_ = 0;
                locked_target_occluded_seconds_ = 0.0F;
                status_ = L"Aim disabled: LineOfSightTo symbol mismatch";
                return;
            }
            line_of_sight = reinterpret_cast<LineOfSightFn>(module_base_ + line_of_sight_rva);
        }
        const auto visible = [&](const Actor& actor, const Vec3& selected_point) {
            // UE's ViewPoint argument is the observer/camera origin. Passing the
            // target point here makes the trace originate at the target and can
            // incorrectly report actors behind geometry as visible.
            if (line_of_sight == nullptr) return true;
            if (!engine_object_live(snapshot_.local_controller) || !engine_actor_live(actor)) return false;
            if (!std::isfinite(selected_point.x) || !std::isfinite(selected_point.y) ||
                !std::isfinite(selected_point.z) || snapshot_.world_time <= 0.0 ||
                actor.last_render_time <= 0.0) return false;
            const double render_age = snapshot_.world_time - actor.last_render_time;
            if (!std::isfinite(render_age) || render_age < 0.0 || render_age > 0.20) return false;
            return line_of_sight(
                reinterpret_cast<const void*>(snapshot_.local_controller),
                reinterpret_cast<const void*>(actor.address), snapshot_.camera.location, false);
        };

        Actor* best{};
        Vec3 best_point{};
        bool best_point_valid{};
        if (settings.aim_lock && locked_target_ != 0)
        {
            const auto found = std::find_if(snapshot_.actors.begin(), snapshot_.actors.end(), [&](const Actor& actor) {
                return actor.address == locked_target_ && eligible(actor);
            });
            if (found != snapshot_.actors.end())
            {
                // The general snapshot runs at a bounded cadence for performance,
                // but the currently locked player's mesh is cheap enough to sample
                // on every camera tick. This prevents visible trailing on sprinting
                // targets without enabling projectile prediction or aiming ahead.
                if (found->kind == ActorKind::player)
                    read_player_bones(found->address, found->position, *found);
                const Vec3 point = target_point(*found);
                if (visible(*found, point))
                {
                    locked_target_occluded_seconds_ = 0.0F;
                    best = &*found;
                    best_point = point;
                    best_point_valid = true;
                }
                else
                {
                    // Fail closed immediately. Retaining an occluded target, even
                    // without rotating for a frame, made wall transitions feel like
                    // the aim was still tracking through geometry.
                    locked_target_ = 0;
                    locked_target_occluded_seconds_ = 0.0F;
                }
            }
            else
            {
                locked_target_ = 0;
                locked_target_occluded_seconds_ = 0.0F;
            }
        }
        if (best == nullptr)
        {
            const float effective_fov = snapshot_.local_mounted ? settings.mounted_aim_fov : settings.aim_fov;
            float best_score = std::numeric_limits<float>::max();
            for (Actor& actor : snapshot_.actors)
            {
                if (!eligible(actor)) continue;
                const Vec3 point = target_point(actor);
                const float angle = angular_distance(point);
                if (angle > effective_fov) continue;
                if (!visible(actor, point)) continue;
                const float target_distance_m = distance(actor.position, snapshot_.camera.location) / 100.0F;
                const float health_ratio = actor.max_health > 0.0F ?
                    std::clamp(actor.health / actor.max_health, 0.0F, 1.0F) : 1.0F;
                float score = angle;
                if (settings.aim_priority == 1) score = target_distance_m;
                else if (settings.aim_priority == 2) score = health_ratio;
                else if (settings.aim_priority == 3)
                    score = angle / std::max(1.0F, effective_fov) * 0.65F +
                        target_distance_m / std::max(1.0F, settings.aim_distance_m) * 0.25F +
                        health_ratio * 0.10F;
                if (score < best_score)
                {
                    best_score = score;
                    best = &actor;
                    best_point = point;
                    best_point_valid = true;
                }
            }
            if (best != nullptr && settings.aim_lock)
            {
                locked_target_ = best->address;
                locked_target_occluded_seconds_ = 0.0F;
            }
        }
        if (best == nullptr || !best_point_valid) return;
        if (best->kind == ActorKind::player &&
            read_player_bones(best->address, best->position, *best))
            best_point = target_point(*best);
        snapshot_.aim_target = best->address;

        // Use the exact point that was scored and visibility-tested. Recomputing it
        // here made random zones and moving targets appear to lead even with prediction off.
        const Vec3 point = best_point;
        const Vec3 delta{point.x - snapshot_.camera.location.x,
            point.y - snapshot_.camera.location.y, point.z - snapshot_.camera.location.z};
        const float flat = std::sqrt(delta.x * delta.x + delta.y * delta.y);
        Vec3 rotation = snapshot_.camera.rotation;
        const float desired_yaw = std::atan2(delta.y, delta.x) * radians_to_degrees;
        const float desired_pitch = std::atan2(delta.z, flat) * radians_to_degrees;
        const float smoothing = snapshot_.local_mounted ? settings.mounted_aim_smoothing : settings.aim_smoothing;
        const float base_response = smoothing <= 1.0F ? 1.0F :
            1.0F - std::exp(std::log1p(-1.0F / smoothing) *
                (std::clamp(delta_seconds, 0.001F, 0.10F) * 60.0F));
        const float pitch_error = normalize_angle(desired_pitch - rotation.x);
        const float yaw_error = normalize_angle(desired_yaw - rotation.y);
        const float angular_error = std::sqrt(pitch_error * pitch_error + yaw_error * yaw_error);
        const float active_fov = snapshot_.local_mounted ? settings.mounted_aim_fov : settings.aim_fov;
        const float normalized_error = std::clamp(angular_error / std::max(1.0F, active_fov), 0.0F, 1.0F);
        const float response_scale = 1.0F + settings.aim_angle_boost * normalized_error;
        const float response = base_response >= 1.0F ? 1.0F :
            1.0F - std::pow(1.0F - base_response, response_scale);
        const Vec3 raw_bone = target_point(*best, false);
        AimTelemetry& aim_debug = snapshot_.aim_debug;
        aim_debug.target_valid = true;
        aim_debug.target_locked = locked_target_ == best->address;
        aim_debug.visible = true;
        aim_debug.target = best->address;
        aim_debug.target_team = best->team;
        if (!best->name.empty())
            wcsncpy_s(aim_debug.target_name.data(), aim_debug.target_name.size(), best->name.c_str(), _TRUNCATE);
        aim_debug.camera = snapshot_.camera.location;
        aim_debug.raw_bone = raw_bone;
        aim_debug.final_point = point;
        aim_debug.velocity = best->velocity;
        aim_debug.distance_m = distance(point, snapshot_.camera.location) / 100.0F;
        aim_debug.angular_error = angular_error;
        aim_debug.response = response;
        if (settings.aim_prediction)
        {
            const float projectile_speed = settings.projectile_velocity_mps * 100.0F;
            const Vec3 relative{raw_bone.x - snapshot_.camera.location.x,
                raw_bone.y - snapshot_.camera.location.y, raw_bone.z - snapshot_.camera.location.z};
            aim_debug.flight_seconds = settings.aim_intercept_solver ?
                intercept_time(relative, best->velocity, projectile_speed) :
                distance(raw_bone, snapshot_.camera.location) / projectile_speed;
            aim_debug.flight_seconds = std::clamp(aim_debug.flight_seconds +
                settings.prediction_latency_ms * 0.001F, 0.0F, 3.0F);
        }
        if (best->kind == ActorKind::player && best->bone_count > 0)
        {
            float nearest = std::numeric_limits<float>::max();
            for (int slot = 0; slot < best->bone_count; ++slot)
            {
                const float separation = distance(best->bones[static_cast<std::size_t>(slot)], raw_bone);
                if (separation < nearest)
                {
                    nearest = separation;
                    aim_debug.bone_slot = slot;
                }
            }
        }
        rotation.x += pitch_error * response;
        rotation.y += yaw_error * response;
        rotation.z = 0.0F;
        // PDB profile: AShooterPlayerController::SetControlRotation at RVA 0x1087E60.
        // Use the game's override on the game thread instead of a raw memory write. Besides
        // avoiding adjacent input fields, the override owns the mounted-dino rotation path.
        constexpr std::uintptr_t set_control_rotation_rva = 0x1087E60;
        static constexpr std::array<std::uint8_t, 6> prologue{0x48, 0x89, 0x54, 0x24, 0x10, 0x48};
        if (std::memcmp(reinterpret_cast<const void*>(module_base_ + set_control_rotation_rva),
            prologue.data(), prologue.size()) != 0)
        {
            locked_target_ = 0;
            locked_target_occluded_seconds_ = 0.0F;
            status_ = L"Aim disabled: SetControlRotation symbol mismatch";
            return;
        }
        using SetControlRotationFn = void(__fastcall*)(void*, const Vec3&);
        if (!engine_object_live(snapshot_.local_controller))
        {
            locked_target_ = 0;
            locked_target_occluded_seconds_ = 0.0F;
            return;
        }
        const auto function = reinterpret_cast<SetControlRotationFn>(module_base_ + set_control_rotation_rva);
        function(reinterpret_cast<void*>(snapshot_.local_controller), rotation);
    }

    void ArkRuntime::run_camera(Settings& settings, const float delta_seconds)
    {
        if (!snapshot_.camera.valid || snapshot_.local_controller < 0x10000) return;
        std::uintptr_t manager{};
        if (!read(snapshot_.local_controller + offsets_.camera_manager, manager) || manager < 0x10000) return;
        const auto pov = manager + offsets_.camera_cache + offsets_.camera_pov;

        // The engine has already produced the ADS camera for this frame. Never
        // overwrite it while RMB is held; the custom FOV resumes on release.
        const bool weapon_zoom_active = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        if (settings.fov_override && !weapon_zoom_active)
        {
            write(pov + 0x28, settings.camera_fov);
            snapshot_.camera.fov = settings.camera_fov;
        }
        if (!settings.freecam)
        {
            freecam_mouse_x_.store(0, std::memory_order_relaxed);
            freecam_mouse_y_.store(0, std::memory_order_relaxed);
            // freecam_pov_ was captured whenever freecam was last enabled or
            // last saw the camera move (see the "moved > 1.0F" branch
            // below) -- potentially many ticks before this exact frame. If
            // the local pawn/controller got recreated in between (a
            // respawn, a possession change -- this game's local-runtime
            // invalidate/revalidate cycle is not rare, see
            // read_local()'s invalidate_local()), the OLD camera manager
            // freecam_pov_ points into may already be freed and reused for
            // something else entirely. `pov` above is freshly recomputed
            // from THIS frame's live camera_manager read -- only trust
            // freecam_pov_ for the restore-write if it still matches that
            // live address; a mismatch means we're not looking at the same
            // manager we cached, and writing anyway is a stale-pointer
            // write into arbitrary memory, not a real restore. (Found via
            // a live crash: two ACCESS_VIOLATIONs at an address nowhere
            // near this module, immediately after disabling freecam --
            // consistent with corrupting unrelated heap memory here that
            // only crashed later, elsewhere.)
            if (freecam_was_enabled_ && freecam_pov_ >= 0x10000 && freecam_pov_ == pov)
            {
                write(freecam_pov_, freecam_restore_position_);
                write(freecam_pov_ + 0xC, freecam_restore_rotation_);
                if (freecam_restore_fov_ >= 10.0F && freecam_restore_fov_ <= 170.0F)
                    write(freecam_pov_ + 0x28, freecam_restore_fov_);
            }
            freecam_pov_ = 0;
            freecam_was_enabled_ = false;
            return;
        }
        if (!freecam_was_enabled_)
        {
            freecam_position_ = snapshot_.camera.location;
            freecam_rotation_ = snapshot_.camera.rotation;
            freecam_target_rotation_ = snapshot_.camera.rotation;
            freecam_restore_position_ = snapshot_.camera.location;
            freecam_restore_rotation_ = snapshot_.camera.rotation;
            freecam_restore_fov_ = snapshot_.camera.fov;
            freecam_pov_ = pov;
            freecam_was_enabled_ = true;
        }
        else if (distance(snapshot_.camera.location, freecam_position_) > 1.0F)
        {
            // The normal camera tick ran since our previous Present; retain it for a lossless rollback.
            freecam_restore_position_ = snapshot_.camera.location;
            freecam_restore_rotation_ = snapshot_.camera.rotation;
            freecam_restore_fov_ = snapshot_.camera.fov;
            freecam_pov_ = pov;
        }

        const long mouse_x = freecam_mouse_x_.exchange(0, std::memory_order_acq_rel);
        const long mouse_y = freecam_mouse_y_.exchange(0, std::memory_order_acq_rel);
        freecam_target_rotation_.y = normalize_angle(freecam_target_rotation_.y +
            static_cast<float>(mouse_x) * settings.freecam_mouse_sensitivity);
        freecam_target_rotation_.x = std::clamp(freecam_target_rotation_.x -
            static_cast<float>(mouse_y) * settings.freecam_mouse_sensitivity, -89.0F, 89.0F);
        const float rotation_response = settings.freecam_smoothing <= 0.001F ? 1.0F :
            1.0F - std::exp(-std::clamp(delta_seconds, 0.0F, 0.1F) / settings.freecam_smoothing);
        freecam_rotation_.x += (freecam_target_rotation_.x - freecam_rotation_.x) * rotation_response;
        freecam_rotation_.y = normalize_angle(freecam_rotation_.y +
            normalize_angle(freecam_target_rotation_.y - freecam_rotation_.y) * rotation_response);
        freecam_rotation_.z = 0.0F;

        const float yaw = freecam_rotation_.y / radians_to_degrees;
        const Vec3 forward{std::cos(yaw), std::sin(yaw), 0.0F};
        const Vec3 right{-std::sin(yaw), std::cos(yaw), 0.0F};
        const auto held = [](const int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
        const float sprint = held(VK_SHIFT) ? settings.freecam_sprint_multiplier : 1.0F;
        const float step = settings.freecam_speed * sprint * std::clamp(delta_seconds, 0.0F, 0.1F);
        if (held('W')) { freecam_position_.x += forward.x * step; freecam_position_.y += forward.y * step; }
        if (held('S')) { freecam_position_.x -= forward.x * step; freecam_position_.y -= forward.y * step; }
        if (held('D')) { freecam_position_.x += right.x * step; freecam_position_.y += right.y * step; }
        if (held('A')) { freecam_position_.x -= right.x * step; freecam_position_.y -= right.y * step; }
        if (held('E') || held(VK_SPACE)) freecam_position_.z += step * settings.freecam_vertical_multiplier;
        if (held('Q') || held(VK_CONTROL)) freecam_position_.z -= step * settings.freecam_vertical_multiplier;
        write(pov, freecam_position_);
        write(pov + 0xC, freecam_rotation_);
        snapshot_.camera.location = freecam_position_;
        snapshot_.camera.rotation = freecam_rotation_;
    }

    void ArkRuntime::update_no_recoil(const Settings& settings)
    {
        if (!settings.no_recoil)
        {
            restore_no_recoil();
            return;
        }
        if (no_recoil_applied_) return;

        // Exact ShooterGame 358.26 PDB profile. GetRecoilMultiplier aggregates the
        // local character's buff multipliers; GetFireCameraShakeScale adds the weapon
        // camera kick. Returning 0 from both leaves accuracy/spread data untouched.
        constexpr std::uintptr_t recoil_multiplier_rva = 0xD29420;
        constexpr std::uintptr_t fire_shake_scale_rva = 0x11C7450;
        static constexpr std::array<std::uint8_t, 6> recoil_original{
            0x48, 0x89, 0x4C, 0x24, 0x08, 0x48};
        static constexpr std::array<std::uint8_t, 6> shake_original{
            0x48, 0x89, 0x4C, 0x24, 0x08, 0x48};
        static constexpr std::array<std::uint8_t, 6> return_zero{
            0x0F, 0x57, 0xC0, 0xC3, 0x90, 0x90}; // xorps xmm0,xmm0; ret

        const auto patch = [&](const std::uintptr_t rva, const auto& expected, const auto& replacement) {
            auto* address = reinterpret_cast<std::uint8_t*>(module_base_ + rva);
            if (std::memcmp(address, expected.data(), expected.size()) != 0) return false;
            DWORD previous{};
            if (VirtualProtect(address, replacement.size(), PAGE_EXECUTE_READWRITE, &previous) == FALSE) return false;
            std::memcpy(address, replacement.data(), replacement.size());
            FlushInstructionCache(GetCurrentProcess(), address, replacement.size());
            DWORD ignored{};
            VirtualProtect(address, replacement.size(), previous, &ignored);
            return true;
        };

        if (!patch(recoil_multiplier_rva, recoil_original, return_zero))
        {
            status_ = L"No recoil unavailable: GetRecoilMultiplier symbol mismatch";
            return;
        }
        if (!patch(fire_shake_scale_rva, shake_original, return_zero))
        {
            // Roll back the first patch transactionally if the second symbol changed.
            patch(recoil_multiplier_rva, return_zero, recoil_original);
            status_ = L"No recoil unavailable: GetFireCameraShakeScale symbol mismatch";
            return;
        }
        no_recoil_applied_ = true;
    }

    void ArkRuntime::restore_no_recoil() noexcept
    {
        if (!no_recoil_applied_ || module_base_ == 0) return;
        constexpr std::uintptr_t recoil_multiplier_rva = 0xD29420;
        constexpr std::uintptr_t fire_shake_scale_rva = 0x11C7450;
        static constexpr std::array<std::uint8_t, 6> original{
            0x48, 0x89, 0x4C, 0x24, 0x08, 0x48};
        static constexpr std::array<std::uint8_t, 6> patched{
            0x0F, 0x57, 0xC0, 0xC3, 0x90, 0x90};
        const auto restore = [&](const std::uintptr_t rva) {
            auto* address = reinterpret_cast<std::uint8_t*>(module_base_ + rva);
            if (std::memcmp(address, patched.data(), patched.size()) != 0) return;
            DWORD previous{};
            if (VirtualProtect(address, original.size(), PAGE_EXECUTE_READWRITE, &previous) == FALSE) return;
            std::memcpy(address, original.data(), original.size());
            FlushInstructionCache(GetCurrentProcess(), address, original.size());
            DWORD ignored{};
            VirtualProtect(address, original.size(), previous, &ignored);
        };
        restore(fire_shake_scale_rva);
        restore(recoil_multiplier_rva);
        no_recoil_applied_ = false;
    }

    void ArkRuntime::update_no_sway(const Settings& settings)
    {
        if (!settings.no_sway || !snapshot_.local_valid || snapshot_.local_pawn < 0x10000)
        {
            restore_no_sway();
            return;
        }

        // Exact ShooterGame 358.26 PDB fields. This is deliberately separate from
        // recoil and spread: only AShooterCharacter weapon bob and the current
        // AShooterWeapon aim-drift amplitudes are touched.
        constexpr std::uintptr_t current_weapon_offset = 0x1708;
        constexpr std::uintptr_t current_bob_speed_offset = 0x15C0;
        constexpr std::uintptr_t applied_bob_offset = 0x15C8;
        constexpr std::uintptr_t bob_magnitudes_offset = 0x1664;
        constexpr std::uintptr_t bob_offsets_offset = 0x167C;
        constexpr std::uintptr_t targeting_bob_magnitudes_offset = 0x1688;
        constexpr std::uintptr_t targeting_bob_offsets_offset = 0x16A0;
        constexpr std::uintptr_t aim_drift_yaw_offset = 0xB64;
        constexpr std::uintptr_t aim_drift_pitch_offset = 0xB68;

        std::uintptr_t weapon{};
        if (!read(snapshot_.local_pawn + current_weapon_offset, weapon) || weapon < 0x10000)
        {
            restore_no_sway();
            return;
        }
        if (sway_state_.active && (sway_state_.pawn != snapshot_.local_pawn ||
            sway_state_.weapon != weapon || sway_state_.world_generation != snapshot_.world_generation))
            restore_no_sway();

        if (!sway_state_.active)
        {
            SwayState state{};
            state.pawn = snapshot_.local_pawn;
            state.weapon = weapon;
            state.world_generation = snapshot_.world_generation;
            if (!read(state.pawn + current_bob_speed_offset, state.current_bob_speed) ||
                !read(state.pawn + applied_bob_offset, state.applied_bob) ||
                !read(state.pawn + bob_magnitudes_offset, state.bob_magnitudes) ||
                !read(state.pawn + bob_offsets_offset, state.bob_offsets) ||
                !read(state.pawn + targeting_bob_magnitudes_offset, state.targeting_bob_magnitudes) ||
                !read(state.pawn + targeting_bob_offsets_offset, state.targeting_bob_offsets) ||
                !read(state.weapon + aim_drift_yaw_offset, state.aim_drift_yaw) ||
                !read(state.weapon + aim_drift_pitch_offset, state.aim_drift_pitch))
            {
                status_ = L"No sway waiting for a valid local weapon";
                return;
            }
            state.active = true;
            sway_state_ = state;
        }

        constexpr float zero{};
        constexpr Vec3 zero_vector{};
        write(sway_state_.pawn + current_bob_speed_offset, zero);
        write(sway_state_.pawn + applied_bob_offset, zero);
        write(sway_state_.pawn + bob_magnitudes_offset, zero_vector);
        write(sway_state_.pawn + bob_offsets_offset, zero_vector);
        write(sway_state_.pawn + targeting_bob_magnitudes_offset, zero_vector);
        write(sway_state_.pawn + targeting_bob_offsets_offset, zero_vector);
        write(sway_state_.weapon + aim_drift_yaw_offset, zero);
        write(sway_state_.weapon + aim_drift_pitch_offset, zero);
    }

    void ArkRuntime::restore_no_sway() noexcept
    {
        if (!sway_state_.active) return;
        constexpr std::uintptr_t current_bob_speed_offset = 0x15C0;
        constexpr std::uintptr_t applied_bob_offset = 0x15C8;
        constexpr std::uintptr_t bob_magnitudes_offset = 0x1664;
        constexpr std::uintptr_t bob_offsets_offset = 0x167C;
        constexpr std::uintptr_t targeting_bob_magnitudes_offset = 0x1688;
        constexpr std::uintptr_t targeting_bob_offsets_offset = 0x16A0;
        constexpr std::uintptr_t aim_drift_yaw_offset = 0xB64;
        constexpr std::uintptr_t aim_drift_pitch_offset = 0xB68;

        // Never write into an object graph from an old UWorld. Inside the current
        // generation validate both UObject vtables before restoring saved values.
        if (sway_state_.world_generation == snapshot_.world_generation)
        {
            void* pawn_vtable{};
            void* weapon_vtable{};
            if (read(sway_state_.pawn, pawn_vtable) && read(sway_state_.weapon, weapon_vtable) &&
                reinterpret_cast<std::uintptr_t>(pawn_vtable) >= module_base_ &&
                reinterpret_cast<std::uintptr_t>(pawn_vtable) < module_base_ + module_size_ &&
                reinterpret_cast<std::uintptr_t>(weapon_vtable) >= module_base_ &&
                reinterpret_cast<std::uintptr_t>(weapon_vtable) < module_base_ + module_size_)
            {
                write(sway_state_.pawn + current_bob_speed_offset, sway_state_.current_bob_speed);
                write(sway_state_.pawn + applied_bob_offset, sway_state_.applied_bob);
                write(sway_state_.pawn + bob_magnitudes_offset, sway_state_.bob_magnitudes);
                write(sway_state_.pawn + bob_offsets_offset, sway_state_.bob_offsets);
                write(sway_state_.pawn + targeting_bob_magnitudes_offset, sway_state_.targeting_bob_magnitudes);
                write(sway_state_.pawn + targeting_bob_offsets_offset, sway_state_.targeting_bob_offsets);
                write(sway_state_.weapon + aim_drift_yaw_offset, sway_state_.aim_drift_yaw);
                write(sway_state_.weapon + aim_drift_pitch_offset, sway_state_.aim_drift_pitch);
            }
        }
        sway_state_ = {};
    }

    void ArkRuntime::restore_transient_state()
    {
        restore_no_recoil();
        restore_no_sway();
        // Same staleness risk as run_camera()'s disable branch (see its
        // comment) -- freecam_pov_ may be many ticks old by the time
        // unload gets here, and this is called from arbitrary unload
        // timing, not a fresh per-frame tick, so the risk of the cached
        // manager having been freed/replaced underneath us is if anything
        // higher here. Re-derive the CURRENT camera pov the same way
        // run_camera() does and only restore if it still matches.
        if (freecam_was_enabled_ && freecam_pov_ >= 0x10000)
        {
            std::uintptr_t manager{};
            const auto pov = read(snapshot_.local_controller + offsets_.camera_manager, manager) && manager >= 0x10000 ?
                manager + offsets_.camera_cache + offsets_.camera_pov : std::uintptr_t{};
            if (freecam_pov_ == pov)
            {
                write(freecam_pov_, freecam_restore_position_);
                write(freecam_pov_ + 0xC, freecam_restore_rotation_);
                if (freecam_restore_fov_ >= 10.0F && freecam_restore_fov_ <= 170.0F)
                    write(freecam_pov_ + 0x28, freecam_restore_fov_);
            }
        }
        freecam_was_enabled_ = false;
        freecam_pov_ = 0;
        // This is the worker-thread fallback. Engine render-state methods are game-thread
        // only; normal unload restores them in hooked_camera_update before hooks are removed.
        abandon_chams();
    }
}
