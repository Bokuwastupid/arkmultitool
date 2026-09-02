#pragma once

// TODO(windows-msvc): real QUIC transport for kopt::Publisher. NOT
// implemented, NOT compiled into any target (see CMakeLists.txt -- this
// header is not referenced from there on purpose, so the mingw
// cross-compile build already verified in the Linux dev sandbox stays
// green while this is worked on). Whoever picks this up MUST have a real
// Windows machine with MSVC -- see "Why this can't be built here" below.
//
// ============================================================================
// WHY THIS CAN'T BE BUILT/VERIFIED IN THE LINUX DEV SANDBOX
// ============================================================================
// msquic (github.com/microsoft/msquic) is the intended library -- it's
// Microsoft's own QUIC implementation, matches this DLL's native Win64
// target, and is what a "real" Windows client would use. Its Windows build
// is documented and supported ONLY with MSVC (uses SChannel/BCrypt for TLS
// 1.3 by default on Windows, and its CMake presets assume cl.exe) --
// cross-compiling it for Windows from Linux via mingw-w64 is not a path
// msquic upstream supports or tests, and attempting it is a real risk of
// burning hours on a build system fight with a low chance of a working
// binary at the end, not a "just needs more flags" problem.
// The dev sandbox this comment was written in has no Wine/Windows/MSVC
// available at all, so this file could not be built OR linked OR run here
// -- do not trust this header's correctness the way you'd trust the rest
// of this codebase's C++; nothing below has compiled even once.
// Needs: a Windows machine (or a Windows VM) with Visual Studio / MSVC
// Build Tools installed, vcpkg or msquic's own CMake+NuGet build for the
// dependency, and a way to actually run ShooterGame.exe (Windows native,
// or Proton -- NOT plain Wine, since QUIC's UDP path plus msquic's
// SChannel/BCrypt calls are far more likely to hit Wine gaps than the
// D3D11/UE4 hooking code the rest of this project already leans on).
//
// ============================================================================
// SERVER-SIDE CONTRACT THIS MUST MATCH (backend/backend_go/internal/quicserver)
// ============================================================================
// The Go relay already implements and end-to-end-verifies the server half
// of this exact transport (real QUIC handshake + JWT auth + group
// membership check + broadcast, proven via a live docker-compose run and a
// throwaway QUIC client in a separate container -- see
// backend/backend_go/internal/quicserver/server.go and server_test.go).
// This client is the missing other end of that already-working contract,
// not a green-field protocol design:
//
//   1. ALPN: "ark-quic-v1" (see quicserver.GenerateDevTLSConfig /
//      LoadTLSConfig's NextProtos). The msquic connection's configuration
//      MUST offer this exact ALPN string or the server's quic.ListenAddr
//      rejects the handshake before it ever reaches application code.
//
//   2. One bidirectional stream per connection, opened by the CLIENT
//      immediately after the QUIC handshake completes (server only ever
//      calls AcceptStream once per connection -- see
//      quicserver.Server.serveConn). Opening a second stream is undefined
//      by the server (ignored, not rejected) -- don't rely on it.
//
//   3. Every message on that stream is a 4-byte big-endian length prefix
//      followed by exactly that many bytes of UTF-8 JSON -- see
//      quicserver/frame.go's readFrame/writeFrame. This is NOT
//      newline-delimited and NOT a raw JSON stream; a naive
//      one-message-per-Write() without the length prefix will hang the
//      server's readFrame forever waiting for 4 header bytes that never
//      come as such.
//
//   4. First frame the client must send (after opening the stream, before
//      anything else) is the handshake, matching quicserver/handshake.go's
//      handshakeRequest exactly:
//          {"token": "<JWT, same bearer token the WS path uses>",
//           "group_id": "<sharing group uuid>",
//           "server_ip": "<this game server's ip:port, see the server_ip
//                          TODO -- also not yet resolved, tracked
//                          separately, do NOT block this transport on it;
//                          a placeholder/manually-configured value is fine
//                          for getting the QUIC path itself working first>"}
//      The server replies with exactly one frame before any data traffic,
//      matching handshakeResponse: {"ok": true} or {"ok": false, "error":
//      "<reason>"}. A false/missing "ok" means the server already closed
//      its side -- do not attempt to read/write anything further on this
//      stream, open a new connection instead (don't retry a failed
//      handshake on the same stream, matching the server's own "reject
//      then close" behavior in Server.handshake).
//
//   5. After a successful handshake, the wire format is exactly
//      internal/protocol.Inbound (client -> server, JSON, same shape the
//      WS path's protocol.Decode expects -- share::Sighting/Notification
//      batches map onto Inbound.Entities/Vanished, see
//      include/kopt/share.hpp for the DTO this client already builds) and
//      internal/protocol.Outbound (server -> client, broadcasts from other
//      clients in the same (group_id, server_ip) room, plus periodic
//      {"type":"ping"} keepalives the client should answer with
//      {"type":"pong"} -- or just send anything at all, any successful
//      read resets the server's liveness deadline, see hub/client.go's
//      readPump comment). Do NOT reinvent this JSON shape -- copy the
//      field names from internal/protocol/message.go verbatim; a
//      near-miss here fails silently as "server drops my messages", not as
//      a compile error.
//
// ============================================================================
// SHAPE THIS CLASS NEEDS TO END UP IN (see include/kopt/publisher.hpp)
// ============================================================================
// class Http3Publisher final : public Publisher
// {
// public:
//     void start(std::wstring endpoint, std::wstring token) override;
//     void stop() override;
//     void submit_sightings(std::vector<share::Sighting> batch,
//         std::vector<std::wstring> vanished) override;
//     void submit_notifications(std::vector<share::Notification> batch) override;
//     void subscribe(std::function<void(share::RemoteBatch)> on_batch) override;
//     [[nodiscard]] bool connected() const noexcept override;
//     // ... msquic handles (HQUIC registration/configuration/connection/
//     // stream), a background read thread pumping length-prefixed frames
//     // into subscribe()'s callback, a write path off the hot D3D11
//     // Present-hook thread (submit_* must never block the caller -- see
//     // publisher.hpp's doc comment on that contract; queue + a dedicated
//     // send thread or a bounded async send via msquic's own stream-send
//     // completion callback, not a synchronous QUIC_STREAM_SEND on the
//     // calling thread).
// };
//
// Wiring point once this exists: src/payload.cpp currently constructs
// `std::unique_ptr<kopt::Publisher> g_publisher =
// std::make_unique<kopt::NoopPublisher>();` -- swap that for
// Http3Publisher there (behind the existing share_enabled config flag,
// unchanged), add http3_publisher.cpp to kopt_payload's sources in
// CMakeLists.txt, and link msquic (target_link_libraries) once its
// Windows/MSVC build is actually producing a .lib to link against.
