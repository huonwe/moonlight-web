# ============================================================================
# mw-click-target — the "ideal game" of plan « attente » (A1).
#
# On a click it draws the latency flag (the bands and the place of
# backend/src/LatencyFlag.cpp) and presents at once, then logs when it got the
# click, when it presented and when the OS says the picture reached the screen,
# on the clock MoonlightWeb stamps with. What a game adds to the click's way
# through the host is then the game's own, not the bench's.
#
# Windows (D3D12, flip model, tearing allowed) and macOS (Metal, a
# CAMetalLayer); Linux (Vulkan on Wayland) is the plan's next step, on the same
# options and log. Nothing but cleared or copied rectangles is drawn: no shader.
#
# Built on demand with -DMW_BUILD_TOOLS=ON, never installed nor shipped. It
# also configures on its own: `cmake -S backend/native-host/tools/click-target`.
# ============================================================================

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.21)
    if(APPLE)
        project(mw-click-target LANGUAGES CXX OBJCXX)
    else()
        project(mw-click-target LANGUAGES CXX)
    endif()
endif()

if(APPLE)
    add_executable(mw-click-target src/main.cpp src/ClickTargetMac.mm)
    target_compile_features(mw-click-target PRIVATE cxx_std_17)
    target_compile_options(mw-click-target PRIVATE -Wall -Wextra
                                                   $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_link_libraries(mw-click-target PRIVATE "-framework AppKit" "-framework Metal"
                                                  "-framework QuartzCore" "-framework CoreGraphics")
    return()
endif()

if(NOT WIN32)
    message(STATUS "mw-click-target skipped: Windows and macOS only for now")
    return()
endif()

add_executable(mw-click-target src/main.cpp src/ClickTargetWin.cpp)
target_compile_features(mw-click-target PRIVATE cxx_std_17)
target_compile_definitions(mw-click-target PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
                                                   _CRT_SECURE_NO_WARNINGS)
target_link_libraries(mw-click-target PRIVATE d3d12 dxgi user32 winmm)
if(MSVC)
    target_compile_options(mw-click-target PRIVATE /W4 /utf-8)
endif()
