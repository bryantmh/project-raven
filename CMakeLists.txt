cmake_minimum_required(VERSION 3.20)
project(ProjectRaven LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

if (NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "Project Raven needs clang (the shim layer relies on __attribute__((sysv_abi))).")
endif()

# SDL 2's SDL_endian.h redefines _m_prefetch, a builtin in recent clang.
add_compile_definitions(__PRFCHWINTRIN_H)
include(FetchContent)
set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MI_OVERRIDE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(mimalloc GIT_REPOSITORY https://github.com/microsoft/mimalloc GIT_TAG v2.1.7 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(mimalloc)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL2 GIT_REPOSITORY https://github.com/libsdl-org/SDL GIT_TAG release-2.30.9 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(SDL2)

# Dear ImGui for the recomp settings menu (src/ui), over SDL2 + OpenGL 3.
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui GIT_TAG v1.91.5 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)
add_library(imgui STATIC ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL2::SDL2-static)


file(GLOB_RECURSE RAVEN_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.c)
list(APPEND RAVEN_SOURCES src/raven.rc)  # the app icon
add_executable(raven ${RAVEN_SOURCES})
target_include_directories(raven PRIVATE src)
target_compile_definitions(raven PRIVATE SDL_MAIN_HANDLED NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_compile_options(raven PRIVATE -mfsgsbase -Wno-unused-parameter -Wno-c99-designator -Wno-gnu-zero-variadic-macro-arguments)
target_link_libraries(raven PRIVATE imgui mimalloc-static SDL2::SDL2-static opengl32 glu32 synchronization ole32 shell32 bcrypt ws2_32 dbghelp winmm windowscodecs)
# Keep the game's fixed range (0x400000-0x2400000) free: the exe goes high, with ASLR.
# A windowed app (no console); the log goes to %APPDATA%\\ProjectRaven\\raven.log and to stdout when redirected.
target_link_options(raven PRIVATE -Wl,/BASE:0x140000000 -Wl,/STACK:16777216 -Wl,/SUBSYSTEM:WINDOWS -Wl,/ENTRY:mainCRTStartup)
# runtime/ (Linux C++ runtime from tools/fetch_runtime.py, the FMOD DLL) goes next to raven.exe.
add_custom_command(TARGET raven POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/runtime $<TARGET_FILE_DIR:raven>/runtime)
# assets/ (the boot image) goes next to raven.exe.
add_custom_command(TARGET raven POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:raven>/assets)
