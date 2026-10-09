cmake_minimum_required(VERSION 3.21)

project(LiveDesktop VERSION 0.1.0 LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "LiveDesktop requires the MSVC toolchain (C++/WinRT + Windows SDK).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(LiveDesktop
    src/main.cpp
    src/Log.cpp
    src/DesktopHost.cpp
    src/DesktopIcons.cpp
    src/Scene.cpp
    src/Effects.cpp
    src/AppIcon.cpp
    src/Util.cpp
    src/Config.cpp
    src/Provider.cpp
    src/SessionsProvider.cpp
    src/GitHubProvider.cpp
    src/SystemProviders.cpp
    src/CalendarProvider.cpp
    src/Visuals.cpp
    src/Graphics.cpp
    src/LiveDesktop.manifest
)

target_precompile_headers(LiveDesktop PRIVATE src/pch.h)

# C++/WinRT projection headers ship with the Windows SDK (Include\<ver>\cppwinrt).
# The VS developer prompt normally puts them on INCLUDE already; add them explicitly
# as a fallback so the build also works from a plain CMake configure.
if(DEFINED ENV{WindowsSdkDir} AND DEFINED ENV{WindowsSDKVersion})
    string(REGEX REPLACE "[\\/]+$" "" _sdkver "$ENV{WindowsSDKVersion}")
    set(_cppwinrt "$ENV{WindowsSdkDir}Include/${_sdkver}/cppwinrt")
    if(EXISTS "${_cppwinrt}/winrt/base.h")
        target_include_directories(LiveDesktop SYSTEM PRIVATE "${_cppwinrt}")
    endif()
endif()

target_compile_definitions(LiveDesktop PRIVATE
    UNICODE _UNICODE NOMINMAX
    _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A00000C)

target_compile_options(LiveDesktop PRIVATE /W4 /permissive- /EHsc /utf-8 /bigobj /Zc:__cplusplus)

# Console subsystem on purpose: live log output + Ctrl+C handling.
target_link_libraries(LiveDesktop PRIVATE
    windowsapp CoreMessaging user32 gdi32 shell32 ole32 oleaut32 uuid dwmapi
    d3d11 d2d1 dwrite dxgi iphlpapi pdh shcore ws2_32 advapi32)

# Static CRT: the POC runs from the build folder without VC++ redist concerns.
set_property(TARGET LiveDesktop PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
