// Standalone smoke test for kopt::share's DTO builders, ChangeFilter, and
// ReporterFilter -- no D3D11/game process needed (none of these three
// pieces touch game memory or the network), same shape kopt_relay_test used
// for RelayClient: no external test framework, plain assertions, non-zero
// exit on failure. Run manually under Proton/Wine or natively on Windows.

#include "kopt/share.hpp"
#include "kopt/share_filter.hpp"
#include "kopt/share_remote.hpp"

#include <cstdio>

using namespace kopt;
using namespace kopt::share;

namespace
{
    int g_failures{};

    void require(const bool condition, const char* message)
    {
        if (!condition)
        {
            std::fprintf(stderr, "FAIL: %s\n", message);
            ++g_failures;
        }
    }

    Actor make_actor(const ActorKind kind, const std::uintptr_t address)
    {
        Actor a;
        a.address = address;
        a.kind = kind;
        a.team = 42;
        a.class_name = L"TestClass_C";
        a.name = L"TestActor";
        a.max_health = 100.0F;
        a.health = 80.0F;
        return a;
    }

    void test_build_sightings()
    {
        std::vector<Actor> actors;
        actors.push_back(make_actor(ActorKind::player, 0x1000));
        actors.push_back(make_actor(ActorKind::dino, 0x2000));
        actors.push_back(make_actor(ActorKind::structure, 0x3000));

        Actor turret = make_actor(ActorKind::structure, 0x4000);
        turret.turret = true;
        turret.turret_ammo = 50;
        turret.turret_powered = true;
        actors.push_back(turret);

        // Вне объёма шеринга -- должны быть пропущены builder_for().
        actors.push_back(make_actor(ActorKind::drop, 0x5000));
        actors.push_back(make_actor(ActorKind::death_cache, 0x6000));

        const auto sightings = build_sightings(actors);
        require(sightings.size() == 4, "build_sightings: drop/death_cache должны быть пропущены");

        bool found_turret = false;
        bool found_structure = false;
        for (const Sighting& s : sightings)
        {
            if (s.address == 0x4000)
            {
                found_turret = true;
                require(s.kind == Kind::turret, "турель должна получить Kind::turret, а не Kind::structure");
                require(s.turret.has_value(), "TurretInfo должен быть заполнен для турели");
                if (s.turret)
                {
                    require(s.turret->ammo == 50, "turret.ammo должен совпадать с actor.turret_ammo");
                    require(s.turret->powered, "turret.powered должен совпадать с actor.turret_powered");
                }
            }
            if (s.address == 0x3000)
            {
                found_structure = true;
                require(s.kind == Kind::structure, "обычная структура не должна становиться турелью");
                require(!s.turret.has_value(), "TurretInfo не должен заполняться для не-турели");
            }
        }
        require(found_turret, "турель должна присутствовать в результате build_sightings");
        require(found_structure, "обычная структура должна присутствовать в результате build_sightings");
    }

    void test_build_notification()
    {
        Alert alert;
        alert.kind = AlertKind::turret;
        alert.title = L"Test alert";
        alert.name = L"Someone";
        alert.tribe = L"SomeTribe";
        alert.distance_m = 12.5F;
        alert.value = 3.0F;
        alert.position = Vec3{100.0F, 200.0F, 300.0F};

        const Notification note = build_notification(alert);
        require(note.kind == AlertKind::turret, "Notification.kind должен совпадать с Alert.kind");
        require(note.title == alert.title, "Notification.title должен совпадать с Alert.title");
        require(note.position.x == 100.0F && note.position.y == 200.0F && note.position.z == 300.0F,
            "Notification.position должен совпадать с Alert.position");
    }

    void test_change_filter()
    {
        FilterConfig cfg;
        cfg.keyframe_structure = std::chrono::milliseconds(1000);
        cfg.position_epsilon_cm = 10.0F;
        ChangeFilter filter(cfg);

        Sighting s;
        s.kind = Kind::structure;
        s.address = 0x9000;
        s.class_name = L"Wall_C";

        const auto t0 = std::chrono::steady_clock::now();
        require(filter.filter({s}, t0).size() == 1, "новая цель должна отправиться немедленно");
        require(filter.collect_vanished({s}).empty(), "присутствующая цель не должна считаться пропавшей");

        require(filter.filter({s}, t0 + std::chrono::milliseconds(100)).empty(),
            "неизменная цель до keyframe не должна пересылаться повторно");

        require(filter.filter({s}, t0 + std::chrono::milliseconds(1500)).size() == 1,
            "маячок должен переслать цель после истечения keyframe, даже без изменений");

        Sighting moved = s;
        moved.x = 50.0F; // больше position_epsilon_cm
        require(filter.filter({moved}, t0 + std::chrono::milliseconds(1600)).size() == 1,
            "значимое перемещение должно отправиться немедленно, не дожидаясь маячка");

        require(filter.collect_vanished({}).size() == 1,
            "исчезнувшая цель должна попасть в collect_vanished ровно один раз");
        require(filter.collect_vanished({}).empty(),
            "повторный вызов collect_vanished не должен снова сообщать об уже пропавшей цели");
    }

    void test_reporter_filter()
    {
        const ReporterFilter filter(/*own_stable_id=*/777, /*radius_cm=*/1000.0F);
        const Vec3 my_pos{0.0F, 0.0F, 0.0F};

        require(!filter.accept(777, Vec3{5000.0F, 0.0F, 0.0F}, my_pos),
            "собственные данные (own_stable_id) никогда не принимаются");
        require(!filter.accept(111, Vec3{500.0F, 0.0F, 0.0F}, my_pos),
            "репортёр внутри радиуса должен быть отклонён");
        require(filter.accept(111, Vec3{5000.0F, 0.0F, 0.0F}, my_pos),
            "репортёр за пределами радиуса должен быть принят");
    }
}

int main()
{
    test_build_sightings();
    test_build_notification();
    test_change_filter();
    test_reporter_filter();

    if (g_failures == 0)
    {
        std::printf("kopt_share_selftest: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "kopt_share_selftest: %d check(s) failed\n", g_failures);
    return 1;
}
