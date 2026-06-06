cmake_minimum_required(VERSION 3.20)

project(OpenDojo
    VERSION     0.1.0
    LANGUAGES   CXX
    DESCRIPTION "Tekken 8 practice-mode drill export/import — standalone DLL")

# ---------------------------------------------------------------------------
# Sanity: Tekken 8 is Windows x64 only. Refuse to configure for anything else.
# ---------------------------------------------------------------------------
if(NOT WIN32)
    message(FATAL_ERROR "OpenDojo is Windows-only (Tekken 8 runs on Windows).")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "Configure for x64. For example:\n"
        "    cmake -B build -A x64\n"
        "(Got ${CMAKE_SIZEOF_VOID_P}-byte pointers.)")
endif()

# ---------------------------------------------------------------------------
# Build settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# Static CRT — end users don't need the VC++ redistributable installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8)
endif()

# ---------------------------------------------------------------------------
# Dear ImGui (the menu UI). Pinned to a stable tag; fetched on first
# configure and cached under build/_deps/.
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(imgui)

# ---------------------------------------------------------------------------
# MinHook — inline function detours. We use this instead of COM-vtable
# swapping because Tekken 8 detects vtable modifications on dxgi.dll and
# crashes after some delay; inline detours patch the function bytes
# themselves and aren't caught by the same scan. Pinned to master since
# the project is mostly dormant and the tagged 1.3.3 release predates
# modern build tooling fixes.
# ---------------------------------------------------------------------------
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(minhook)
if(MSVC AND TARGET minhook)
    target_compile_options(minhook PRIVATE /W0)
endif()

# ---------------------------------------------------------------------------
# nlohmann/json — header-only JSON for the cloud client (Supabase API +
# anon-auth token persistence). Pinned to a stable tag; fetched once and
# cached under build/_deps/.
# ---------------------------------------------------------------------------
set(JSON_BuildTests        OFF CACHE INTERNAL "")
set(JSON_Install           OFF CACHE INTERNAL "")
set(JSON_MultipleHeaders   OFF CACHE INTERNAL "")
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(nlohmann_json)

add_library(imgui_static STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp")
target_include_directories(imgui_static PUBLIC
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends")
target_compile_features(imgui_static PUBLIC cxx_std_17)
# Disable the Win32 backend's own XInput polling. We feed gamepad input
# ourselves via AddKeyEvent from poll_gamepad(), reading real state via
# the trampoline so it sees through our gamepad-masking hook. Without
# this define, imgui_impl_win32's per-frame XInputGetState call sees the
# masked zero state (since it calls the OS XInputGetState which we hook)
# and overwrites our real events.
target_compile_definitions(imgui_static PRIVATE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
if(MSVC)
    # ImGui upstream produces /W4 warnings; silence them to keep our build clean.
    target_compile_options(imgui_static PRIVATE /W0)
endif()

# ---------------------------------------------------------------------------
# Proxy target. Which Windows DLL we masquerade as. dinput8 is the standard
# fighting-game modding entry point.
# ---------------------------------------------------------------------------
set(OPENDOJO_PROXY "dinput8" CACHE STRING
    "Which Windows DLL OpenDojo masquerades as. dinput8 is the default.")
set_property(CACHE OPENDOJO_PROXY PROPERTY STRINGS dinput8)

set(OPENDOJO_PROXY_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/hooks/proxy_${OPENDOJO_PROXY}.cpp")
set(OPENDOJO_PROXY_DEF "${CMAKE_CURRENT_SOURCE_DIR}/proxy_${OPENDOJO_PROXY}.def")

if(NOT EXISTS "${OPENDOJO_PROXY_SRC}" OR NOT EXISTS "${OPENDOJO_PROXY_DEF}")
    message(FATAL_ERROR
        "Unsupported OPENDOJO_PROXY=${OPENDOJO_PROXY}. Add src/hooks/proxy_${OPENDOJO_PROXY}.cpp "
        "and proxy_${OPENDOJO_PROXY}.def to the dll/ directory.")
endif()

# ---------------------------------------------------------------------------
# The DLL
# ---------------------------------------------------------------------------
add_library(opendojo SHARED
    src/main.cpp
    src/autosave.cpp
    src/commands.cpp
    src/config.cpp
    src/drill.cpp
    src/log.cpp
    src/memory.cpp
    src/players.cpp
    src/practice_state.cpp
    src/signatures.cpp
    src/slot.cpp
    src/subsystems.cpp
    src/hooks/player_hook.cpp
    src/hooks/render_hook.cpp
    src/ui/menu.cpp
    src/ui/theme.cpp
    src/cloud/api.cpp
    src/cloud/auth.cpp
    src/cloud/cloud.cpp
    src/cloud/cloud_ui.cpp
    src/cloud/handle.cpp
    src/cloud/http.cpp
    src/cloud/worker.cpp
    "${OPENDOJO_PROXY_SRC}")

target_include_directories(opendojo PRIVATE include)
target_link_libraries(opendojo PRIVATE
    imgui_static
    minhook
    nlohmann_json::nlohmann_json
    user32
    winhttp
    bcrypt
    d3d12
    dxgi
    d3dcompiler
    dxguid)
target_link_options(opendojo PRIVATE "/DEF:${OPENDOJO_PROXY_DEF}")

# ---------------------------------------------------------------------------
# Cloud coordinates. Empty defaults are intentional — a build with unset
# values still compiles; the in-game Browse/Upload features just show a
# "cloud not configured" state. Official builds set these.
# ---------------------------------------------------------------------------
set(OPENDOJO_CLOUD_URL ""
    CACHE STRING "OpenDojo Cloud endpoint URL")
set(OPENDOJO_CLOUD_PROXY_KEY ""
    CACHE STRING "OpenDojo Cloud access key")

# Tekken patch version this DLL targets. Recorded against every drill
# uploaded from this build, so the Browse tab can warn when downloading
# a drill recorded on an incompatible patch. Bump when the mod's
# signatures.cpp / offsets are updated for a new game patch.
set(OPENDOJO_TEKKEN_VERSION "3.01.01"
    CACHE STRING "Tekken 8 version this DLL was built for (free-form, e.g. 3.00.02)")

target_compile_definitions(opendojo PRIVATE
    "OPENDOJO_CLOUD_URL=\"${OPENDOJO_CLOUD_URL}\""
    "OPENDOJO_CLOUD_PROXY_KEY=\"${OPENDOJO_CLOUD_PROXY_KEY}\""
    "OPENDOJO_TEKKEN_VERSION=\"${OPENDOJO_TEKKEN_VERSION}\"")

# Output filename = the proxy DLL name we're impersonating, no "lib" prefix.
set_target_properties(opendojo PROPERTIES
    OUTPUT_NAME "${OPENDOJO_PROXY}"
    PREFIX      "")

# ---------------------------------------------------------------------------
# clang-format: run before each build so the tree stays formatted. Hints
# cover the VS BuildTools / Community LLVM install paths and a PATH lookup.
# If clang-format isn't found we skip silently (CI / fresh machines without
# LLVM still build).
# ---------------------------------------------------------------------------
find_program(OPENDOJO_CLANG_FORMAT
    NAMES clang-format
    HINTS
        "C:/Program Files (x86)/Microsoft Visual Studio/2019/BuildTools/VC/Tools/Llvm/x64/bin"
        "C:/Program Files (x86)/Microsoft Visual Studio/2019/BuildTools/VC/Tools/Llvm/bin"
        "C:/Program Files (x86)/Microsoft Visual Studio/2019/Community/VC/Tools/Llvm/x64/bin"
        "C:/Program Files (x86)/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/x64/bin"
        "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/x64/bin"
        "C:/Program Files/LLVM/bin")

if(OPENDOJO_CLANG_FORMAT)
    file(GLOB OPENDOJO_FORMAT_SRCS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/cloud/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/hooks/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/cloud/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/hooks/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ui/*.hpp")
    add_custom_command(TARGET opendojo PRE_BUILD
        COMMAND ${OPENDOJO_CLANG_FORMAT} -i -style=file ${OPENDOJO_FORMAT_SRCS}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "clang-format: formatting dll/src"
        VERBATIM)
    message(STATUS "OpenDojo: clang-format = ${OPENDOJO_CLANG_FORMAT}")
else()
    message(STATUS "OpenDojo: clang-format not found — skipping format step")
endif()

# ---------------------------------------------------------------------------
# Install: convenience target so iteration is one command. Defaults to
# <build>/install — override with -DCMAKE_INSTALL_PREFIX="<game>\Polaris\..."
# to copy directly into the game folder.
# ---------------------------------------------------------------------------
install(TARGETS opendojo RUNTIME DESTINATION ".")

# ---------------------------------------------------------------------------
# Post-build deploy: copy the freshly built DLL into the Tekken game folder
# so `cmake --build` lands a working drop in one step. Skipped if the path
# doesn't exist (e.g. on a clean checkout where Tekken isn't installed at
# this exact path). Override with -DOPENDOJO_DEPLOY_DIR=... at configure time.
#
# If Tekken is running, the copy will fail and so will the build — that's
# the intended signal to "close the game before redeploying."
# ---------------------------------------------------------------------------
set(OPENDOJO_DEPLOY_DIR
    "E:/Steam/steamapps/common/TEKKEN 8/Polaris/Binaries/Win64"
    CACHE PATH "Tekken 8 install dir to copy the built DLL into after each build")

if(OPENDOJO_DEPLOY_DIR AND EXISTS "${OPENDOJO_DEPLOY_DIR}")
    add_custom_command(TARGET opendojo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:opendojo>"
            "${OPENDOJO_DEPLOY_DIR}/$<TARGET_FILE_NAME:opendojo>"
        COMMENT "Deploying $<TARGET_FILE_NAME:opendojo> to ${OPENDOJO_DEPLOY_DIR}"
        VERBATIM)
endif()
