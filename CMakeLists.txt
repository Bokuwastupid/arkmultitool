cmake_minimum_required(VERSION 3.24)

project(KOPTInternal VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR
        "ARK: Survival Evolved is a Win64 process under Proton. Configure this project "
        "with a Win64 compiler (MSVC on Windows or mingw-w64 on Linux).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default relay address baked into kopt_payload (see its own
# target_compile_definitions and config.hpp's KOPT_DEFAULT_SHARE_ENDPOINT
# comment) -- override with -DKOPT_RELAY_DEFAULT=host:port at configure
# time. Still just a default: kopt_internal.ini's [Share] Endpoint and
# kopt_injector.exe's --backend both win over it at runtime.
#
# Placeholder on purpose -- not the real relay address. That value is
# infra, not source: it lives in the gitignored .env.sh
# (scripts/build-linux.sh sources it if present, see .env.sh.example) or
# gets passed ad hoc via -DKOPT_RELAY_DEFAULT=... A real IP baked into a
# tracked file would ship with every clone/fork of this repo.
set(KOPT_RELAY_DEFAULT "127.0.0.1:8443" CACHE STRING "Default kopt_payload relay address (host:port), overridable via ini/--backend at runtime")

add_library(kopt_payload SHARED
    src/config.cpp
    src/overlay.cpp
    src/payload.cpp
    src/runtime.cpp
    src/share.cpp
    src/share_filter.cpp
    src/share_remote.cpp
    src/http3_publisher.cpp
)
target_include_directories(kopt_payload PRIVATE include)
target_compile_definitions(kopt_payload PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    KOPT_PAYLOAD_EXPORTS
    # Default relay address baked into this build -- kopt_internal.ini's
    # [Share] Endpoint and --backend still override it at runtime (see
    # config.hpp's own comment on KOPT_DEFAULT_SHARE_ENDPOINT). Override
    # per invocation with: cmake ... -DKOPT_RELAY_DEFAULT=host:port
    KOPT_DEFAULT_SHARE_ENDPOINT="${KOPT_RELAY_DEFAULT}"
)
target_link_libraries(kopt_payload PRIVATE d3d11 d3dcompiler dxgi gdi32 user32 winmm dbghelp)

# ngtcp2 + ngtcp2_crypto_ossl + OpenSSL, cross-compiled for
# x86_64-w64-mingw32 -- see scripts/fetch-quic-deps.sh (run it before
# configuring; not vendored into git, real binary blobs). nghttp3 is
# deliberately not here: the Go relay side (backend/backend_go/internal/
# quicserver) is bare QUIC, not HTTP/3, so Http3Publisher (kopt::share's
# transport, despite the name inherited from the original plan) has no use
# for an HTTP/3 framing layer.
set(KOPT_QUIC_DEPS_DIR "${CMAKE_SOURCE_DIR}/third_party/quic-mingw64")
if(NOT EXISTS "${KOPT_QUIC_DEPS_DIR}/include/ngtcp2/ngtcp2.h")
    message(FATAL_ERROR
        "third_party/quic-mingw64 is missing ngtcp2 headers -- run "
        "scripts/fetch-quic-deps.sh first (needs `nix` on PATH).")
endif()
target_include_directories(kopt_payload PRIVATE "${KOPT_QUIC_DEPS_DIR}/include")
# Explicit .dll.a paths, not target_link_directories + -l names: nixpkgs'
# mingw cross build of ngtcp2/OpenSSL is shared-library-only (no plain
# static .a for ngtcp2 at all), but kopt_payload links with -static below
# (see that flag's own comment -- mcfgthread, unrelated to these libs
# entirely). -static puts the linker in -Bstatic mode, which then refuses
# to resolve a bare "-lngtcp2" against an import library at all ("have you
# installed the static version"). A full path bypasses that search-mode
# preference outright, since it names a file, not a library to search for.
foreach(quic_lib ngtcp2 ngtcp2_crypto_ossl ssl crypto)
    target_link_libraries(kopt_payload PRIVATE "${KOPT_QUIC_DEPS_DIR}/lib/lib${quic_lib}.dll.a")
endforeach()
# ws2_32/bcrypt: Winsock (UDP socket the QUIC transport runs over) and
# BCrypt (ngtcp2_crypto_ossl's OpenSSL backend still calls into Windows'
# own CNG for a few platform RNG paths on this target).
target_link_libraries(kopt_payload PRIVATE ws2_32 bcrypt)
set_target_properties(kopt_payload PROPERTIES
    PREFIX ""
    OUTPUT_NAME "kopt_payload"
)

# Linux/Proton auto-load path: dropped next to ShooterGame.exe as
# "version.dll" (a DLL it genuinely imports -- see src/version_proxy.cpp),
# forwards every real export to version_orig.dll, and loads kopt_payload.dll
# itself. No injector process needed for this path; kopt_injector.exe stays
# for the Windows manual-attach flow.
add_library(kopt_version_proxy SHARED
    src/version_proxy.cpp
    src/version.def
)
target_compile_definitions(kopt_version_proxy PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
set_target_properties(kopt_version_proxy PROPERTIES
    PREFIX ""
    OUTPUT_NAME "version"
)

add_executable(kopt_injector src/injector.cpp)
target_compile_definitions(kopt_injector PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_link_libraries(kopt_injector PRIVATE advapi32 psapi bcrypt)

# Standalone smoke test for kopt::share's DTO builders, ChangeFilter, and
# ReporterFilter -- none of the three touch game memory or the network, so
# this needs neither D3D11 nor a live process, same "no test framework, run
# it manually, non-zero exit on failure" shape kopt_relay_test used for
# RelayClient. kopt_share_test (the network-facing counterpart, exercising
# kopt::Http3Publisher against a real QUIC/HTTP-3 handshake) lands together
# with the transport itself -- see the DTO-sharing plan, step 7.
add_executable(kopt_share_selftest
    src/share.cpp
    src/share_filter.cpp
    src/share_remote.cpp
    src/share_selftest_main.cpp
)
target_include_directories(kopt_share_selftest PRIVATE include)
target_compile_definitions(kopt_share_selftest PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# Standalone Http3Publisher smoke test -- a plain wine64 EXE (not injected
# into ShooterGame.exe) so a hard crash surfaces Wine's own SEH backtrace
# instead of vanishing inside someone else's process. Same QUIC deps/link
# setup as kopt_payload, minus the D3D11/game-hook pieces it doesn't need.
add_executable(kopt_quic_test
    src/http3_publisher.cpp
    src/quic_test_main.cpp
)
target_include_directories(kopt_quic_test PRIVATE include "${KOPT_QUIC_DEPS_DIR}/include")
target_compile_definitions(kopt_quic_test PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
foreach(quic_lib ngtcp2 ngtcp2_crypto_ossl ssl crypto)
    target_link_libraries(kopt_quic_test PRIVATE "${KOPT_QUIC_DEPS_DIR}/lib/lib${quic_lib}.dll.a")
endforeach()
target_link_libraries(kopt_quic_test PRIVATE ws2_32 bcrypt)

if(MSVC)
    set_property(TARGET kopt_payload kopt_injector kopt_version_proxy kopt_share_selftest kopt_quic_test PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(kopt_payload PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_compile_options(kopt_injector PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_compile_options(kopt_version_proxy PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_compile_options(kopt_share_selftest PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_compile_options(kopt_quic_test PRIVATE /W4 /permissive- /EHsc /utf-8)
else()
    target_compile_options(kopt_payload PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(kopt_injector PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(kopt_version_proxy PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(kopt_share_selftest PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(kopt_quic_test PRIVATE -Wall -Wextra -Wpedantic)
    # Fully static, not just -static-libgcc/-libstdc++: the nixpkgs
    # mingw-w64 cross toolchain defaults to the mcfgthread threading model,
    # whose runtime (libmcfgthread-2.dll) ships as a DLL that exists
    # nowhere in real Windows or Wine. Dynamically linking it means every
    # one of these binaries fails to load at all under Wine --
    # confirmed against a real Proton launch: version.dll's missing
    # libmcfgthread-2.dll cascaded into msi.dll -> steam.exe's own loader
    # failing (status c0000135), taking the whole launch down before
    # ShooterGame.exe ever started. `-static` pulls in libmcfgthread (or
    # whatever threading runtime the toolchain uses) statically instead,
    # for every mingw-w64 distro packaging, not just nixpkgs'.
    target_link_options(kopt_payload PRIVATE -static)
    target_link_options(kopt_injector PRIVATE -static -municode)
    target_link_options(kopt_version_proxy PRIVATE -static)
    target_link_options(kopt_share_selftest PRIVATE -static)
    target_link_options(kopt_quic_test PRIVATE -static)
endif()

foreach(target kopt_payload kopt_injector kopt_version_proxy kopt_share_selftest kopt_quic_test)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dist"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dist"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    )
    foreach(configuration Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER "${configuration}" configuration_upper)
        set_target_properties(${target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${configuration_upper} "${CMAKE_BINARY_DIR}/dist"
            LIBRARY_OUTPUT_DIRECTORY_${configuration_upper} "${CMAKE_BINARY_DIR}/dist"
            ARCHIVE_OUTPUT_DIRECTORY_${configuration_upper} "${CMAKE_BINARY_DIR}/lib"
        )
    endforeach()
endforeach()
