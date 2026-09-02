#pragma once

// DTO-слой для шеринга: превращает то, что клиент уже рисует локально
// (kopt::Actor/kopt::Alert), в стабильный, версионируемый формат для сети.
// Источник правды -- клиент: построители здесь читают ровно те же поля,
// что уже наполняет ArkRuntime для локального ESP, ничего не пересчитывая
// заново на другом конце канала.
//
// Раздельно от транспорта (kopt::Publisher, publisher.hpp) нарочно: этот
// заголовок не знает о сети вообще, и потому тестируется без неё --
// build_sightings() принимает snapshot.actors и отдаёт DTO, детерминированно
// и без сторонних эффектов.

#include "kopt/runtime.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace kopt::share
{
    // Версия едет на каждом сообщении, а не согласовывается один раз при
    // подключении: HTTP/3-запросы здесь -- "выстрелил и забыл" по кадрам, а
    // не долгоживущая сессия с рукопожатием под одну версию.
    inline constexpr std::uint8_t kSchemaVersion = 1;

    // Не ActorKind напрямую: ActorKind свободен получить новое внутреннее
    // значение (например, будущий vehicle), не протекая на провод, пока
    // kind_from_actor() не научится его туда явно отображать.
    enum class Kind : std::uint8_t
    {
        other,
        player,
        dino,
        turret,
        structure
    };

    // Отображение ActorKind -> Kind. Турель -- не отдельный ActorKind (см.
    // ArkRuntime::ClassMeta -- turret это отдельный булев флаг на структуре,
    // не другое значение kind), поэтому этой функции недостаточно -- решение
    // "structure или turret" довершается в build_structure() по actor.turret.
    [[nodiscard]] Kind kind_from_actor(ActorKind k) noexcept;

    struct TurretInfo
    {
        // -1 -- не удалось прочитать, отличать от "боезапас пуст".
        std::int32_t ammo{-1};
        std::uint8_t range{};
        std::uint8_t targeting{};
        std::uint8_t warning{};
        bool powered{};
        bool active{};
        bool targeting_actor{};
    };

    struct Sighting
    {
        std::uint8_t schema_version{kSchemaVersion};
        Kind kind{Kind::other};

        // Адрес процесса -- годится только для дедупликации во времени
        // внутри одного клиента (см. ChangeFilter, share_filter.hpp).
        // Между разными клиентами ничего не значит -- у чужого процесса
        // совершенно другое адресное пространство.
        std::uintptr_t address{};

        // stable_id -- игровой account-id (linked_player_data_id) для
        // игроков, единственное поле здесь, которое остаётся осмысленным
        // между разными клиентами. 0 для дино/структур/турелей -- у них
        // такого id в памяти игры не существует ни у одного клиента.
        std::uint64_t stable_id{};

        std::int32_t team{};
        std::wstring label;
        std::wstring class_name;
        std::wstring tribe;

        float x{};
        float y{};
        float z{};

        float health{};
        float max_health{};
        bool has_health{};

        float torpor{};
        float max_torpor{};
        bool has_torpor{};

        bool sleeping{};
        bool dead{};

        // Заполнено только для kind == Kind::turret.
        std::optional<TurretInfo> turret;
    };

    struct Notification
    {
        std::uint8_t schema_version{kSchemaVersion};
        AlertKind kind{};
        std::wstring title;
        std::wstring name;
        std::wstring tribe;
        float distance_m{};
        float value{};
        // Абсолютная позиция цели, породившей событие -- distance_m одного
        // достаточно для локальной карточки (та же точка отсчёта, что у
        // игрока, который её видит), но бесполезно на другом конце канала
        // шеринга без своей точки отсчёта.
        Vec3 position{};
    };

    // Собирает событие в Notification. Групповые тревоги (AlertKind::
    // enemy_group) не привязаны к одной цели -- у них Alert::position
    // остаётся нулевым, это не баг конвертера, а честное отсутствие точки.
    [[nodiscard]] Notification build_notification(const Alert& a);

    // Один построитель на ActorKind. Каждый сам решает, достаточно ли
    // данных сказать что-то -- симметрично esp-построителям конкретных
    // фигур в overlay.cpp (там то же правило "сам решаю, рисовать ли",
    // здесь -- "сам решаю, отдавать ли").
    using Builder = bool (*)(const Actor&, Sighting&);

    bool build_player(const Actor& a, Sighting& out);
    bool build_dino(const Actor& a, Sighting& out);
    // Общий построитель для ActorKind::structure -- поднимает Kind до
    // Kind::turret, если actor.turret истинен (турель -- это структура с
    // булевым флагом, не отдельный ActorKind, см. class_meta() в
    // runtime.cpp).
    bool build_structure(const Actor& a, Sighting& out);

    // Построитель для данного ActorKind, либо nullptr, если для этого вида
    // сознательно ничего не шарим (drop/death_cache/other). Новый вид
    // сущности позже -- это один новый build_*() плюс одна строка в теле
    // builder_for() -- остальной код не трогается.
    [[nodiscard]] Builder builder_for(ActorKind kind) noexcept;

    // Собирает DTO по всему снимку разом. Мёртвые/неживые акторы, для
    // которых нет построителя, молча пропускаются -- не ошибка, просто вне
    // объёма шеринга (CatOther-подобные, служебные точки карты и т.п.).
    [[nodiscard]] std::vector<Sighting> build_sightings(const std::vector<Actor>& actors);

    // Локальный игрок никогда не попадает в snapshot.actors -- ArkRuntime
    // намеренно исключает его на этапе обнаружения (см. update() в
    // runtime.cpp: адрес local_pawn/local_character явно пропускается),
    // потому что actors -- это "что я вижу вокруг", а не "где я сам", и
    // ESP-оверлею собственная рамка на себе не нужна. Для шеринга это ровно
    // обратное: тиммейтам нужны именно свои координаты отправителя. Читает
    // только уже собранные snapshot.local_* поля -- нового обращения к
    // памяти игры не требует. Возвращает nullopt, когда !local_valid (нет
    // валидного пешки -- нечего сообщать).
    //
    // address = local_pawn: тот же смысл, что у Sighting::address для любого
    // обычного актора -- ключ ChangeFilter (address, class_name) внутри
    // одного клиента, не межклиентский идентификатор (для этого служит
    // stable_id = local_stable_id).
    [[nodiscard]] std::optional<Sighting> build_self_sighting(const Snapshot& snapshot);
}
