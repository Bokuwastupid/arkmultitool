# KOPT roadmap

## Правила релиза

- Основная DLL не заменяется, пока кандидат не прошёл build, static validation и runtime smoke в точной версии ARK.
- Новая оптимизация не имеет права менять player/dino/turret ESP. Любой LOD/declutter допускается только как явная настройка и только для статических построек.
- Aim и ESP используют независимые relation settings. Menu click, held key и старое состояние ввода не могут активировать aim.
- Reconnect/travel инвалидирует world-scoped pointers; старые chams/components никогда не восстанавливаются через engine calls.
- Game INI/CVar editor исключён из проекта до отдельного решения.

## Этап 1 — stability gate

- [x] Fresh-click обработка checkbox/button и эксклюзивный drag capture slider.
- [x] Cursor unclip/show на каждом кадре открытого меню и reset при focus loss.
- [x] Left/Right Alt, Ctrl и Shift в rebind; отдельные bind для menu, unload, aim, freecam, ESP и panic.
- [x] Fresh activation aim после включения функции, смены bind/mode и закрытия меню.
- [x] Aim через PDB-validated `SetControlRotation`, без raw write в соседние input fields.
- [x] World-generation guard, stabilization delay и abandon-only path для chams на reconnect.
- [ ] 20-cycle runtime smoke: open/close menu, rebind, reconnect, unload/reinject.

## Этап 2 — menu и ESP

- [x] Фиолетово-чёрная draggable UI, dropdowns, toggles, checkbox, captured sliders.
- [x] Отдельное draggable Player ESP Preview с drag-and-drop anchors; world settings не меняются.
- [x] Отдельные player/world box, label, health и torpor styles.
- [x] Полные embedded armor/weapon icon atlases с сохранением RGB.
- [x] Матрица OWN/ALLY/ENEMY отдельно для players, tamed dinos и structures.
- [x] Alliance UI показывает tribe name и ID; manual allies сохраняются по team ID.
- [x] Live Search и hidden lists для tribes, dino types и structure types.
- [x] Sleeping/dead filters, Battle Mode, summaries, radar, offscreen markers и static-only declutter.
- [x] Turret ammo/state/power/range/target/warning controls и target-mode filter.
- [x] Threat panel и alerts, перенесённые из Multi Tool.
- [ ] Runtime visual QA на 1080p/1440p/4K и high-DPI.

## Этап 3 — aim и camera

- [x] Target priority: angle, distance, health и balanced.
- [x] Configurable hit zones, FOV, distance и frame-rate-independent smoothing.
- [x] Velocity/gravity/latency prediction без изменения обычного smoothing.
- [x] Mounted controller route, independent mounted FOV/smoothing и mounted aim lock.
- [x] Freecam Raw Input, independent rotation, sensitivity, smoothing, speed multipliers и lossless rollback.
- [ ] Runtime calibration profiles для конкретных projectile classes и mounted camera variants.

## Этап 4 — commercial control plane

- [x] ASP.NET Core/PostgreSQL baseline, Identity auth, Admin RBAC и signed capability leases.
- [x] Admin UI: keys, subscription time adjustment/revoke, HWID/device reset/revoke, sessions, quarantine и releases.
- [x] Signed manifest metadata, rollout/rollback controls, emergency product stop и immutable audit events.
- [x] Tamper response: capability/lease revoke, controlled shutdown, memory secret wipe и quarantine.
- [ ] MFA/step-up provider, billing webhooks, Redis revocation cache и production observability.

Tamper detection не удаляет файлы пользователя и не повреждает ОС. Стоп-кран реализуется серверным revoke/quarantine и controlled shutdown: это устойчивее к false positive и оставляет проверяемый audit trail.

## Этап 5 — loader

- [x] Avalonia/.NET 10 shell: purple/black design, compatible bundled font и non-blocking busy states.
- [x] Login, entitlement/device lease, remaining subscription time, exact target status и diagnostics ID.
- [ ] Signed manifest verification, encrypted payload delivery, changelog, rollback и known-issues state.
- [ ] Versioned injection coordinator: development LoadLibrary готов; tested x64 manual-map и Proton/Wine backend ещё нужны.
- [ ] Payload handshake, feature-level compatibility и controlled unload.

## Этап 6 — будущие функции

- target priority extensions: visibility and threat scoring;
- projectile prediction profiles per weapon/ammo while preserving normal smoothing;
- independent rider/dino target selection while mounted;
- freecam bookmarks and smooth camera paths;
- multiple saved menu layouts and UI scale;
- themes, custom accent and accessibility contrast presets;
- searchable command palette for every setting;
- quick presets: `Everyday`, `Raid`, `Performance`, `Streamer`, `Debug`;
- turret ESP telemetry: ammo, power, range and targeting filters;
- generator fuel/time telemetry;
- separate beds, caches, bags and selected resource categories;
- one-click update with changelog, rollback and known-issues status;
- dashboard for product, loader, target process, profile and backend state;
- cloud config sync with local offline fallback;
- feature-level compatibility so one mismatched symbol disables only its dependent feature;
- reproducible diagnostics bundle and guided onboarding.
