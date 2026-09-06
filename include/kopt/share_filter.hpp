#pragma once

// Не спамить неизменным: сотни неподвижных турелей/стен не должны улетать в
// сеть каждый тик только потому, что они существуют. ChangeFilter решает,
// какие Sighting из build_sightings() реально стоит отправить в этот раз --
// новые, значимо изменившиеся, и просроченные по keyframe-маячку (чтобы
// TTL/кэш на другом конце не решил по тишине, что цель пропала).
//
// build_sightings() остаётся чистой функцией без памяти о прошлом -- эта
// память живёт здесь, в отдельном стейтфул-слое между построением и
// Publisher::submit_sightings.

#include "kopt/share.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kopt::share
{
    struct FilterConfig
    {
        // Меньше этого -- "не сдвинулся", не считается изменением.
        float position_epsilon_cm{25.0F};
        float health_epsilon{1.0F};
        // Маячок: даже без изменений пересылаем не реже этого периода,
        // чтобы TTL/кэш на другом конце не решил по тишине, что цель
        // пропала. Структуры/турели почти всегда неподвижны -- маячок
        // редкий; игроки/дино почти всегда "изменились" самим движением, но
        // на случай, если стоят на месте (спят, привязаны), маячок чаще.
        std::chrono::milliseconds keyframe_structure{30000};
        std::chrono::milliseconds keyframe_mobile{5000};
    };

    class ChangeFilter
    {
    public:
        explicit ChangeFilter(FilterConfig cfg = {}) noexcept : cfg_(cfg) {}

        // Из текущего снимка отбирает то, что реально стоит слать; остальное
        // молча пропускает. Обновляет внутреннюю память о последнем
        // отправленном состоянии -- не переиспользовать один ChangeFilter из
        // разных потоков без внешней синхронизации.
        [[nodiscard]] std::vector<Sighting> filter(const std::vector<Sighting>& current,
            std::chrono::steady_clock::time_point now);

        // Ключи, которые были в прошлом вызове и пропали сейчас -- цель
        // умерла/деспавнилась/вышла из радиуса чтения. Явный сигнал "больше
        // не вижу" вместо надежды на TTL получателя. Вызывать один раз за
        // тик на том же current, что и filter() -- порядок вызова между
        // ними не важен, оба читают current независимо.
        [[nodiscard]] std::vector<std::wstring> collect_vanished(
            const std::vector<Sighting>& current);

    private:
        // Ключ -- (address, class_name), не голый address: адрес процесса
        // переиспользуется движком после смерти актора, и без class_name в
        // паре можно принять нового актора на старом адресе за прежний,
        // просто передвинувшийся -- состояние тогда молча "телепортируется".
        struct Key
        {
            std::uintptr_t address;
            std::wstring class_name;
            bool operator==(const Key& other) const noexcept
            {
                return address == other.address && class_name == other.class_name;
            }
        };
        struct KeyHash
        {
            std::size_t operator()(const Key& k) const noexcept
            {
                return std::hash<std::uintptr_t>{}(k.address) ^
                    (std::hash<std::wstring>{}(k.class_name) << 1);
            }
        };
        struct SentState
        {
            Sighting last;
            std::chrono::steady_clock::time_point at;
        };

        std::unordered_map<Key, SentState, KeyHash> sent_;
        // Отдельно от sent_: ключ может присутствовать в снимке, но ещё не
        // быть due для отправки (keyframe не истёк, ничего не изменилось) --
        // такой ключ не должен считаться "пропавшим", пока он правда не
        // исчезнет из current.
        std::unordered_set<Key, KeyHash> seen_last_;
        FilterConfig cfg_;
    };
}
