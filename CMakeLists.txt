# =============================================================================
# FightSight - FGC Telemetry & Automation Engine
# Phase 2: + OpenCV CPU readback pipeline
# =============================================================================
#
# Build (from project root):
#   cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
#         -DOpenCV_DIR=C:/opencv/build
#   cmake --build build --config Release
#
# Run:
#   .\build\Release\FightSight.exe
#
# Dependencies fetched automatically: ImGui (docking), nlohmann/json, Spout2.
#
# OpenCV is the one dependency you install separately. Two supported paths:
#   1) Prebuilt official binaries from https://opencv.org/releases/
#      Extract to e.g. C:/opencv, then pass -DOpenCV_DIR=C:/opencv/build
#   2) vcpkg:  vcpkg install opencv4:x64-windows
#      Then pass -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
#
# OpenCV runtime DLLs are auto-copied next to FightSight.exe via
# $<TARGET_RUNTIME_DLLS>, so no PATH fiddling required.
# =============================================================================

cmake_minimum_required(VERSION 3.21)
project(FightSight LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_GENERATOR_PLATFORM STREQUAL "x64" AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "FightSight requires x64. Re-run with -A x64.")
endif()

if(NOT WIN32)
    message(FATAL_ERROR "FightSight is Windows-only (DX11 + Spout2).")
endif()

# Release-by-default for the perf target.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# -----------------------------------------------------------------------------
# Dependencies
# -----------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)

# --- Dear ImGui (docking branch) ---
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler)

# --- nlohmann/json ---
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

# --- Spout2 SDK (DX11 subset only) ---
# SpoutDX depends on a handful of utility .cpps that live in the sibling
# SpoutGL folder. We list them explicitly and build one static lib so we
# can ignore the rest of the Spout2 repo (OpenGL, examples, demos, etc.).
FetchContent_Declare(
    spout2
    GIT_REPOSITORY https://github.com/leadedge/Spout2.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(spout2)
if(NOT spout2_POPULATED)
    FetchContent_Populate(spout2)
endif()

set(SPOUTSDK_DIR ${spout2_SOURCE_DIR}/SPOUTSDK)
set(SPOUTGL_DIR  ${SPOUTSDK_DIR}/SpoutGL)
set(SPOUTDX_DIR  ${SPOUTSDK_DIR}/SpoutDirectX/SpoutDX)

set(SPOUT_SOURCES
    ${SPOUTGL_DIR}/SpoutCommon.h
    ${SPOUTGL_DIR}/SpoutCopy.cpp
    ${SPOUTGL_DIR}/SpoutCopy.h
    ${SPOUTGL_DIR}/SpoutDirectX.cpp
    ${SPOUTGL_DIR}/SpoutDirectX.h
    ${SPOUTGL_DIR}/SpoutFrameCount.cpp
    ${SPOUTGL_DIR}/SpoutFrameCount.h
    ${SPOUTGL_DIR}/SpoutSenderNames.cpp
    ${SPOUTGL_DIR}/SpoutSenderNames.h
    ${SPOUTGL_DIR}/SpoutSharedMemory.cpp
    ${SPOUTGL_DIR}/SpoutSharedMemory.h
    ${SPOUTGL_DIR}/SpoutUtils.cpp
    ${SPOUTGL_DIR}/SpoutUtils.h
    ${SPOUTDX_DIR}/SpoutDX.cpp
    ${SPOUTDX_DIR}/SpoutDX.h
)
add_library(SpoutDX STATIC ${SPOUT_SOURCES})
target_include_directories(SpoutDX PUBLIC ${SPOUTGL_DIR} ${SPOUTDX_DIR})
target_link_libraries(SpoutDX PUBLIC d3d11 dxgi)

# --- OpenCV (user-installed; see header comment) ---
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)
message(STATUS "FightSight: OpenCV ${OpenCV_VERSION} at ${OpenCV_DIR}")

# --- Tesseract OCR (via vcpkg) ---
# Install with:  vcpkg install tesseract:x64-windows
# Then configure with:
#   -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
# Leptonica is pulled in transitively by tesseract's vcpkg port; we don't
# need a separate find_package for it. vcpkg's tesseract port exports a
# config-style package, so CONFIG REQUIRED is the canonical form.
find_package(Tesseract CONFIG REQUIRED)
message(STATUS "FightSight: Tesseract found")
# Target name compatibility: newer vcpkg ports expose Tesseract::Tesseract
# directly; older ports use Tesseract::libtesseract. If you hit a "target
# does not exist" link error, swap the name in the target_link_libraries
# block below.

# --- Asio (standalone, header-only) ---
FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG        asio-1-28-0
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(asio)
if(NOT asio_POPULATED)
    FetchContent_Populate(asio)
endif()

add_library(asio_standalone INTERFACE)
target_include_directories(asio_standalone INTERFACE ${asio_SOURCE_DIR}/asio/include)
target_compile_definitions(asio_standalone INTERFACE
    ASIO_STANDALONE
    _WIN32_WINNT=0x0601
)

# --- websocketpp (header-only) ---
FetchContent_Declare(
    websocketpp
    GIT_REPOSITORY https://github.com/zaphoyd/websocketpp.git
    GIT_TAG        0.8.2
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(websocketpp)
if(NOT websocketpp_POPULATED)
    FetchContent_Populate(websocketpp)
endif()

add_library(websocketpp INTERFACE)
target_include_directories(websocketpp INTERFACE ${websocketpp_SOURCE_DIR})
target_link_libraries(websocketpp INTERFACE asio_standalone)
target_compile_definitions(websocketpp INTERFACE
    _WEBSOCKETPP_CPP11_STL_
    ASIO_STANDALONE
)

# -----------------------------------------------------------------------------
# FightSight executable
# -----------------------------------------------------------------------------
add_executable(FightSight WIN32
    src/main.cpp
    src/SpoutReceiverDX.cpp
    src/ConfigManager.cpp
    src/FrameProcessor.cpp
    src/RoiManager.cpp
    src/RoiEditor.cpp
    src/WsClient.cpp
    src/CvWorker.cpp
    src/ProfileManager.cpp
    src/GhostTextureCache.cpp

    # Win32 resource script - embeds the application icon (icon.ico)
    # into the compiled .exe so Explorer / taskbar / Alt+Tab all show
    # the FightSight branding. CMake hands .rc files to the MSVC RC
    # compiler (rc.exe) automatically; the resulting .res is linked
    # into the executable like any other object file.
    src/resource.rc
)

# Tell CMake to invoke rc.exe on .rc files (MSVC only path). This is
# the default behavior, but stating it explicitly guards against future
# generator changes and silently breaking the icon embed.
if(MSVC)
    enable_language(RC)
    set_source_files_properties(src/resource.rc PROPERTIES LANGUAGE RC)
endif()

target_include_directories(FightSight PRIVATE include)

target_link_libraries(FightSight PRIVATE
    imgui
    SpoutDX
    nlohmann_json::nlohmann_json
    opencv_core
    opencv_imgproc
    opencv_imgcodecs
    websocketpp
    Tesseract::libtesseract
    d3d11
    dxgi
    d3dcompiler
    comdlg32   # Win32 GetOpenFileNameW for View > Load Reference Image
)

target_compile_definitions(FightSight PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if(MSVC)
    target_compile_options(FightSight PRIVATE /W4 /permissive- /MP)
    target_compile_options(imgui      PRIVATE /MP)
    target_compile_options(SpoutDX    PRIVATE /MP /wd4100 /wd4244 /wd4267)
endif()

# Copy /config and /templates next to the exe so relative paths Just Work.
# /tessdata is created empty - the user must drop `eng.traineddata`
# (downloaded from https://github.com/tesseract-ocr/tessdata) into it
# before any OCR-linked ROIs will resolve text.
add_custom_command(TARGET FightSight POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/config $<TARGET_FILE_DIR:FightSight>/config
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:FightSight>/templates
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:FightSight>/tessdata
)

# Auto-copy OpenCV (and any other) runtime DLLs next to the exe.
# Requires CMake 3.21+; works with OpenCV's IMPORTED targets out of the box.
add_custom_command(TARGET FightSight POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:FightSight> $<TARGET_FILE_DIR:FightSight>
    COMMAND_EXPAND_LISTS
)
