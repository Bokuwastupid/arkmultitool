#pragma once

// Транспорт-независимый контракт шеринга. Ничего здесь не знает про
// QUIC/HTTP-3, WinHTTP или что бы то ни было ещё -- реализация подставляется
// снаружи (kopt::Http3Publisher в проде, kopt::NoopPublisher, пока сетевой
// стек не собран или для CI-сборки без сетевых зависимостей).

#include "kopt/share.hpp"
#include "kopt/share_remote.hpp"

#include <functional>
#include <string>
#include <vector>

namespace kopt
{
    class Publisher
    {
    public:
        virtual ~Publisher() = default;

        // Возвращается сразу же -- само подключение происходит на фоновом
        // потоке реализации, тот же контракт, что и у RelayClient::start
        // сегодня.
        //
        // group_id/server_ip едут в JSON-хендшейке на ark_relay (см.
        // backend/backend_go/internal/quicserver/handshake.go) -- сервер
        // без них не пропустит SISMEMBER-проверку членства в группе.
        // Настоящего источника (аккаунт знает свои группы, клиент узнаёт
        // ip:port сервера при подключении к игре) пока нет -- см. config.hpp
        // Share-секция: до появления обоих оба поля читаются из
        // kopt_internal.ini как есть, тем же "launch parameter, не
        // аккаунт-система" принципом, что уже применён к самому токену.
        virtual void start(std::wstring endpoint, std::wstring token,
            std::wstring group_id, std::wstring server_ip) = 0;

        // Упорядоченная остановка: сигнал фоновым потокам, закрытие
        // соединения, join. Безопасно вызывать, даже если start() не
        // вызывался или соединение не поднялось.
        virtual void stop() = 0;

        // Тот же контракт, что у RelayClient::submit сегодня: заменяет
        // непереданную пачку, никогда не блокирует вызывающего (D3D11
        // Present hook) -- решение, когда реально отправлять, живёт на
        // фоновом потоке. vanished -- ключи целей, пропавших с прошлого
        // вызова (share::ChangeFilter::collect_vanished) -- едут в теле
        // того же запроса, что и sightings (см. JSON-контракт), поэтому
        // отдельного метода для них нет.
        virtual void submit_sightings(std::vector<share::Sighting> batch,
            std::vector<std::wstring> vanished) = 0;
        virtual void submit_notifications(std::vector<share::Notification> batch) = 0;

        // Приёмная сторона -- колбэк вызывается на фоновом read-потоке для
        // каждого целого батча от другого репортёра команды; вызывающий
        // (share::ReporterFilter + share::RemoteView) сам обеспечивает
        // потокобезопасность своей стороны, Publisher не блокируется на
        // обработке.
        virtual void subscribe(std::function<void(share::RemoteBatch)> on_batch) = 0;

        [[nodiscard]] virtual bool connected() const noexcept = 0;
    };

    class NoopPublisher final : public Publisher
    {
    public:
        void start(std::wstring, std::wstring, std::wstring, std::wstring) override {}
        void stop() override {}
        void submit_sightings(std::vector<share::Sighting>, std::vector<std::wstring>) override {}
        void submit_notifications(std::vector<share::Notification>) override {}
        void subscribe(std::function<void(share::RemoteBatch)>) override {}
        [[nodiscard]] bool connected() const noexcept override { return false; }
    };
}
