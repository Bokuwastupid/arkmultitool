# KOPT commercial backend

## Текущий implementation baseline

Рабочий baseline находится в `backend/Kopt.ControlPlane`: ASP.NET Core 10,
PostgreSQL/EF Core migrations, Identity bearer auth, Admin RBAC, signed ECDSA
capability leases и фиолетово-чёрная web-панель. Реализованы выпуск/revoke ключей,
увеличение/уменьшение/revoke подписки, HWID reset/revoke, force logout,
quarantine/release, tamper queue, emergency product stop и audit trail.

`backend/compose.yaml` поднимает backend с PostgreSQL, а
`scripts/generate-backend-secrets.ps1` создаёт pepper, пароль БД, bootstrap admin
и ECDSA P-256 keypair. Production startup отклоняет placeholder/слабые секреты.

## Цель

Backend обслуживает slotted/sell-вариант продукта: идентификацию, подписки,
entitlements, device slots, выдачу краткоживущих capability, релизы и операции
администратора. Все решения о доступе принимаются сервером; loader считается
недоверенным клиентом.

## Stack и границы

- ASP.NET Core 10 Minimal APIs с route groups и единым Problem Details contract;
- EF Core + PostgreSQL как источник истины;
- Redis только для rate limits, distributed leases и коротких revocation caches;
- отдельный Qt/QML loader и web admin panel на одном API contract;
- object storage/CDN для зашифрованных payload packages;
- signing keys в KMS/HSM, secrets не хранятся в репозитории или desktop-клиенте.

Доменные сущности: `User`, `AdminRole`, `Product`, `LicenseKey`, `Subscription`,
`Entitlement`, `Device`, `Seat`, `Lease`, `Session`, `Release`, `Manifest`,
`Revocation`, `TamperIncident`, `AuditEvent`.

## Slotted access model

1. Ключ активирует entitlement с датой начала/окончания и числом slots.
2. Устройство проходит enrollment и занимает конкретный slot.
3. Loader получает short-lived signed lease, привязанный к user/product/device,
   release channel, nonce и session ID.
4. Lease обновляется с jitter; истечение или revoke останавливает новые privileged
   operations и переводит payload в controlled shutdown.
5. Device reset имеет cooldown, лимит и причину; администратор может сделать ручной
   reset, revoke или перенос с обязательным audit event.
6. Offline grace ограничен временем и заранее подписанным scope; он не позволяет
   активировать новый ключ, сменить устройство или получить новый release key.

## Admin panel

Экраны:

- Overview: active users/leases, expiring subscriptions, incidents, release health;
- Users: поиск, роли, status, devices, sessions, incidents и полная timeline;
- Keys: batch generation, product/tier, duration, slots, redemption и revoke;
- Subscriptions: extend, shorten, pause, resume, cancel и history;
- Devices/HWID: fingerprints, occupied slots, reset cooldown, reset/revoke/ban;
- Sessions: active leases, forced logout, token-family revoke и diagnostics ID;
- Releases: channels, manifests, rollout %, minimum version, rollback и kill switch;
- Tamper: incident evidence, risk score, correlated accounts/devices и resolution;
- Audit: actor, action, target, before/after diff, reason, IP/session и timestamp;
- Settings: RBAC policies, MFA enforcement, rate limits and webhook secrets.

Опасные действия используют typed confirmation, reason field и step-up MFA.
Нельзя удалить audit trail, незаметно изменить срок или выдать ключ без записи.

## Auth и API

Route groups:

- `/api/auth/*`: login, refresh rotation, logout, recovery, MFA;
- `/api/entitlements/*`: status и capability scopes;
- `/api/devices/*`: enrollment и slot state;
- `/api/leases/*`: acquire, renew, release;
- `/api/releases/*`: signed manifest, payload ticket, changelog;
- `/api/admin/*`: user/key/subscription/device/release/incident operations;
- `/api/webhooks/*`: idempotent billing events с signature validation.

Access tokens живут минуты, refresh tokens rotating и хранятся hashed. Reuse одного
refresh token отзывает всю token family. Admin endpoints требуют policy-based RBAC,
MFA claims, CSRF-защиту для browser session и строгий rate limit.

## Tamper и anti-crack

Абсолютной защиты клиентского кода не существует, поэтому защита многослойная:

- подписанный manifest и проверка хэша/версии до запуска;
- зашифрованный payload key выдаётся только под действующую device lease;
- персонализированный session watermark для корреляции утечки;
- integrity/tamper sensors с debounce и server-side risk scoring;
- критические entitlement/release решения не дублируются локальным boolean;
- feature flags, version floor, account/device quarantine и emergency stop;
- obfuscation и delayed checks используются только как дополнительный слой;
- loader очищает собственные токены/ключи из памяти при revoke или tamper.

Tamper response никогда не стирает чужие файлы и не повреждает ОС. Реакция:
controlled shutdown, token/lease revoke, секреты из памяти, quarantine, incident ID,
сохранение минимальной redacted-телеметрии и блокировка повторной выдачи capability.
Это даёт стоп-кран без риска уничтожения данных при ложном срабатывании.

## Privacy и HWID

Fingerprint строится из нескольких нормализованных сигналов, каждый компонент
salted/hashed; raw serials не сохраняются. UI показывает пользователю устройства и
last-seen. Retention, экспорт и удаление данных отделены от неизменяемого security
audit в пределах применимых требований.

## Надёжность и тесты

- миграции БД, optimistic concurrency и idempotency keys для mutation endpoints;
- health/readiness checks, structured logs, metrics, tracing и alerting;
- encrypted backups и регулярно проверяемое restore;
- unit tests для entitlement/slot/time policies;
- integration tests PostgreSQL/Redis/auth/manifest/webhooks;
- end-to-end tests admin extend/revoke/reset/rollback/emergency stop;
- clock-skew, replay, duplicate webhook, token reuse и partial outage scenarios;
- tamper false-positive tests: один sensor не приводит к автоматическому ban.

## Следующая очерёдность реализации

1. Signed release manifests, rollout, rollback и loader handshake.
2. Полный внешний MFA provider и step-up для критических операций.
3. Billing adapter и idempotent signed webhooks.
4. Redis revocation cache и горизонтальное масштабирование lease service.
5. Cloud profile sync и operational hardening.
