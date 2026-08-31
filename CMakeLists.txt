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

add_library(kopt_payload SHARED
    src/config.cpp
    src/overlay.cpp
    src/payload.cpp
    src/runtime.cpp
)
target_include_directories(kopt_payload PRIVATE include)
target_compile_definitions(kopt_payload PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    KOPT_PAYLOAD_EXPORTS
)
target_link_libraries(kopt_payload PRIVATE d3d11 d3dcompiler dxgi gdi32 user32 winmm dbghelp)
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

if(MSVC)
    set_property(TARGET kopt_payload kopt_injector kopt_version_proxy PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(kopt_payload PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_compile_options(kopt_injector PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_compile_options(kopt_version_proxy PRIVATE /W4 /permissive- /EHsc /utf-8)
else()
    target_compile_options(kopt_payload PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(kopt_injector PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(kopt_version_proxy PRIVATE -Wall -Wextra -Wpedantic)
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
endif()

foreach(target kopt_payload kopt_injector kopt_version_proxy)
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
