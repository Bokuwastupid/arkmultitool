#include "kopt/share.hpp"

namespace kopt::share
{
    Kind kind_from_actor(const ActorKind k) noexcept
    {
        switch (k)
        {
        case ActorKind::player: return Kind::player;
        case ActorKind::dino: return Kind::dino;
        // structure -- дальше решает build_structure() по actor.turret:
        // турель это структура с булевым флагом, не отдельный ActorKind.
        case ActorKind::structure: return Kind::structure;
        case ActorKind::drop:
        case ActorKind::death_cache:
        case ActorKind::other: return Kind::other;
        }
        return Kind::other;
    }

    namespace
    {
        // has_health/has_torpor через max_* > 0, а не отдельный флаг на
        // Actor -- тот же приём, что actor_is_dead()/player_esp_state()
        // (runtime.hpp) уже используют для той же пары полей: нулевой
        // max_health значит "поле не читалось для этого класса", а не
        // "здоровье кончилось".
        void fill_common(const Actor& a, Sighting& out)
        {
            out.address = a.address;
            out.team = a.team;
            out.label = a.name;
            out.class_name = a.class_name;
            out.tribe = a.tribe;
            out.x = a.position.x;
            out.y = a.position.y;
            out.z = a.position.z;
            out.health = a.health;
            out.max_health = a.max_health;
            out.has_health = a.max_health > 0.0F;
            out.sleeping = a.sleeping;
            out.dead = actor_is_dead(a);
        }
    }

    bool build_player(const Actor& a, Sighting& out)
    {
        out = Sighting{};
        out.kind = Kind::player;
        fill_common(a, out);
        out.stable_id = a.linked_player_data_id;
        out.torpor = a.torpor;
        out.max_torpor = a.max_torpor;
        out.has_torpor = a.max_torpor > 0.0F;
        return true;
    }

    bool build_dino(const Actor& a, Sighting& out)
    {
        out = Sighting{};
        out.kind = Kind::dino;
        fill_common(a, out);
        // Дино не имеет постоянного игрового id ни у одного клиента --
        // stable_id остаётся нулём, см. TurretInfo/Sighting doc-комментарий
        // в share.hpp про кросс-клиентную идентичность.
        out.torpor = a.torpor;
        out.max_torpor = a.max_torpor;
        out.has_torpor = a.max_torpor > 0.0F;
        return true;
    }

    bool build_structure(const Actor& a, Sighting& out)
    {
        out = Sighting{};
        out.kind = a.turret ? Kind::turret : Kind::structure;
        fill_common(a, out);
        if (a.turret)
        {
            TurretInfo info;
            info.ammo = a.turret_ammo;
            info.range = a.turret_range;
            info.targeting = a.turret_targeting;
            info.warning = a.turret_warning;
            info.powered = a.turret_powered;
            info.active = a.turret_active;
            info.targeting_actor = a.turret_targeting_actor;
            out.turret = info;
        }
        return true;
    }

    Builder builder_for(const ActorKind kind) noexcept
    {
        switch (kind)
        {
        case ActorKind::player: return &build_player;
        case ActorKind::dino: return &build_dino;
        case ActorKind::structure: return &build_structure;
        // Вне объёма шеринга сознательно: ящики снабжения и кэши предметов
        // не несут тактической ценности, которую стоило бы передавать
        // команде -- в отличие от игроков/дино/структур/турелей.
        case ActorKind::drop:
        case ActorKind::death_cache:
        case ActorKind::other: return nullptr;
        }
        return nullptr;
    }

    std::vector<Sighting> build_sightings(const std::vector<Actor>& actors)
    {
        std::vector<Sighting> out;
        out.reserve(actors.size());
        for (const Actor& a : actors)
        {
            const Builder builder = builder_for(a.kind);
            if (builder == nullptr) continue;
            Sighting s;
            if (builder(a, s)) out.push_back(std::move(s));
        }
        return out;
    }

    std::optional<Sighting> build_self_sighting(const Snapshot& snapshot)
    {
        if (!snapshot.local_valid) return std::nullopt;
        Sighting out;
        out.kind = Kind::player;
        out.address = snapshot.local_pawn;
        out.stable_id = snapshot.local_stable_id;
        out.team = snapshot.local_team;
        out.x = snapshot.local_position.x;
        out.y = snapshot.local_position.y;
        out.z = snapshot.local_position.z;
        out.label = snapshot.local_name;
        out.tribe = snapshot.local_tribe;
        // class_name/health/torpor: not read for the local player anywhere
        // today (only ever needed for ESP display on OTHER actors) -- left
        // at Sighting{}'s defaults rather than inventing placeholder
        // values. has_health/has_torpor already default false, so
        // max_health==0/max_torpor==0 read correctly on the far end as
        // "not reported", same as any other actor missing that data. label
        // is NOT optional like those, though -- ark_relay's
        // Entity.Validate() rejects (and disconnects) an empty label for
        // Category player, confirmed live, so it comes from
        // snapshot.local_name rather than being left blank.
        return out;
    }

    Notification build_notification(const Alert& a)
    {
        Notification n;
        n.kind = a.kind;
        n.title = a.title;
        n.name = a.name;
        n.tribe = a.tribe;
        n.distance_m = a.distance_m;
        n.value = a.value;
        n.position = a.position;
        return n;
    }
}
