# KOPT Internal — roadmap

Этот документ фиксирует порядок разработки, критерии готовности и будущие функции.
Главный принцип: новая функция не считается готовой, если она ухудшает существующий
ESP, ввод, качество изображения, время кадра, reconnect или безопасную выгрузку DLL.

## Границы проекта

Обязательная область:

- внутреннее DX11-меню и ESP;
- aim на игроках и динозаврах, включая управление верхом;
- freecam, camera/FOV и безопасные локальные visual-функции;
- Local/Enemy CustomDepth chams;
- ручные alliances и tribe relations;
- экипировка, durability и weapon icons;
- alerts, radar, summaries, grouping и фильтры из ARK Multi Tool;
- Windows injection и запуск через Proton/Wine на Linux;
- локальные versioned-настройки самого софта; игровая INI/CVar-тема отложена.

Не переносим:

- breeding automation;
- Smart Herd;
- автоматизацию прокачки;
- автоматические клики по статам;
- другие unattended gameplay-автоматизации.

## Этап 0 — аварийная стабилизация

Статус: в работе. До завершения этого этапа новая DLL не выдаётся для обычной игры.

1. Reconnect-safe lifecycle.
   - Отслеживать адрес и generation `UWorld`.
   - Останавливать chams/freecam/aim/alerts на travel и teardown.
   - Не вызывать методы Unreal на stale или unregistering components.
   - Не вызывать render-state функции из worker thread.
   - Очищать actor/component/alert cache при смене мира.
   - Критерий: цикл join → disconnect → reconnect не падает минимум 5 раз подряд.

2. Aim input corruption.
   - Использовать PDB-подтверждённый `AController::ControlRotation + 0x490`.
   - Игнорировать старый ошибочный `0x4A0` из legacy-конфига.
   - Немедленно прекращать запись после release/disable/menu/panic/world transition.
   - Критерий: обычная мышь работает до, во время и после Hold/Toggle/Always.

3. Safe unload/reinject.
   - Очистка на game thread до снятия camera hook.
   - Ожидание активных callback без принудительной выгрузки исполняемого кода.
   - Критерий: unload → reinject без рестарта ARK и без crash.

## Этап 1 — ввод и меню

1. Выбирать один основной игровой DXGI swap chain.
2. Масштабировать client mouse coordinates к размеру backbuffer.
3. Один physical click должен создавать одно UI-событие.
4. Проверить checkbox, toggle, combo, slider, tabs, menu drag и Preview drag-and-drop.
5. Поддержать бинды мыши, Ctrl, Alt, Shift, `WM_SYSKEYDOWN/UP` и конфликт bindings.
6. Home только показывает/скрывает меню; при закрытии полностью возвращает ввод игре.
7. Критерий: 100 последовательных кликов по тестовым checkbox без потерь и самопереключений.

## Этап 2 — ESP Preview и визуальная среда

1. Player ESP style отделён от world/dino/structure/drop style.
2. Preview меняет только player box/label/health/torpor anchors.
3. Preview рисуется отдельным боковым окном и переносится влево при нехватке места.
4. Preview сохраняет drag-and-drop handles и качественный реальный макет игрока.
5. Отдельная страница World Style сохраняет гибкую настройку остальных объектов.
6. Категории ESP остаются независимыми от aim relation filters.

## Этап 3 — отношения и ресурсы

1. Allies показывает `Tribe Name [Team ID]`, а не голый ID.
2. Название выбирается с приоритетом character tribe, затем structure owner fallback.
3. Manual allies сохраняются и применяются одинаково к ESP, aim, radar и chams.
4. Armor/weapon atlas сохраняет исходный RGB и alpha.
5. Durability использует плавный green → yellow → orange → red → gray gradient.
6. Неподдерживаемые предметы получают fallback icon вместо пустого белого квадрата.

## Этап 4 — Alerts из ARK Multi Tool

Статус: реализован baseline, требуется runtime-проверка в ARK.

Alerts полностью локальные, неблокирующие и привязаны к текущему `UWorld generation`.

Обязательные события:

- новый вражеский игрок в snapshot;
- вражеский игрок быстро приближается;
- игрок оглушён или заснул;
- обнаружена смерть игрока;
- Noglin вошёл в настраиваемый радиус;
- активная/нацеленная вражеская turret угроза;
- резкое появление нескольких врагов рядом.

Настройки:

- master switch и отдельный checkbox для каждого типа;
- distance/radius, approach speed, cooldown и lifetime;
- максимум четыре уведомления одновременно;
- история последних событий без дубликат-спама;
- цвет, позиция, компактный режим и optional sound;
- очистка истории на reconnect или ручной кнопкой.

Критерий: alerts никогда не вызывают world scan из Present, не держат game thread и
не используют actor addresses из предыдущего мира.

## Этап 5 — оставшийся ESP/Misc parity с Multi Tool

Высокий приоритет:

- grouped off-screen arrows с количеством по направлению;
- structure/player/dino summary panels;
- smart declutter только для статических structures, никогда для players/dinos/turrets;
- dynamic name/class/tribe search и hidden tribe list;
- sortable turret table: distance, name, tribe, ammo, targeting, state, power;
- turret target-mode filter и отдельные total/detail budgets;
- compact threat dashboard;
- local weapon-view offset с полным rollback при ADS/weapon change/unload;
- actor hysteresis и короткая screen-space extrapolation без 30 Hz stepping.

Средний приоритет:

- Battle Mode preset для players/tamed dinos/turrets;
- separate distance/LOD для drops, structures и turrets;
- sleeping/dead filters;
- detected class/tribe history;
- ESP presets, import/export и reset одной категории;
- radar shapes, rotation mode, zoom и draggable position;
- color editor для каждой relation/category;
- profiler: CPU scan, render, actor age, budgets и dropped candidates.

## Этап 6 — будущие функции

Кандидаты после полной стабильности основных этапов:

- target priority: distance, angular error, visibility, threat, health;
- prediction с configurable velocity/gravity без изменения обычного smoothing;
- independent mounted aim tuning и rider/dino target selection;
- freecam bookmarks и плавные camera paths;
- multiple saved menu layouts и UI scale;
- themes, custom accent и accessibility contrast presets;
- searchable command palette для всех настроек;

## Этап 6.5 — производные функции на уже собираемых данных

Общий принцип этапа: ни одна из функций не требует новых офсетов или реверса. Всё
строится на данных, которые снапшот уже читает каждый цикл, но выбрасывает.

### 0. Замер-гейт (перед 5, кода нет)

`read_horde_details` уже перебирает варианты имени свойства инвентаря и пишет в лог
`prop= off= inv= items= count=` по каждому крейту. Нужно подойти к активному OSD и к
чужому контейнеру и прочитать лог.

- Если содержимое удалённых контейнеров реплицируется клиенту — пункты 5 и полный
  лут OSD делаются на готовой машинерии.
- Если нет — обе функции честно снимаются с плана, а не изображаются заглушкой.

Критерий: решение принято по строкам лога, а не по предположению.

### 1. Общее хранилище истории актёров (фундамент для 2, 3, 4)

Кольцевой буфер сэмплов на актёра: время, позиция, торпор, здоровье.

- Ключ — адрес актёра; полная очистка при смене `world_generation`.
- История ведётся **не для всех** актёров: игроки и объекты с ненулевым торпором.
  Вести её для 7000+ актёров бессмысленно по памяти и по времени кадра.
- Записи протухают по `stale_seconds` вместе с самим актёром.
- Критерий: `esp_ms` и `runtime_ms` в диагностике не выросли по сравнению с базой,
  снятой до включения истории.

### 2. Таймер пробуждения по торпору

- Скорость падения торпора считается по **наблюдаемым** сэмплам, а не по формуле:
  реальный темп зависит от вида, уровня и еды и учитывается сам собой.
- Показывается оценка времени до пробуждения на боксе существа.
- Если торпор не падает (существо подкармливают), показывать «стабилен», а не
  бесконечность и не выдуманное число.
- Критерий: на реально сбитом существе оценка сходится к факту с ошибкой в пределах
  темпа сэмплирования.

### 3. Следы перемещения игроков

- Последние N позиций игрока рисуются затухающим следом.
- Отвечает на вопрос «с какой стороны заходили», дополняя журнал контактов.
- Подчиняется общему правилу анти-клаттера: количество точек и длина следа
  настраиваются, след не должен превращаться в кашу на людной базе.

### 4. Журнал контактов

- Alerts уже детектируют появление, приближение, сон и смерть; журнал сохраняет эти
  события: время, имя, племя, дистанция, направление.
- Пишется на диск, чтобы переживать сессию: смысл функции — вернуться к компьютеру и
  увидеть, кто подходил.
- Критерий: события переживают reconnect и перезапуск игры.

### 5. Разведка контейнеров (зависит от пункта 0)

- Тот же путь чтения инвентаря, что и у крейтов OSD, применяется к
  `PrimalStructureItemContainer`.
- Подсветка контейнеров, в которых есть ценное, вместо обхода каждого вручную.

### 6. Приоритет цели по эффективному HP

- `armor_ratios` уже читается и рисуется иконками, но не влияет ни на одно решение.
- Новый режим приоритета аима: здоровье с поправкой на фактически разбитую броню.
- Существующие режимы приоритета не трогаются, добавляется ещё один.

### 7. Автопереключение профиля по карте

- Профили уже есть, но выбираются вручную; настройки под PvE и PvP различаются.
- Привязка профиля к имени карты и переключение при загрузке мира.
- Предварительно проверить, доступно ли имя карты в снапшоте; если нет — пункт
  переносится в backlog, а не решается угадыванием.

## Этап 6.6 — ручные отчёты о базах и контактах (клиент → бекенд → DS/TG)

Из обсуждения Kqinks / Dee@ от 06.09.2026. Здесь зафиксирован **клиентский
контракт**: что софт обязан уметь до того, как за это возьмётся бекенд.
Порядок принят явно: «для этого нужно решить это всё на клиенте и потом
сделать на бекенд, чтобы передавать всё в нормальном формате».

### Почему не автоматическая запись всех построек

Отвергнуто на этапе идеи, две причины:

1. **Актуальность.** 500 фундаментов от зарейженной базы будут висеть на карте
   как «база», хотя там давно ничего нет. Отметка о находке без отметки о
   состоянии — мусор, который со временем только накапливается.
2. **Объём.** Постоянная запись каждой структуры со своим идентификатором за
   день-два скаута даёт объём, который никто не собирается хранить и
   валидировать.

Поэтому базовый поток — **ручной**: игрок сам решает, что является находкой,
и сам её отправляет. Автоматика остаётся только на лайв-алертах.

### Что отправляет клиент

Панель в меню, отдельно от лайв-алертов:

- тип находки: `base` / `fob` / `stash` (кнопки, не свободный ввод);
- трайб — выбором из списка уже наблюдаемых владельцев, а не набором руками;
- координаты: мировые UE-координаты переводятся в игровые (те, что видит
  игрок на карте) **плюс высота Y** — без неё отчёт о базе в пещере или на
  платформе бесполезен;
- structure summary — агрегат по типам, а не список объектов
  (`1 tek gen`, `1 force field`, ...);
- таймстамп и произвольный комментарий.

Формат уведомления, которое из этого собирает бот:

```
Player X found a new base/fob/stash
Map:
Cords: X Y Z
Structure summary:
  1 tek gen
  1 force field
Time stamp:
<комментарий>
```

Для контакта с игроком — тот же конверт: кто задетектил, на какой карте,
координаты, когда, кого.

### Состояние находки

Профиль базы редактируется после создания, отметками с временными штампами:
`зарейжено` / `пусто` / `неактуально` / `было актуально`. В Discord отметки
показываются в часовом поясе читателя.

### Открытые вопросы (решаются на бекенде, не на клиенте)

Зафиксированы как есть — консенсуса в обсуждении не было:

- **Идентичность постройки.** У фундамента нет игрового ID; Dee@ считает хеш
  от класса и координат. Находка создаётся один раз, последующие детекты
  обновляют время последнего наблюдения; не обновилась при следующем скауте —
  считаем сломанной. Kqinks: постоянная валидация каждой постройки (а не
  группы) — нагрузка, особенно когда пропадает не 100 из 500, а вся база, и
  когда скаутят одновременно несколько человек. Решено замерять, а не спорить.
- **Дедуп нескольких репортёров в одной зоне.** Отбрасывать всех кроме одного
  нельзя: один игрок физически не грузит базу целиком (пример — база в пустыне
  на Extinction), двое с разных сторон видят разное. Предложенные варианты —
  параметр радиуса «рядом» и дедуп по одинаковым хешам на стороне бекенда.
- **Стакнутые структуры** делят координаты. Dee@ считает это несущественным:
  достаточно факта «по этим координатам стоит фундамент».
- **Косвенное подтверждение сноса**: зная координаты скаутившего аккаунта,
  можно сказать «человек пролетел, постройка не обновилась — её нет».

### Клиентские задачи по порядку

1. Панель отчёта в меню: тип, выбор трайба из наблюдаемых, комментарий, отправка.
2. Перевод мировых координат в игровые (lat/lon карты) + высота.
3. Агрегация structure summary по типам в радиусе находки.
4. Ручная отправка контакта игрока тем же конвертом.
5. Локальная очередь отчётов с ретраем — отправка не должна работать из Present.

Лайв-алерты остаются локальными; журнал событий за 60 минут уже сделан
(Этап 6.5, пункт 4) и служит источником для пункта 4 этого этапа.

## Competitive feature backlog

Публичные продукты вокруг ARK обычно продают не только отдельные игровые функции,
но и цельный путь использования. Из полезных идей в наш backlog входят:

- Safe Mode с понятной маркировкой функций по уровню риска;
- быстрые presets `Everyday`, `Raid`, `Performance`, `Streamer`, `Debug`;
- hard lock, configurable hitboxes и projectile/ping prediction profiles;
- trigger/activation profiles без смешивания с ESP relation settings;
- turret ESP с ammo, power, range и targeting filters;
- generator fuel/time telemetry;
- отдельные beds, caches, bags и selected resource categories;
- one-click update с changelog, rollback и known-issues status;
- dashboard состояния продукта, loader, target process, profile и backend;
- cloud config sync с локальным offline fallback;
- feature-level compatibility: несовпавший символ выключает только зависимую функцию;
- quick diagnostics bundle для воспроизводимого crash report;
- onboarding, который не заставляет пользователя вручную искать DLL/process/offsets.

Не копируем маркетинговые обещания вроде `100% undetected`. Loader показывает
фактический статус версии, проверок и backend, а неизвестное состояние обозначает
как неизвестное.

## Этап 7 — KOPT Loader + Injector

Полная архитектура записана в `docs/LOADER_ARCHITECTURE.md`.

Рекомендуемый UI stack: C++20 backend + Qt 6/QML frontend. Он позволяет сохранить
один визуальный код для Windows и Linux, GPU-composited animations, high-DPI и
отделить UI от platform-specific injection backend. Существующий CLI injector
остаётся диагностическим fallback.

Visual direction: фиолетово-чёрная система, глубокие нейтрально-чёрные поверхности,
фиолетовый accent и semantic status colors. Основной шрифт — `Segoe UI Variable`,
fallback — `Segoe UI`; обе гарнитуры совместимы с кириллицей и Windows DPI.

Обязательные возможности loader:

- borderless, no-console окно с нормальным taskbar/tray lifecycle;
- Login, Dashboard, Product, Injection, Profiles, Diagnostics и Settings screens;
- плавные 180–240 ms transitions, skeleton loading и progress timeline;
- reduced-motion mode, keyboard navigation, DPI 100–250% и корректный resize;
- encrypted session storage через Windows DPAPI или Linux Secret Service;
- access/refresh tokens, expiry, revoke, offline grace и session conflict UI;
- server challenge, signed entitlement и отсутствие постоянного пароля в памяти;
- Ed25519-signed manifest, SHA-256 payload verification и atomic rollback update;
- точная проверка SHA-256/PE/PDB profile целевого ShooterGame.exe;
- автоматическое обнаружение target process с явным version/status result;
- standard LoadLibrary backend для разработки;
- полноценный x64 manual-map backend с imports, relocations, TLS callbacks,
  x64 unwind/exception tables, final section protections и unload contract;
- optional memory-only payload delivery после успешной авторизации;
- минимизация в tray/auto-hide после успешной загрузки без потери status controls;
- отдельный Proton/Wine path с проверкой prefix и запуском Linux payload workflow;
- structured local log с redaction токенов, HWID и memory addresses;
- crash-safe loader: ошибка payload не должна уронить UI или оставить зависший helper;
- clear status: `Idle → Validating → Waiting for ARK → Mapping → Handshake → Ready`;
- отмена до Mapping и безопасная выгрузка после Handshake;
- анимированный, но функциональный UI без декоративных задержек рабочего процесса.

Критерии готовности loader:

1. Авторизация, refresh, revoke, network failure и offline grace проверены тестами.
2. Повреждённый/подменённый manifest или payload никогда не запускается.
3. Неподходящая версия ShooterGame блокируется до injection с точной причиной.
4. LoadLibrary и manual-map проходят одинаковый payload handshake/self-test.
5. Loader переживает launch/close/retry/inject/unload минимум 50 циклов.
6. UI держит плавный frame pacing при network/update/injection operations.
7. Никакой сетевой или injection operation не блокирует UI thread.

## Этап 8 — slotted backend и admin panel

Статус: реализован backend/admin baseline, production deploy и loader integration
остаются отдельными задачами следующего прохода.

Полная модель записана в `docs/COMMERCIAL_BACKEND.md`. Backend является
server-authoritative: клиент не может сам продлить подписку, освободить HWID-slot,
выдать entitlement или получить payload key без активной короткоживущей lease.

Обязательная область:

- пользователи, роли, продукты, ключи, подписки, сроки и slot limits;
- device/HWID enrollment, reset cooldown, ручной reset и revoke;
- продление/сокращение/пауза подписки с причиной и audit trail;
- session list, принудительный logout, ban/unban и emergency product stop;
- release channels, signed manifests, minimum version и rollback;
- tamper incident queue, evidence summary, risk score и ручное решение;
- RBAC, MFA для администраторов и append-only audit log;
- dashboard состояния backend, loader, target profile и active leases.

Tamper response не удаляет пользовательские файлы. Он отзывает lease/refresh token,
очищает только собственные секреты из памяти, останавливает payload, помещает аккаунт
или устройство в quarantine и блокирует повторную выдачу capability до решения.

## Производительность и качество — обязательные инварианты

- Player/dino visual quality не понижается адаптивной оптимизацией.
- Ограничения применяются только к дальним static structures и detail-heavy turrets.
- Present не выполняет полный actor discovery и не сортирует весь мир.
- Fast path обновляет позиции/кости; slow path классифицирует и открывает новые actors.
- Никакой функции нельзя маскировать как live, если она требует restart.
- Любая настройка, меняющая игру, имеет rollback и восстанавливается при unload.
- Disable должен реально прекращать работу функции, а не только скрывать результат.

## Порядок проверки релиз-кандидата

1. Release build и PE x64 self-test.
2. Config migration: legacy settings → versioned local defaults.
3. Menu input test: Home, cursor, 100 checkbox clicks, combos, sliders, drag-and-drop.
4. Aim test: foot/mounted, Hold/Toggle/Always, Alt/Ctrl/mouse binds, lock release.
5. ESP test: relation independence, Preview isolation, icons, radar, grouping, alerts.
6. Chams test: enable/disable, death/respawn, teleport, disconnect/reconnect.
7. Panic → restore, unload → reinject.
8. Пять последовательных reconnect без crash.

Только после прохождения всех применимых пунктов сборка переносится в `dist` как
основной `kopt_payload.dll`.
