// Standalone QUIC-client smoke test -- NOT injected into ShooterGame.exe.
// Exercises Http3Publisher::start() exactly like payload.cpp does, but as
// a plain wine64 process launched directly from a terminal, so a hard
// crash gets Wine's own SEH backtrace (module+offset, register dump)
// instead of vanishing silently the way it does when the crashing thread
// lives inside an injected DLL in someone else's process.
//
// Second use (argv[1] == "broadcast"): connects as a SECOND client into the
// SAME (active_group, server_ip) room as a real, already-connected game
// client and submits one fake Sighting, to exercise the receive side
// end-to-end -- ark_relay should broadcast it to every OTHER client in
// that room, and the live game's on_remote_batch (payload.cpp) should
// pick it up. "Same room" now means "same account's active_group_id" --
// pass a token for an account whose active group matches the live
// session's, there's no group_id argument to line up separately anymore.
// Token/server_ip come from argv so this stays in sync with whatever the
// live test session is actually using, instead of a hardcoded copy.
#include "kopt/http3_publisher.hpp"
#include "kopt/share.hpp"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace
{
    std::wstring to_wide(const char* utf8)
    {
        const int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
        std::wstring out(static_cast<std::size_t>(len > 0 ? len - 1 : 0), L'\0');
        if (len > 1) MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), len);
        return out;
    }
}

int main(int argc, char** argv)
{
    const bool broadcast_mode = argc > 1 && std::string(argv[1]) == "broadcast";
    const std::wstring token = argc > 2 ? to_wide(argv[2]) : L"quic-test-token";
    const std::wstring server_ip = argc > 3 ? to_wide(argv[3]) : L"10.99.0.1:7777";

    std::puts("quic_test: starting Http3Publisher");
    std::fflush(stdout);

    kopt::Http3Publisher publisher;
    publisher.start(L"127.0.0.1:8443", token, server_ip);

    std::puts("quic_test: start() returned, waiting for handshake");
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::seconds(1));

    if (broadcast_mode)
    {
        std::puts("quic_test: submitting one fake Sighting (kind=dino, label=QuicTestBroadcast)");
        std::fflush(stdout);
        kopt::share::Sighting fake;
        fake.kind = kopt::share::Kind::dino;
        fake.address = 0xDEADBEEF;
        fake.label = L"QuicTestBroadcast";
        fake.class_name = L"Rex_Character_BP_C";
        fake.team = 999;
        fake.x = argc > 4 ? std::stof(argv[4]) : 12345.0F;
        fake.y = argc > 5 ? std::stof(argv[5]) : 6789.0F;
        fake.z = argc > 6 ? std::stof(argv[6]) : 100.0F;
        fake.health = 500.0F;
        fake.max_health = 500.0F;
        fake.has_health = true;
        publisher.submit_sightings({fake}, {});
    }

    std::puts("quic_test: sleeping 4s");
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::seconds(4));

    std::puts("quic_test: stopping");
    std::fflush(stdout);
    publisher.stop();

    std::puts("quic_test: done");
    return 0;
}
