#include "kopt/http3_publisher.hpp"

// Wire contract taken from the REAL Go source at the time this was written
// (backend/backend_go/internal/protocol/message.go,
// internal/quicserver/{frame,handshake,server}.go) -- not from memory.
// Any future change to that side must be mirrored here by hand; there is
// no shared schema between the two languages.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <openssl/ssl.h>
#include <openssl/rand.h>
#include <openssl/err.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>

using json = nlohmann::json;

namespace kopt
{
    namespace
    {
        constexpr const char* kAlpn = "ark-quic-v1";
        constexpr std::size_t kMaxFramePayload = 1 * 1024 * 1024; // generous vs. quicserver's maxBytes
        constexpr std::size_t kCidLen = 16;
        constexpr std::size_t kMaxUdpPayload = 1452; // conservative MTU-safe default, no PMTUD attempted

        // hub.Client (backend_go, shared by the WS and QUIC transports)
        // expects the CLIENT to be the one sending periodic app-level pings
        // -- its own writePump sends one every RELAY_PING_INTERVAL (server
        // default 30s) but that alone doesn't help: hub.Client's readPump
        // resets ITS OWN liveness deadline (RELAY_PONG_WAIT, default 60s)
        // only on something it RECEIVES, and this client never sent
        // anything back during a quiet scene (no sightings to report).
        // Confirmed live: a connection with zero outbound traffic died to
        // ngtcp2's own idle-timeout right around the 60s pongWait mark,
        // taking the whole publisher down with no reconnect (see the
        // worker-loop comment on Http3Publisher::start). 20s keeps a
        // comfortable margin under both the server's 30s ping cadence and
        // its 60s liveness window, and resets on any real send (sightings
        // count exactly like a ping server-side, see hub.Client.readPump),
        // so this only actually fires during genuine silence.
        constexpr auto kPingInterval = std::chrono::seconds(20);

        std::string build_ping_frame()
        {
            json message;
            message["type"] = "ping";
            return message.dump();
        }

        // --------------------------------------------------------------
        // Small self-contained helpers: this .cpp is the only place ngtcp2/
        // openssl headers exist in the project, and payload.cpp's own
        // log_line()/module_directory() are private to ITS anonymous
        // namespace -- duplicating these two trivial helpers here is
        // cheaper than exporting them just for this one caller.
        // --------------------------------------------------------------
        std::filesystem::path module_directory()
        {
            HMODULE this_module{};
            GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&module_directory), &this_module);
            std::wstring buffer(32768, L'\0');
            const DWORD length = GetModuleFileNameW(this_module, buffer.data(), static_cast<DWORD>(buffer.size()));
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }

        void log_line(const std::wstring& message)
        {
            static const std::filesystem::path log_path = module_directory() / L"kopt_internal.log";
            const HANDLE file = CreateFileW(log_path.c_str(), FILE_APPEND_DATA,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) return;
            SYSTEMTIME time{};
            GetLocalTime(&time);
            const std::wstring line = std::format(L"{:02}:{:02}:{:02}.{:03} [share] {}\r\n",
                time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, message);
            DWORD written{};
            WriteFile(file, line.data(), static_cast<DWORD>(line.size() * sizeof(wchar_t)), &written, nullptr);
            CloseHandle(file);
        }

        std::string to_utf8(const std::wstring& wide)
        {
            if (wide.empty()) return {};
            const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                nullptr, 0, nullptr, nullptr);
            if (size <= 0) return {};
            std::string out(static_cast<std::size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), size, nullptr, nullptr);
            return out;
        }

        std::wstring to_wide(const std::string& utf8)
        {
            if (utf8.empty()) return {};
            const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
            if (size <= 0) return {};
            std::wstring out(static_cast<std::size_t>(size), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), size);
            return out;
        }

        ngtcp2_tstamp now_ts()
        {
            return static_cast<ngtcp2_tstamp>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
        }

        // ----------------------------------------------------------------
        // Wire encoding: protocol.Category (message.go)
        // ----------------------------------------------------------------
        const char* category_string(share::Kind kind)
        {
            switch (kind)
            {
            case share::Kind::player: return "player";
            case share::Kind::dino: return "dino";
            case share::Kind::turret: return "turret";
            case share::Kind::structure: return "structure";
            case share::Kind::other: return "";
            }
            return "";
        }

        // protocol.Status (message.go) -- awake/sleeping/knocked_out/dead,
        // player-only; Sighting doesn't carry a PlayerEspState directly
        // (that's overlay-only classification), so this is re-derived from
        // the same has_torpor/sleeping/dead fields build_player() already
        // filled in, the same rule kopt::player_esp_state() (runtime.hpp)
        // uses -- kept independent rather than calling it, since that
        // function takes an Actor, not a Sighting DTO already stripped of
        // the fields it would need.
        const char* status_string(const share::Sighting& s)
        {
            if (s.kind != share::Kind::player) return "";
            if (s.dead) return "dead";
            if (s.has_torpor && s.max_torpor > 0.0F && (s.torpor / s.max_torpor) >= 0.95F) return "knocked_out";
            if (s.sleeping) return "sleeping";
            return "awake";
        }

        // Player/dino need a client-computed Key (protocol.Entity.Validate
        // requires it non-empty for every category, and hub.go's
        // fillComputedKeys only ever fills it in for structure/turret --
        // see that function's doc comment, quoted here because it is the
        // actual normative source): "{cat}:{label}:{team}". Structure/
        // turret leave Key empty on purpose -- the server derives it from
        // class+position (internal/dedup.Key), a stable identity across
        // reloads that this client has no way to reproduce independently.
        std::string entity_key(const share::Sighting& s)
        {
            if (s.kind == share::Kind::structure || s.kind == share::Kind::turret) return {};
            return std::string(category_string(s.kind)) + ":" + to_utf8(s.label) + ":" + std::to_string(s.team);
        }

        json turret_json(const share::TurretInfo& t)
        {
            json j;
            if (t.ammo >= 0) j["ammo"] = t.ammo;
            j["range"] = t.range;
            j["targeting"] = t.targeting;
            j["warning"] = t.warning;
            if (t.powered) j["powered"] = true;
            if (t.active) j["active"] = true;
            if (t.targeting_actor) j["targeting_actor"] = true;
            return j;
        }

        // Mirrors protocol.Entity exactly (field names, field presence
        // rules) -- see Entity.Validate in message.go for what the server
        // actually enforces: Key always required, ClassName required for
        // structure/turret, Label required otherwise, Turret required (non
        // -null) for turret.
        json entity_json(const share::Sighting& s)
        {
            json j;
            j["v"] = s.schema_version;
            j["key"] = entity_key(s);
            j["cat"] = category_string(s.kind);
            j["team"] = s.team;
            if (!s.label.empty()) j["label"] = to_utf8(s.label);
            if (!s.class_name.empty()) j["class"] = to_utf8(s.class_name);
            j["x"] = s.x;
            j["y"] = s.y;
            j["z"] = s.z;
            if (!s.tribe.empty()) j["tribe"] = to_utf8(s.tribe);
            if (const char* status = status_string(s); status[0] != '\0') j["status"] = status;
            if (s.has_health)
            {
                j["health"] = s.health;
                j["max_health"] = s.max_health;
            }
            if (s.kind == share::Kind::turret && s.turret) j["turret"] = turret_json(*s.turret);
            // stable_id -- linked_player_data_id, only meaningful for
            // Kind::player (Sighting::stable_id's own doc comment: 0 for
            // dino/structure/turret, no such id exists for those). Feeds
            // protocol.Entity.StableID -> streamproducer.PlayerFields'
            // platform_id on the Go side (see its own doc comment for why
            // this, not a real Steam/platform id, is what's available).
            if (s.kind == share::Kind::player && s.stable_id != 0) j["stable_id"] = s.stable_id;
            // steam_id -- real SteamID64, only for Kind::player and only
            // when read_player_steam_id() (runtime.cpp) resolved one (0
            // when the owner disconnected, or the client hasn't hooked a
            // PlayerState-bearing pawn for this target yet). Distinct from
            // stable_id above: this is the real cross-platform identity,
            // stable_id is the ARK-internal reporter/dedup surrogate --
            // feeds protocol.Entity.SteamID on the Go side.
            if (s.kind == share::Kind::player && s.steam_id != 0) j["steam_id"] = s.steam_id;
            // tamed -- всегда для дино, даже false: это не опциональные
            // метаданные, а признак, по которому релей решает, писать ли
            // существо в durable-поток вообще (hub.maybeStream). Пропуск
            // поля и явный false на приёме значат одно и то же ("дикий"),
            // но явная передача снимает неоднозначность "старый клиент
            // против дикого существа" в логах.
            if (s.kind == share::Kind::dino) j["tamed"] = s.tamed;
            return j;
        }

        // protocol.Inbound{Type:"sighting", Seq, ReporterCharacterID,
        // ReporterX/Y/Z, Vanished, Entities}. ReporterCharacterID/position
        // are this client's own identity (ArkRuntime::Snapshot::
        // local_stable_id -- the ARK linked_player_data_id, already used
        // as kopt::share::Sighting::stable_id for player *targets*, same
        // value, different role here: identifying the reporter, not a
        // sighted actor) and position at submission time, sent whenever
        // known (reporter_stable_id != 0) so the receiving side's
        // ReporterFilter (share_remote.hpp) can actually dedup a batch
        // from a teammate already in view range instead of always seeing
        // zeros.
        std::string build_sighting_frame(std::uint64_t seq, const std::vector<share::Sighting>& batch,
            const std::vector<std::wstring>& vanished,
            std::uint64_t reporter_stable_id, const Vec3& reporter_position)
        {
            json entities = json::array();
            for (const auto& sighting : batch) entities.push_back(entity_json(sighting));
            json vanished_json = json::array();
            for (const auto& key : vanished) vanished_json.push_back(to_utf8(key));

            json message;
            message["type"] = "sighting";
            message["seq"] = seq;
            if (reporter_stable_id != 0)
            {
                message["reporter_character_id"] = std::to_string(reporter_stable_id);
                message["reporter_x"] = reporter_position.x;
                message["reporter_y"] = reporter_position.y;
                message["reporter_z"] = reporter_position.z;
            }
            message["entities"] = std::move(entities);
            if (!vanished_json.empty()) message["vanished"] = std::move(vanished_json);
            return message.dump();
        }

        std::string build_handshake_frame(const std::string& token, const std::string& server_ip)
        {
            json message;
            message["token"] = token;
            message["server_ip"] = server_ip;
            return message.dump();
        }

        // Reverse mapping of category_string -- best-effort; an unknown
        // category is not a parse error (a future server-side addition
        // this build predates), just a Sighting this client can't display
        // and silently skips, matching build_sightings()' own "unknown
        // ActorKind -> no builder -> skip" convention on the send side.
        share::Kind kind_from_category(const std::string& cat)
        {
            if (cat == "player") return share::Kind::player;
            if (cat == "dino") return share::Kind::dino;
            if (cat == "turret") return share::Kind::turret;
            if (cat == "structure") return share::Kind::structure;
            return share::Kind::other;
        }

        // protocol.Outbound{Type, ReportedBy, ReporterCharacterID,
        // ReporterX/Y/Z, RelayedAt, Entities} -> kopt::share::RemoteBatch.
        // reporter_character_id is the sender's own linked_player_data_id,
        // stringified (see build_sighting_frame's doc comment on the send
        // side) -- parsed back into reporter_stable_id/reporter_position
        // below so ReporterFilter::accept (share_remote.hpp) can actually
        // dedup a batch from a teammate already in view range. A missing
        // or malformed value degrades to 0/zero-position ("unknown"), same
        // permissive handling as every other optional field parsed here --
        // never drops the whole batch over it.
        //
        // reporter_account_id (below) is a separate concern from
        // reporter_stable_id: RemoteView keys its per-reporter map on it
        // (see its own doc comment) because reporter_stable_id can
        // legitimately be 0 (old client, or !local_valid at submission
        // time) -- without a stable non-identity key, two teammates
        // sharing at once would collide into the same map slot and
        // silently evict each other, live symptom "some teammates'
        // sightings never show, no pattern to it" regardless of ESP/radius
        // settings on either end. Keep both fields, don't conflate them.
        bool parse_broadcast(const std::string& payload, share::RemoteBatch& out)
        {
            json message;
            try
            {
                message = json::parse(payload);
            }
            catch (const json::parse_error&)
            {
                return false;
            }
            if (!message.is_object() || message.value("type", "") != "sighting") return false;

            out = share::RemoteBatch{};
            // protocol.Outbound.ReportedBy on the wire (backend_go/internal/
            // hub/client.go's handleSighting sets it to c.AccountID before
            // broadcasting) -- see RemoteBatch::reporter_account_id's own
            // doc comment for why RemoteView needs this to not collide
            // different reporters into the same map slot.
            out.reporter_account_id = message.value("reported_by", std::string{});
            const std::string reporter_character_id = message.value("reporter_character_id", std::string{});
            if (!reporter_character_id.empty())
            {
                try
                {
                    out.reporter_stable_id = std::stoull(reporter_character_id);
                }
                catch (const std::exception&)
                {
                    out.reporter_stable_id = 0; // malformed -- degrade to "unknown", don't drop the batch
                }
            }
            out.reporter_position.x = message.value("reporter_x", 0.0F);
            out.reporter_position.y = message.value("reporter_y", 0.0F);
            out.reporter_position.z = message.value("reporter_z", 0.0F);
            for (const auto& entity : message.value("entities", json::array()))
            {
                share::Sighting s;
                s.kind = kind_from_category(entity.value("cat", ""));
                if (s.kind == share::Kind::other) continue;
                s.team = entity.value("team", 0);
                s.label = to_wide(entity.value("label", std::string{}));
                s.class_name = to_wide(entity.value("class", std::string{}));
                s.tribe = to_wide(entity.value("tribe", std::string{}));
                s.x = entity.value("x", 0.0);
                s.y = entity.value("y", 0.0);
                s.z = entity.value("z", 0.0);
                if (entity.contains("health"))
                {
                    s.has_health = true;
                    s.health = entity.value("health", 0.0);
                    s.max_health = entity.value("max_health", 0.0);
                }
                const std::string status = entity.value("status", std::string{});
                s.dead = status == "dead";
                s.sleeping = status == "sleeping";
                if (status == "knocked_out")
                {
                    s.has_torpor = true;
                    s.torpor = 1.0F;
                    s.max_torpor = 1.0F;
                }
                if (s.kind == share::Kind::turret && entity.contains("turret"))
                {
                    const auto& t = entity["turret"];
                    share::TurretInfo info;
                    info.ammo = t.value("ammo", -1);
                    info.range = t.value("range", static_cast<std::uint8_t>(0));
                    info.targeting = t.value("targeting", static_cast<std::uint8_t>(0));
                    info.warning = t.value("warning", static_cast<std::uint8_t>(0));
                    info.powered = t.value("powered", false);
                    info.active = t.value("active", false);
                    info.targeting_actor = t.value("targeting_actor", false);
                    s.turret = info;
                }
                out.sightings.push_back(std::move(s));
            }
            return true;
        }
    } // namespace (anonymous)

    // ======================================================================
    // Http3Publisher::Impl -- all ngtcp2/OpenSSL state, touched only from
    // the one background worker thread (see run()). Public API methods
    // only ever queue work or flip atomics; none of them call into ngtcp2
    // directly -- ngtcp2_conn is explicitly documented as not thread-safe.
    // ======================================================================
    struct Http3Publisher::Impl
    {
        std::thread worker;
        std::atomic<bool> running{false};
        std::atomic<bool> connected_flag{false};
        // running=false ends just the CURRENT connection attempt (set by
        // run() itself on any error/close, or by stop() to cut an attempt
        // short); stop_requested=true additionally tells the outer
        // reconnect loop in start()'s worker lambda to give up for good
        // instead of retrying with backoff. Only stop() ever sets this one.
        std::atomic<bool> stop_requested{false};

        std::string host;
        std::string port_str;
        std::string token_utf8;
        std::string server_ip_utf8;

        std::mutex out_mutex;
        std::deque<std::string> out_queue; // whole frame payloads, length-prefix added when moved into stream_send_buffer
        std::atomic<std::uint64_t> seq{0};

        std::mutex sub_mutex;
        std::function<void(share::RemoteBatch)> on_batch;

        // -- worker-thread-only state below --
        SOCKET sock{INVALID_SOCKET};
        bool wsa_started{false};
        sockaddr_storage local_addr{};
        int local_addr_len{};
        sockaddr_storage remote_addr{};
        int remote_addr_len{};

        ngtcp2_conn* conn{};
        SSL_CTX* ssl_ctx{};
        SSL* ssl{};
        ngtcp2_crypto_ossl_ctx* ossl_ctx{};
        ngtcp2_crypto_conn_ref conn_ref{};
        std::uint8_t reset_secret[32]{};

        bool stream_opened{false};
        int64_t stream_id{-1};
        bool handshake_frame_queued{false};
        bool handshake_acked{false};

        std::string stream_send_buffer;
        std::string stream_recv_buffer;
        std::uint64_t recv_unacked_bytes{0};
        std::chrono::steady_clock::time_point last_queued_at;

        ~Impl() { teardown(); }

        void queue_frame(std::string payload)
        {
            std::string framed;
            framed.resize(4 + payload.size());
            const std::uint32_t len = static_cast<std::uint32_t>(payload.size());
            framed[0] = static_cast<char>((len >> 24) & 0xFF);
            framed[1] = static_cast<char>((len >> 16) & 0xFF);
            framed[2] = static_cast<char>((len >> 8) & 0xFF);
            framed[3] = static_cast<char>(len & 0xFF);
            std::memcpy(framed.data() + 4, payload.data(), payload.size());
            last_queued_at = std::chrono::steady_clock::now();
            const std::lock_guard<std::mutex> lock(out_mutex);
            out_queue.push_back(std::move(framed));
        }

        // Parses complete length-prefixed frames out of stream_recv_buffer
        // as they become available -- a TCP-like stream has no message
        // boundaries of its own, this is the client-side mirror of
        // quicserver/frame.go's readFrame.
        void drain_recv_frames()
        {
            for (;;)
            {
                if (stream_recv_buffer.size() < 4) return;
                const auto b = reinterpret_cast<const unsigned char*>(stream_recv_buffer.data());
                const std::uint32_t len = (static_cast<std::uint32_t>(b[0]) << 24) |
                    (static_cast<std::uint32_t>(b[1]) << 16) | (static_cast<std::uint32_t>(b[2]) << 8) | b[3];
                if (len == 0 || len > kMaxFramePayload)
                {
                    log_line(std::format(L"share: bogus frame length {} from relay, dropping connection", len));
                    running.store(false, std::memory_order_release);
                    return;
                }
                if (stream_recv_buffer.size() < 4U + len) return; // wait for more bytes
                const std::string payload = stream_recv_buffer.substr(4, len);
                stream_recv_buffer.erase(0, 4U + len);
                handle_frame(payload);
            }
        }

        void handle_frame(const std::string& payload)
        {
            if (!handshake_acked)
            {
                json response;
                try
                {
                    response = json::parse(payload);
                }
                catch (const json::parse_error&)
                {
                    log_line(L"share: handshake response was not valid JSON");
                    running.store(false, std::memory_order_release);
                    return;
                }
                if (!response.value("ok", false))
                {
                    log_line(to_wide("share: handshake rejected: " + response.value("error", std::string{"unknown"})));
                    running.store(false, std::memory_order_release);
                    return;
                }
                handshake_acked = true;
                connected_flag.store(true, std::memory_order_release);
                log_line(L"share: handshake accepted, connected");
                return;
            }

            share::RemoteBatch batch;
            if (!parse_broadcast(payload, batch)) return; // not a sighting broadcast (e.g. a ping) -- ignore
            batch.received_at = std::chrono::steady_clock::now();
            const std::lock_guard<std::mutex> lock(sub_mutex);
            if (on_batch) on_batch(std::move(batch));
        }

        // ------------------------------------------------------------------
        // ngtcp2 callbacks -- static member functions, not free functions:
        // ngtcp2's C API takes plain function pointers, and every one of
        // these recovers its Impl* from user_data (set at
        // ngtcp2_conn_client_new time). Static members (not an anonymous-
        // namespace free function referring to Impl by name) because Impl
        // itself is a private nested type of Http3Publisher -- code outside
        // any of Impl's own members can't name "Http3Publisher::Impl" at
        // all, even just to declare a local type alias for it.
        // ------------------------------------------------------------------
        static void rand_cb(uint8_t* dest, size_t destlen, const ngtcp2_rand_ctx*)
        {
            // No per-connection state needed -- OpenSSL's own DRBG is
            // already process-global and thread-safe.
            RAND_bytes(dest, static_cast<int>(destlen));
        }

        static int get_new_connection_id2_cb(ngtcp2_conn*, ngtcp2_cid* cid, ngtcp2_stateless_reset_token* token,
            size_t cidlen, void* user_data)
        {
            auto* self = static_cast<Impl*>(user_data);
            cid->datalen = cidlen;
            RAND_bytes(cid->data, static_cast<int>(cidlen));
            if (ngtcp2_crypto_generate_stateless_reset_token(token->data, self->reset_secret,
                    sizeof(self->reset_secret), cid) != 0)
                return NGTCP2_ERR_CALLBACK_FAILURE;
            return 0;
        }

        static int handshake_completed_cb(ngtcp2_conn*, void*)
        {
            log_line(L"share: QUIC handshake completed");
            return 0;
        }

        static int recv_stream_data_cb(ngtcp2_conn*, uint32_t, int64_t stream_id, uint64_t, const uint8_t* data,
            size_t datalen, void* user_data, void*)
        {
            auto* self = static_cast<Impl*>(user_data);
            if (stream_id != self->stream_id) return 0; // only ever one stream on this connection
            self->stream_recv_buffer.append(reinterpret_cast<const char*>(data), datalen);
            self->recv_unacked_bytes += datalen;
            self->drain_recv_frames();
            return 0;
        }

        static int stream_open_cb(ngtcp2_conn*, int64_t stream_id, void*)
        {
            log_line(std::format(L"share: unexpected server-opened stream {}", stream_id));
            return 0;
        }

        static int stream_close_cb(ngtcp2_conn*, uint32_t, int64_t stream_id, uint64_t app_error_code,
            void* user_data, void*)
        {
            log_line(std::format(L"share: stream {} closed (app_error_code={})", stream_id, app_error_code));
            static_cast<Impl*>(user_data)->running.store(false, std::memory_order_release);
            return 0;
        }

        // Every other MANDATORY client callback (client_initial,
        // recv_crypto_data, encrypt, decrypt, hp_mask, recv_retry,
        // delete_crypto_aead_ctx, delete_crypto_cipher_ctx,
        // get_path_challenge_data2, version_negotiation) is filled in with
        // ngtcp2_crypto's own ready-made implementation -- see run()'s
        // ngtcp2_conn_client_new callbacks argument. Reimplementing TLS
        // record handling by hand is exactly what linking
        // ngtcp2_crypto_ossl exists to avoid.
        static ngtcp2_conn* get_conn_cb(ngtcp2_crypto_conn_ref* ref)
        {
            return static_cast<Impl*>(ref->user_data)->conn;
        }

        void run();
        void teardown();
    };

    void Http3Publisher::Impl::teardown()
    {
        if (conn != nullptr) { ngtcp2_conn_del(conn); conn = nullptr; }
        if (ossl_ctx != nullptr) { ngtcp2_crypto_ossl_ctx_del(ossl_ctx); ossl_ctx = nullptr; }
        if (ssl != nullptr) { SSL_set_app_data(ssl, nullptr); SSL_free(ssl); ssl = nullptr; }
        if (ssl_ctx != nullptr) { SSL_CTX_free(ssl_ctx); ssl_ctx = nullptr; }
        if (sock != INVALID_SOCKET) { closesocket(sock); sock = INVALID_SOCKET; }
        if (wsa_started) { WSACleanup(); wsa_started = false; }
        stream_opened = false;
        stream_id = -1;
        handshake_frame_queued = false;
        handshake_acked = false;
        connected_flag.store(false, std::memory_order_release);
    }

    void Http3Publisher::Impl::run()
    {
        log_line(L"share: DIAG run() entered");
        WSADATA wsa_data{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
        {
            log_line(L"share: WSAStartup failed");
            return;
        }
        wsa_started = true;
        log_line(L"share: DIAG WSAStartup ok");

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;
        addrinfo* resolved{};
        if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &resolved) != 0 || resolved == nullptr)
        {
            log_line(std::format(L"share: DNS resolve failed for {}:{}", to_wide(host), to_wide(port_str)));
            teardown();
            return;
        }
        sock = socket(resolved->ai_family, resolved->ai_socktype, resolved->ai_protocol);
        if (sock == INVALID_SOCKET)
        {
            freeaddrinfo(resolved);
            log_line(L"share: socket() failed");
            teardown();
            return;
        }
        if (connect(sock, resolved->ai_addr, static_cast<int>(resolved->ai_addrlen)) == SOCKET_ERROR)
        {
            log_line(std::format(L"share: connect() failed (WSAGetLastError={})", WSAGetLastError()));
            freeaddrinfo(resolved);
            teardown();
            return;
        }
        std::memcpy(&remote_addr, resolved->ai_addr, resolved->ai_addrlen);
        remote_addr_len = static_cast<int>(resolved->ai_addrlen);
        freeaddrinfo(resolved);
        log_line(L"share: DIAG socket connected");

        local_addr_len = sizeof(local_addr);
        if (getsockname(sock, reinterpret_cast<sockaddr*>(&local_addr), &local_addr_len) == SOCKET_ERROR)
        {
            log_line(L"share: getsockname() failed");
            teardown();
            return;
        }
        u_long non_blocking = 1;
        ioctlsocket(sock, FIONBIO, &non_blocking);

        log_line(L"share: DIAG socket setup done, calling ngtcp2_crypto_ossl_init");
        ngtcp2_crypto_ossl_init();
        log_line(L"share: DIAG ngtcp2_crypto_ossl_init returned");
        RAND_bytes(reset_secret, sizeof(reset_secret));
        log_line(L"share: DIAG RAND_bytes ok, calling SSL_CTX_new");

        ssl_ctx = SSL_CTX_new(TLS_client_method());
        log_line(L"share: DIAG SSL_CTX_new returned, ssl_ctx=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(ssl_ctx)));
        if (ssl_ctx == nullptr)
        {
            log_line(L"share: SSL_CTX_new failed");
            teardown();
            return;
        }
        log_line(L"share: DIAG calling SSL_CTX_set_min_proto_version");
        const int min_ver_rc = SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_3_VERSION);
        log_line(L"share: DIAG SSL_CTX_set_min_proto_version returned " + std::to_wstring(min_ver_rc));
        if (min_ver_rc != 1)
        {
            log_line(L"share: SSL_CTX_set_min_proto_version failed");
            teardown();
            return;
        }
        // No CA verification wired up yet -- the relay's cert is either the
        // ephemeral dev one (quicserver.GenerateDevTLSConfig, no CA to
        // check against at all) or a real one pinned by fingerprint later;
        // neither exists as a shipped trust root for this client today. See
        // include/kopt/http3_publisher.hpp's history in the DTO-sharing
        // plan -- production TLS verification is explicitly future work,
        // not silently skipped without a reason on file.
        log_line(L"share: DIAG calling SSL_CTX_set_verify");
        SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, nullptr);
        log_line(L"share: DIAG SSL_CTX_set_verify returned");

        log_line(L"share: DIAG calling SSL_new");
        ssl = SSL_new(ssl_ctx);
        log_line(L"share: DIAG SSL_new returned, ssl=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(ssl)));
        if (ssl == nullptr)
        {
            log_line(L"share: SSL_new failed");
            teardown();
            return;
        }
        unsigned char alpn_wire[1 + std::char_traits<char>::length(kAlpn)];
        alpn_wire[0] = static_cast<unsigned char>(std::char_traits<char>::length(kAlpn));
        std::memcpy(alpn_wire + 1, kAlpn, alpn_wire[0]);
        log_line(L"share: DIAG calling SSL_set_alpn_protos");
        SSL_set_alpn_protos(ssl, alpn_wire, static_cast<unsigned int>(sizeof(alpn_wire)));
        log_line(L"share: DIAG calling SSL_set_tlsext_host_name");
        SSL_set_tlsext_host_name(ssl, host.c_str());
        log_line(L"share: DIAG SSL_set_tlsext_host_name returned");

        conn_ref.get_conn = get_conn_cb;
        conn_ref.user_data = this;
        log_line(L"share: DIAG calling SSL_set_app_data");
        SSL_set_app_data(ssl, &conn_ref);
        // ngtcp2_crypto_ossl_configure_client_session only wires up the
        // QUIC-TLS callbacks (SSL_set_quic_tls_cbs) -- it does NOT set
        // connect/accept state, unlike a normal SSL_connect() flow that
        // would. Skipping this produced a real, confirmed-live failure:
        // ngtcp2_conn_writev_stream returning NGTCP2_ERR_CALLBACK_FAILURE
        // with OpenSSL's error queue holding "SSL routines::connection
        // type not set" (error 0A000090) on the very first write attempt.
        SSL_set_connect_state(ssl);
        log_line(L"share: DIAG calling ngtcp2_crypto_ossl_ctx_new");

        if (ngtcp2_crypto_ossl_ctx_new(&ossl_ctx, ssl) != 0)
        {
            log_line(L"share: ngtcp2_crypto_ossl_ctx_new failed");
            teardown();
            return;
        }
        log_line(L"share: DIAG calling ngtcp2_crypto_ossl_configure_client_session");
        if (ngtcp2_crypto_ossl_configure_client_session(ssl) != 0)
        {
            log_line(L"share: ngtcp2_crypto_ossl_configure_client_session failed");
            teardown();
            return;
        }
        log_line(L"share: DIAG ngtcp2_crypto_ossl setup done");

        ngtcp2_cid dcid{};
        ngtcp2_cid scid{};
        dcid.datalen = kCidLen;
        RAND_bytes(dcid.data, static_cast<int>(kCidLen));
        scid.datalen = kCidLen;
        RAND_bytes(scid.data, static_cast<int>(kCidLen));
        log_line(L"share: DIAG cids generated");

        ngtcp2_path path{};
        path.local.addr = reinterpret_cast<ngtcp2_sockaddr*>(&local_addr);
        path.local.addrlen = static_cast<ngtcp2_socklen>(local_addr_len);
        path.remote.addr = reinterpret_cast<ngtcp2_sockaddr*>(&remote_addr);
        path.remote.addrlen = static_cast<ngtcp2_socklen>(remote_addr_len);

        ngtcp2_callbacks callbacks{};
        callbacks.client_initial = ngtcp2_crypto_client_initial_cb;
        callbacks.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
        callbacks.handshake_completed = handshake_completed_cb;
        callbacks.encrypt = ngtcp2_crypto_encrypt_cb;
        callbacks.decrypt = ngtcp2_crypto_decrypt_cb;
        callbacks.hp_mask = ngtcp2_crypto_hp_mask_cb;
        callbacks.recv_stream_data = recv_stream_data_cb;
        callbacks.stream_open = stream_open_cb;
        callbacks.stream_close = stream_close_cb;
        callbacks.recv_retry = ngtcp2_crypto_recv_retry_cb;
        // Mandatory even though this client never triggers a post-handshake
        // key update itself -- ngtcp2_conn_client_new_versioned asserts on
        // this field being non-null at connection-creation time (confirmed
        // live: "Assertion failed: callbacks->update_key,
        // ngtcp2_conn.c:1230" -- not a lazily-invoked path, so it can't be
        // skipped just because this client doesn't initiate key updates).
        callbacks.update_key = ngtcp2_crypto_update_key_cb;
        callbacks.rand = rand_cb;
        callbacks.get_new_connection_id2 = get_new_connection_id2_cb;
        callbacks.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
        callbacks.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
        callbacks.get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb;
        callbacks.version_negotiation = ngtcp2_crypto_version_negotiation_cb;

        log_line(L"share: DIAG callbacks populated, calling ngtcp2_settings_default");
        ngtcp2_settings settings;
        ngtcp2_settings_default(&settings);
        settings.initial_ts = now_ts();
        settings.max_tx_udp_payload_size = kMaxUdpPayload;
        settings.rand_ctx.native_handle = nullptr;
        log_line(L"share: DIAG calling ngtcp2_transport_params_default");

        ngtcp2_transport_params params;
        ngtcp2_transport_params_default(&params);
        params.initial_max_stream_data_bidi_local = 1U << 20;
        params.initial_max_stream_data_bidi_remote = 1U << 20;
        params.initial_max_data = 1U << 22;
        params.initial_max_streams_bidi = 1;
        params.initial_max_streams_uni = 0;

        log_line(L"share: DIAG calling ngtcp2_conn_client_new");
        const int client_new_rc = ngtcp2_conn_client_new(&conn, &dcid, &scid, &path, NGTCP2_PROTO_VER_V1,
            &callbacks, &settings, &params, nullptr, this);
        log_line(L"share: DIAG ngtcp2_conn_client_new returned " + std::to_wstring(client_new_rc));
        if (client_new_rc != 0)
        {
            log_line(L"share: ngtcp2_conn_client_new failed");
            teardown();
            return;
        }
        log_line(L"share: DIAG calling ngtcp2_conn_set_tls_native_handle");
        ngtcp2_conn_set_tls_native_handle(conn, ossl_ctx);
        log_line(L"share: DIAG ngtcp2_conn_set_tls_native_handle returned");

        log_line(std::format(L"share: connecting to {}:{}", to_wide(host), to_wide(port_str)));
        running.store(true, std::memory_order_release);
        last_queued_at = std::chrono::steady_clock::now();

        std::uint8_t recv_buf[65536];
        while (running.load(std::memory_order_acquire))
        {
            // ---- write everything ngtcp2 currently has to say ----
            for (;;)
            {
                if (stream_opened && stream_send_buffer.empty())
                {
                    std::lock_guard<std::mutex> lock(out_mutex);
                    if (!out_queue.empty())
                    {
                        stream_send_buffer = std::move(out_queue.front());
                        out_queue.pop_front();
                    }
                }

                std::uint8_t send_buf[kMaxUdpPayload];
                ngtcp2_ssize datalen = -1;
                ngtcp2_vec vec{};
                std::size_t vec_cnt = 0;
                int64_t write_stream_id = -1;
                if (stream_opened && !stream_send_buffer.empty())
                {
                    vec.base = reinterpret_cast<uint8_t*>(stream_send_buffer.data());
                    vec.len = stream_send_buffer.size();
                    vec_cnt = 1;
                    write_stream_id = stream_id;
                }
                ngtcp2_pkt_info pi{};
                const ngtcp2_ssize n = ngtcp2_conn_writev_stream(conn, &path, &pi, send_buf, sizeof(send_buf),
                    &datalen, NGTCP2_WRITE_STREAM_FLAG_NONE, write_stream_id, &vec, vec_cnt, now_ts());
                if (n < 0)
                {
                    if (n == NGTCP2_ERR_STREAM_DATA_BLOCKED) break; // flow control -- wait for a window update
                    log_line(std::format(L"share: ngtcp2_conn_writev_stream failed ({})", static_cast<long long>(n)));
                    if (n == NGTCP2_ERR_CALLBACK_FAILURE)
                    {
                        // The library only reports "some user callback
                        // returned failure", not which one or why -- drain
                        // OpenSSL's thread-local error queue, since
                        // ngtcp2_crypto's TLS callbacks (client_initial,
                        // recv_crypto_data, ...) surface real SSL_do_handshake
                        // failures this way and otherwise leave zero trace.
                        unsigned long ssl_err;
                        while ((ssl_err = ERR_get_error()) != 0)
                        {
                            char buf[256];
                            ERR_error_string_n(ssl_err, buf, sizeof(buf));
                            log_line(L"share: OpenSSL error: " + to_wide(buf));
                        }
                    }
                    running.store(false, std::memory_order_release);
                    break;
                }
                if (datalen > 0) stream_send_buffer.erase(0, static_cast<std::size_t>(datalen));
                if (n == 0) break; // nothing left to send this round
                send(sock, reinterpret_cast<const char*>(send_buf), static_cast<int>(n), 0);
            }
            if (!running.load(std::memory_order_acquire)) break;

            // Handshake complete and no stream open yet -- this is the
            // ONE stream this connection will ever use for the rest of its
            // life (see stream_open_cb's log for the only other case: the
            // server trying to open one of its own, which never happens in
            // this protocol).
            if (!stream_opened && ngtcp2_conn_get_handshake_completed(conn))
            {
                int64_t new_stream_id{};
                if (ngtcp2_conn_open_bidi_stream(conn, &new_stream_id, nullptr) == 0)
                {
                    stream_id = new_stream_id;
                    stream_opened = true;
                    if (!handshake_frame_queued)
                    {
                        handshake_frame_queued = true;
                        queue_frame(build_handshake_frame(token_utf8, server_ip_utf8));
                    }
                }
            }

            // ---- keepalive: see kPingInterval's doc comment above ----
            if (handshake_acked &&
                std::chrono::steady_clock::now() - last_queued_at >= kPingInterval)
            {
                queue_frame(build_ping_frame());
            }

            // ---- wait for the next incoming packet or the next timer ----
            const ngtcp2_tstamp expiry = ngtcp2_conn_get_expiry(conn);
            const ngtcp2_tstamp now = now_ts();
            long timeout_ms = expiry > now
                ? static_cast<long>((expiry - now) / 1'000'000)
                : 0;
            timeout_ms = std::clamp<long>(timeout_ms, 1, 200);

            fd_set read_set;
            FD_ZERO(&read_set);
            FD_SET(sock, &read_set);
            timeval tv{};
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            const int ready = select(0, &read_set, nullptr, nullptr, &tv);

            if (ready > 0 && FD_ISSET(sock, &read_set))
            {
                for (;;)
                {
                    const int received = recv(sock, reinterpret_cast<char*>(recv_buf), sizeof(recv_buf), 0);
                    if (received <= 0) break; // WOULDBLOCK or error -- either way, nothing more to read right now
                    ngtcp2_pkt_info pi{};
                    const int read_rv = ngtcp2_conn_read_pkt(conn, &path, &pi, recv_buf,
                        static_cast<std::size_t>(received), now_ts());
                    if (read_rv != 0)
                    {
                        log_line(std::format(L"share: ngtcp2_conn_read_pkt failed ({})", read_rv));
                        running.store(false, std::memory_order_release);
                        break;
                    }
                }
            }
            if (!running.load(std::memory_order_acquire)) break;

            if (ngtcp2_conn_handle_expiry(conn, now_ts()) != 0)
            {
                log_line(L"share: ngtcp2_conn_handle_expiry failed (idle timeout or similar)");
                running.store(false, std::memory_order_release);
                break;
            }

            if (recv_unacked_bytes > 0)
            {
                ngtcp2_conn_extend_max_offset(conn, recv_unacked_bytes);
                if (stream_opened) ngtcp2_conn_extend_max_stream_offset(conn, stream_id, recv_unacked_bytes);
                recv_unacked_bytes = 0;
            }
        }

        log_line(L"share: connection loop ending");
        teardown();
    }

    // ======================================================================
    // Public API -- every method here only touches atomics/mutex-guarded
    // queues, never ngtcp2 state directly (see Impl's own comment).
    // ======================================================================
    Http3Publisher::Http3Publisher() : impl_(std::make_unique<Impl>()) {}
    Http3Publisher::~Http3Publisher() { stop(); }

    void Http3Publisher::start(std::wstring endpoint, std::wstring token, std::wstring server_ip)
    {
        stop(); // idempotent: tears down any previous connection first
        impl_->stop_requested.store(false, std::memory_order_release);

        const std::string endpoint_utf8 = to_utf8(endpoint);
        const auto colon = endpoint_utf8.rfind(':');
        if (colon == std::string::npos)
        {
            log_line(std::format(L"share: endpoint {} has no :port, refusing to start", endpoint));
            return;
        }
        impl_->host = endpoint_utf8.substr(0, colon);
        impl_->port_str = endpoint_utf8.substr(colon + 1);
        impl_->token_utf8 = to_utf8(token);
        impl_->server_ip_utf8 = to_utf8(server_ip);

        // std::thread has no default exception handler at all -- an
        // uncaught exception escaping the thread function calls
        // std::terminate() unconditionally, which (unlike a Win32 SEH
        // exception) does not reliably go through SetUnhandledExceptionFilter
        // under Wine, taking the whole host process down with no
        // diagnostics/crash-* bundle and no FATAL SEH log line either --
        // found exactly this way, live, while first bringing this transport
        // up. Every exception this code could plausibly throw (nlohmann::
        // json's parse/type errors, std::format_error, std::bad_alloc) is a
        // "this connection attempt failed" event, not a fatal one.
        //
        // Outer reconnect loop: run() returns on ANY disconnect (idle
        // timeout, relay restart, transient network drop, stream close by
        // the server, ...) -- confirmed live, a single dropped connection
        // otherwise killed sharing for the rest of the game session with no
        // way back short of --unload/reinject. Backoff starts at 1s,
        // doubles, caps at 30s; resets to 1s once a connection survives
        // long enough to have plausibly gone through a real handshake
        // (kResetBackoffAfter), so one bad attempt doesn't inflate the
        // wait for a connection that's otherwise healthy and just blipped.
        impl_->worker = std::thread([impl = impl_.get()]
        {
            using namespace std::chrono;
            constexpr auto kBaseBackoff = seconds(1);
            constexpr auto kMaxBackoff = seconds(30);
            constexpr auto kResetBackoffAfter = seconds(5);
            auto backoff = kBaseBackoff;

            while (!impl->stop_requested.load(std::memory_order_acquire))
            {
                const auto attempt_started = steady_clock::now();
                try
                {
                    impl->run();
                }
                catch (const std::exception& e)
                {
                    log_line(std::format(L"share: worker thread threw {}: {}",
                        to_wide(typeid(e).name()), to_wide(e.what())));
                }
                catch (...)
                {
                    log_line(L"share: worker thread threw a non-std::exception");
                }

                if (impl->stop_requested.load(std::memory_order_acquire)) break;

                backoff = (steady_clock::now() - attempt_started >= kResetBackoffAfter)
                    ? kBaseBackoff
                    : std::min(backoff * 2, kMaxBackoff);

                log_line(std::format(L"share: connection lost, reconnecting in {}s",
                    duration_cast<seconds>(backoff).count()));

                // Slept in short slices, not one long sleep_for(backoff) --
                // otherwise stop() (a live share_enabled=false toggle, or
                // the game/menu unloading) would block for up to
                // kMaxBackoff waiting on a join() of a thread that's just
                // sleeping.
                const auto wake_at = steady_clock::now() + backoff;
                while (!impl->stop_requested.load(std::memory_order_acquire) &&
                    steady_clock::now() < wake_at)
                {
                    std::this_thread::sleep_for(milliseconds(100));
                }
            }
        });
    }

    void Http3Publisher::stop()
    {
        impl_->stop_requested.store(true, std::memory_order_release);
        impl_->running.store(false, std::memory_order_release);
        if (impl_->worker.joinable()) impl_->worker.join();
    }

    void Http3Publisher::submit_sightings(std::vector<share::Sighting> batch, std::vector<std::wstring> vanished,
        std::uint64_t reporter_stable_id, Vec3 reporter_position)
    {
        if (!impl_->connected_flag.load(std::memory_order_acquire)) return;
        if (batch.empty() && vanished.empty()) return;
        const std::uint64_t seq = impl_->seq.fetch_add(1, std::memory_order_relaxed) + 1;
        impl_->queue_frame(build_sighting_frame(seq, batch, vanished, reporter_stable_id, reporter_position));
    }

    void Http3Publisher::submit_notifications(std::vector<share::Notification>)
    {
        // No-op, deliberately: protocol.Inbound (backend/backend_go/
        // internal/protocol/message.go) has no notifications field at all
        // -- only Entities/Vanished. There is currently no wire shape for
        // this on the Go side to send it as. Inventing one here that the
        // server doesn't understand would either be silently dropped by
        // its own JSON decode (harmless but pointless) or, worse, rejected
        // as a malformed sighting if shoehorned into the Entities array --
        // neither is better than an honest no-op with this comment
        // attached. Needs a protocol extension on the Go side first.
    }

    void Http3Publisher::subscribe(std::function<void(share::RemoteBatch)> on_batch)
    {
        const std::lock_guard<std::mutex> lock(impl_->sub_mutex);
        impl_->on_batch = std::move(on_batch);
    }

    bool Http3Publisher::connected() const noexcept
    {
        return impl_->connected_flag.load(std::memory_order_acquire);
    }
}
