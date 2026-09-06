#include "kopt/share_filter.hpp"

#include <cmath>
#include <ios>
#include <sstream>

namespace kopt::share
{
    namespace
    {
        float distance3(const float ax, const float ay, const float az,
            const float bx, const float by, const float bz) noexcept
        {
            const float dx = ax - bx;
            const float dy = ay - by;
            const float dz = az - bz;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        const wchar_t* kind_label(const Kind kind) noexcept
        {
            switch (kind)
            {
            case Kind::player: return L"player";
            case Kind::dino: return L"dino";
            case Kind::turret: return L"turret";
            case Kind::structure: return L"structure";
            case Kind::other: return L"other";
            }
            return L"other";
        }

        // Значимо изменилось -- позиция сдвинулась дальше эпсилона,
        // здоровье/сон/смерть/владелец сменились, либо турель поменяла
        // боезапас/питание/цель. Мелкий джиттер координат и субпиксельные
        // колебания здоровья намеренно игнорируются -- иначе фильтр не
        // экономит вообще ничего на "неподвижных" структурах, у которых
        // координата дрожит на доли сантиметра из-за плавающей точки.
        bool significantly_changed(const Sighting& prev, const Sighting& cur,
            const FilterConfig& cfg) noexcept
        {
            if (prev.dead != cur.dead || prev.sleeping != cur.sleeping) return true;
            if (prev.tribe != cur.tribe) return true; // раскопка/смена владельца
            if (distance3(prev.x, prev.y, prev.z, cur.x, cur.y, cur.z) >
                cfg.position_epsilon_cm) return true;
            if (prev.has_health != cur.has_health) return true;
            if (cur.has_health &&
                std::fabs(prev.health - cur.health) > cfg.health_epsilon) return true;
            if (prev.turret.has_value() != cur.turret.has_value()) return true;
            if (prev.turret && cur.turret)
            {
                const TurretInfo& a = *prev.turret;
                const TurretInfo& b = *cur.turret;
                if (a.ammo != b.ammo || a.powered != b.powered || a.active != b.active ||
                    a.targeting != b.targeting || a.targeting_actor != b.targeting_actor ||
                    a.warning != b.warning || a.range != b.range) return true;
            }
            return false;
        }
    }

    std::vector<Sighting> ChangeFilter::filter(const std::vector<Sighting>& current,
        const std::chrono::steady_clock::time_point now)
    {
        std::vector<Sighting> out;
        out.reserve(current.size());
        for (const Sighting& s : current)
        {
            const Key key{s.address, s.class_name};
            const auto keyframe = (s.kind == Kind::structure || s.kind == Kind::turret) ?
                cfg_.keyframe_structure : cfg_.keyframe_mobile;
            const auto it = sent_.find(key);

            bool send = it == sent_.end();
            if (!send && now - it->second.at >= keyframe) send = true;
            if (!send && significantly_changed(it->second.last, s, cfg_)) send = true;

            if (send)
            {
                out.push_back(s);
                sent_[key] = SentState{s, now};
            }
        }
        return out;
    }

    std::vector<std::wstring> ChangeFilter::collect_vanished(const std::vector<Sighting>& current)
    {
        std::unordered_set<Key, KeyHash> now_keys;
        now_keys.reserve(current.size());
        for (const Sighting& s : current) now_keys.insert(Key{s.address, s.class_name});

        std::vector<std::wstring> vanished;
        for (auto it = seen_last_.begin(); it != seen_last_.end();)
        {
            const Key& key = *it;
            if (now_keys.find(key) != now_keys.end())
            {
                ++it;
                continue;
            }

            const wchar_t* label = L"other";
            const auto sent_it = sent_.find(key);
            if (sent_it != sent_.end()) label = kind_label(sent_it->second.last.kind);

            std::wostringstream oss;
            oss << label << L":0x" << std::hex << key.address << L":" << key.class_name;
            vanished.push_back(oss.str());

            sent_.erase(key);
            it = seen_last_.erase(it);
        }
        for (const Key& k : now_keys) seen_last_.insert(k);
        return vanished;
    }
}
