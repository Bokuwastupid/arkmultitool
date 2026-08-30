# KOPT Loader architecture

## 1. Назначение

KOPT Loader — отдельное desktop-приложение, которое авторизует пользователя,
проверяет entitlement и совместимость игры, получает проверенный payload, выбирает
platform backend, выполняет загрузку и показывает полноценный lifecycle. UI не
содержит injection-логики и не хранит серверные secrets.

## 2. Компоненты

```text
Avalonia/.NET 10 UI
   │ commands + immutable view state
Application coordinator
   ├── Auth + entitlement client
   ├── Seat/device lease client
   ├── Product/status client
   ├── Manifest/update verifier
   ├── Game/profile detector
   ├── Versioned profile store
   └── Injection coordinator
          ├── LoadLibrary backend
          ├── Manual-map x64 backend
          └── Proton/Wine backend
```

UI и coordinator работают без elevation. Если конкретному backend нужны другие
права, запускается короткоживущий helper с минимальным IPC-протоколом и привязкой к
одной signed injection job.

## 3. Экраны

### Splash / bootstrap

- локальная integrity-проверка;
- загрузка последней безопасной сессии;
- product status и update manifest;
- переход не дольше необходимого, без искусственного progress.

### Login

- email/username + password или device-code flow;
- show/hide password, Caps Lock indicator, validation и recovery link;
- rate-limit/cooldown отображается явно;
- пароль очищается сразу после обмена на tokens.

### Dashboard

- карточка ASE с build hash, payload version и service state;
- состояние лицензии и оставшееся время;
- кнопка `Launch / Inject` меняет название по фактическому состоянию;
- last changelog, warnings и known issues;
- быстрый preset и Safe Mode.

### Injection progress

- timeline `Validate game → Verify payload → Prepare backend → Map → Handshake`;
- точная текущая операция, elapsed time и cancel availability;
- ошибки имеют stable code, human description и copyable diagnostics ID;
- success плавно сворачивает loader в tray, если это включено.

### Profiles

- Everyday, Raid, Performance, Streamer и пользовательские presets;
- diff до применения, import/export и reset category;
- несовместимые settings отмечаются, а не молча удаляются.

### Diagnostics / Settings

- target path/hash/version, renderer, backend, payload handshake и logs;
- theme, accent, language, UI scale, reduced motion и tray behavior;
- token/HWID/addresses всегда redacted.

## 4. Visual system

- фиолетово-чёрная база: `#09070D` background, `#120E1A` surfaces,
  `#8B5CF6` primary accent и semantic status colors;
- `Segoe UI Variable` как основной шрифт, `Segoe UI` как совместимый fallback;
- 8 px spacing grid, 10–14 px radii, predictable typography hierarchy;
- transitions 180–240 ms, ease-out for appearance и ease-in-out for layout;
- animation не задерживает click handling и не блокирует navigation;
- GPU composition, frame-coalescing и отсутствие busy animation loop в tray;
- skeleton states только для реальной async loading operation;
- high contrast, focus rings, tooltips и keyboard navigation;
- reduced-motion заменяет movement на короткий opacity transition.

## 5. Authentication

- TLS API; клиент не содержит master/API secrets;
- access token короткий, refresh token rotating и revocable;
- refresh хранится через DPAPI на Windows или Secret Service на Linux;
- device binding использует privacy-preserving server challenge, а не открытый dump HWID;
- entitlement подписан сервером и содержит product, expiry, channel и allowed profile;
- clock rollback не даёт бессрочный offline mode;
- offline grace — отдельное подписанное разрешение с ограниченным сроком;
- logout/revoke очищает memory и credential store;
- loader никогда не пишет tokens в logs/crash dumps.

## 6. Manifest и updates

Manifest содержит channel, loader/payload version, target SHA-256, minimum loader,
payload SHA-256, size, compatibility features и rollback artifact. Manifest
проверяется встроенным Ed25519 public key до разбора доверенных полей. Загрузка идёт
во временный файл, затем hash/signature check и atomic replace. Последняя рабочая
версия хранится для rollback.

## 7. Injection backends

### LoadLibrary development backend

- точный target PID и architecture check;
- absolute payload path;
- remote allocation/write + loader call;
- подтверждение не по завершению thread, а через payload handshake;
- безопасный `KoptRequestUnload` для выгрузки.

### Manual-map x64 backend

- строгий PE32+ parser с bounds checks;
- image allocation и копирование headers/sections;
- base relocations (`DIR64`), imports и delay imports;
- TLS callbacks в правильном порядке;
- регистрация x64 function table для SEH/unwind;
- final RX/RW/R protections и discard ненужных sections;
- вызов entry point через небольшой versioned bootstrap context;
- двусторонний handshake с build/profile IDs;
- unload contract вызывает payload cleanup до удаления function table/image;
- любое частичное падение выполняет rollback только уже созданных ресурсов.

Memory-only delivery не отменяет PE validation и signature verification. Скрытый
режим означает отсутствие console/temp clutter, минимальный helper lifecycle,
автосворачивание и аккуратный in-memory mapping; это не обещание неуязвимости или
гарантированной невидимости.

### Proton/Wine backend

- обнаружение Steam compatdata prefix;
- проверка Wine/Proton process и Windows path translation;
- совместимый launch/attach workflow;
- DXVK status и отдельная диагностика hook initialization;
- никакой Windows elevation логики на Linux path.

## 8. IPC и payload handshake

Каждая операция получает случайный job ID и одноразовый nonce. Helper принимает
только signed job с target PID, expected target hash, payload hash и expiry. Payload
после загрузки публикует protocol version, build ID, target profile и feature mask.
Loader показывает Ready только после совпадения всех полей.

## 9. Ошибки и восстановление

- retry никогда не повторяет уже подтверждённую destructive phase вслепую;
- target exit отменяет job и очищает временные ресурсы;
- network loss после verified payload не повреждает текущую injection operation;
- helper timeout завершается с диагностикой, но loader остаётся жив;
- reconnect в игре не запускает повторную injection автоматически;
- crash report связывается с loader/payload/target build IDs.

## 10. Реализация по этапам

1. Вынести существующий CLI injector в интерфейс `IInjectionBackend`.
2. Добавить deterministic target/profile verifier и payload handshake.
3. Создать Avalonia shell, state machine и реальные bounded service clients.
4. Подключить LoadLibrary backend и локальный unsigned development channel.
5. Реализовать manifest signature/update/rollback.
6. Подключить auth API и secure credential storage.
7. Реализовать manual-map backend и его failure-injection tests.
8. Добавить tray, animations, themes, accessibility и localization.
9. Добавить Proton path.
10. Прогнать lifecycle, high-DPI, network failure, update rollback и 50-cycle tests.
