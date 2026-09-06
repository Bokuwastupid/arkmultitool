#pragma once

// Приёмная сторона шеринга: то, что прислали другие клиенты той же команды,
// и решение, стоит ли это вообще показывать.
//
// Дедупликация не посущностная (не сеточный хеш по каждому дино/структуре) --
// репортёр всегда игрок с настоящим игровым id, поэтому решение принимается
// на весь его батч разом: если репортёр сейчас у меня в радиусе обзора,
// считаем, что он видит примерно то же, что и я, и весь его батч не рисуем.
// Грубее посущностной сверки, зато на порядок дешевле (одно сравнение
// расстояний на батч, а не хеш на каждую цель) и не требует держать
// координатную сетку для дино/структур/турелей, у которых в памяти игры нет
// собственного id ни у одного клиента.

#include "kopt/share.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace kopt::share
{
    // Целиком то, что прислал один репортёр за один раз.
    struct RemoteBatch
    {
        // Числовой игровой id репортёра (linked_player_data_id) и его
        // позиция на момент отправки батча -- protocol.Outbound
        // (backend_go/internal/protocol/message.go) несёт их как
        // reporter_character_id/reporter_x/y/z, парсится в
        // Http3Publisher::parse_broadcast. Может быть 0/{} для старого
        // клиента или если у отправителя !local_valid в момент отправки
        // (см. Publisher::submit_sightings's own doc comment) -- ReporterFilter
        // ниже уже написан и протестирован под эти значения.
        std::uint64_t reporter_stable_id{};
        Vec3 reporter_position{};
        // reported_by с провода -- единственное реально приходящее поле,
        // однозначно отличающее одного репортёра от другого. RemoteView
        // ключует по нему (см. его собственный комментарий): раньше
        // ключевание шло по reporter_stable_id, который всегда 0 для любого
        // отправителя -- батчи разных тиммейтов стирали друг друга в одной
        // и той же ячейке карты, и на экране в любой момент оставался
        // только тот, чей батч пришёл по сети последним.
        std::string reporter_account_id;
        std::vector<Sighting> sightings;
        std::vector<Notification> notifications;
        std::vector<std::wstring> vanished;
        std::chrono::steady_clock::time_point received_at{};
    };

    // Решение на весь батч репортёра разом, а не на каждую цель.
    class ReporterFilter
    {
    public:
        ReporterFilter(std::uint64_t own_stable_id, float radius_cm) noexcept;

        // false -- батч не показываем: либо это мы сами (own_stable_id),
        // либо репортёр внутри radius_cm, где я, скорее всего, вижу то же
        // сам. true -- репортёр вне моего обзора, его батч несёт то, чего у
        // меня ещё нет.
        //
        // Граница ровно на radius_cm включена в "внутри" (строгое >, не >=)
        // -- совпадающие с границей репортёры чаще результат идентичных
        // округлённых координат (двое стоят плечом к плечу), а не два
        // разных наблюдателя на противоположных концах базы.
        [[nodiscard]] bool accept(std::uint64_t reporter_stable_id,
            const Vec3& reporter_position, const Vec3& my_position) const noexcept;

    private:
        std::uint64_t own_stable_id_;
        float radius_cm_;
    };

    // Буфер отрисовки: последний принятый батч на репортёра, с протуханием.
    // Не смешивается с ChangeFilter -- там память о СВОИХ последних
    // отправленных состояниях, здесь -- о ЧУЖИХ последних принятых батчах.
    class RemoteView
    {
    public:
        // ttl -- дольше самого длинного keyframe-интервала источника
        // (FilterConfig::keyframe_structure), иначе собственный маячок
        // отправителя будет считаться протухшим раньше, чем он успел
        // повториться.
        explicit RemoteView(std::chrono::milliseconds ttl) noexcept;

        void update(RemoteBatch batch);

        // Батчи всех репортёров, чья запись не протухла; протухшие тихо
        // вычищаются здесь же -- если у тиммейта упало соединение, его
        // последний отчёт не должен висеть на экране бесконечно.
        [[nodiscard]] std::vector<RemoteBatch> visible(
            std::chrono::steady_clock::time_point now);

    private:
        // Ключ -- reporter_account_id (см. RemoteBatch's own doc comment),
        // не reporter_stable_id: последний всегда 0 для любого отправителя
        // (протокол не переносит игровой id), так что ключевание по нему
        // сводило бы всех репортёров в одну ячейку.
        std::unordered_map<std::string, RemoteBatch> by_reporter_;
        std::chrono::milliseconds ttl_;
    };
}
