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

add_executable(kopt_injector src/injector.cpp)
target_compile_definitions(kopt_injector PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_link_libraries(kopt_injector PRIVATE advapi32 psapi bcrypt shell32 user32)

add_executable(kopt_quick_injector WIN32 src/injector.cpp)
target_compile_definitions(kopt_quick_injector PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    KOPT_QUICK_GUI
)
target_link_libraries(kopt_quick_injector PRIVATE advapi32 psapi bcrypt shell32 user32)
set_target_properties(kopt_quick_injector PROPERTIES OUTPUT_NAME "KOPT_Inject")

if(MSVC)
    set_property(TARGET kopt_payload kopt_injector kopt_quick_injector PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(kopt_payload PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_compile_options(kopt_injector PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_compile_options(kopt_quick_injector PRIVATE /W4 /permissive- /EHsc /utf-8)
else()
    target_compile_options(kopt_payload PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(kopt_injector PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(kopt_quick_injector PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(kopt_payload PRIVATE -static-libgcc -static-libstdc++)
    target_link_options(kopt_injector PRIVATE -static-libgcc -static-libstdc++ -municode)
    target_link_options(kopt_quick_injector PRIVATE -static-libgcc -static-libstdc++ -municode)
endif()

foreach(target kopt_payload kopt_injector kopt_quick_injector)
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
