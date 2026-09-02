#pragma once

// Настоящий транспорт для kopt::Publisher (publisher.hpp): голый QUIC
// (ngtcp2 + ngtcp2_crypto_ossl + OpenSSL 3.6, все три кросс-собраны под
// x86_64-w64-mingw32 -- см. scripts/fetch-quic-deps.sh) поверх UDP,
// говорящий тем же самым протоколом, что уже реализован и end-to-end
// проверен на стороне Go (backend/backend_go/internal/quicserver):
// ALPN "ark-quic-v1", один двунаправленный стрим на соединение,
// 4-байтный big-endian префикс длины перед каждым JSON-сообщением,
// JSON-хендшейк {token, group_id, server_ip} первым сообщением.
//
// Класс называется Http3Publisher по инерции от исходного плана (там
// предполагался HTTP/3) -- сам транспорт HTTP-семантики не несёт вообще,
// переименовывать сейчас смысла нет: имя уже используется в publisher.hpp
// (g_publisher's doc comment) и CMakeLists.txt.
//
// pImpl нарочно: ngtcp2/openssl-заголовки не должны протекать в
// payload.cpp дальше этого одного .cpp (единственное место, которое их
// реально включает).

#include "kopt/publisher.hpp"

#include <memory>

namespace kopt
{
    class Http3Publisher final : public Publisher
    {
    public:
        Http3Publisher();
        ~Http3Publisher() override;

        Http3Publisher(const Http3Publisher&) = delete;
        Http3Publisher& operator=(const Http3Publisher&) = delete;

        void start(std::wstring endpoint, std::wstring token,
            std::wstring group_id, std::wstring server_ip) override;
        void stop() override;
        void submit_sightings(std::vector<share::Sighting> batch,
            std::vector<std::wstring> vanished) override;
        void submit_notifications(std::vector<share::Notification> batch) override;
        void subscribe(std::function<void(share::RemoteBatch)> on_batch) override;
        [[nodiscard]] bool connected() const noexcept override;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
