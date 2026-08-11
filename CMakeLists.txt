cmake_minimum_required(VERSION 3.22)
include("${CMAKE_CURRENT_LIST_DIR}/cmake/PjVersions.cmake")
pj_read_versions("${CMAKE_CURRENT_LIST_DIR}/versions.env")
if(NOT PJ_VERSION AND DEFINED ENV{PJ_VERSION} AND NOT "$ENV{PJ_VERSION}" STREQUAL "")
    set(PJ_VERSION "$ENV{PJ_VERSION}")
endif()
if(NOT PJ_VERSION)
    set(PJ_VERSION "${PJ_APP_VERSION}")
endif()
set(_PJ_PROJECT_VERSION_INPUT "${PJ_VERSION}")
string(REGEX REPLACE "^[vV]" "" _PJ_PROJECT_VERSION_INPUT "${_PJ_PROJECT_VERSION_INPUT}")
string(REGEX MATCH "^[0-9]+(\\.[0-9]+)?(\\.[0-9]+)?(\\.[0-9]+)?" PJ_PROJECT_VERSION "${_PJ_PROJECT_VERSION_INPUT}")
if(NOT PJ_PROJECT_VERSION)
    message(FATAL_ERROR "PJ_VERSION='${PJ_VERSION}' must start with a numeric version, optionally prefixed with v")
endif()
project(plotjuggler4 VERSION "${PJ_PROJECT_VERSION}" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Packaging channel stamped into the anonymous telemetry ping. A channel token
# (like gcc_64/msvc2022_64), not a version — deliberately NOT in versions.env.
set(PJ_INSTALLATION "source" CACHE STRING "Packaging channel: source, appimage, deb, or windows")

configure_file(cmake/pj_version.h.in ${CMAKE_BINARY_DIR}/generated/pj_version.h @ONLY)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

option(PJ4_BUILD_APP "Build the pj_app desktop shell" ON)
option(PJ_ENABLE_SANITIZERS "Enable AddressSanitizer instrumentation (opt-in, any build type)" OFF)
option(PJ_ENABLE_TSAN "Enable ThreadSanitizer instrumentation (opt-in, any build type)" OFF)
# Standalone dev tools (pj_scene2D/tools, e.g. extract_frame). Off by default so
# they don't cost a default/CI build; configure with -DPJ_BUILD_TOOLS=ON to build.
option(PJ_BUILD_TOOLS "Build pj_scene2D dev tools" OFF)
# Standalone GPLv2 render helper (raster_helper, links vendored doomgeneric).
# Off by default: PlotJuggler links none of it, and compiling the doomgeneric C
# engine only costs time on a default/CI build. Configure with
# -DPJ_BUILD_RASTER_HELPER=ON to build it (also requires the vendored source).
option(PJ_BUILD_RASTER_HELPER "Build the standalone pj-raster-helper (doomgeneric)" OFF)

# Qt for WebAssembly is selected by the Qt/Emscripten toolchain. Keep this
# derived from EMSCRIPTEN so a cache option cannot accidentally create a
# desktop compiler + wasm source-guard combination (or the inverse).
if(EMSCRIPTEN)
    set(PJ_TARGET_WASM ON)
    add_compile_definitions(PJ_TARGET_WASM PJ_STATIC_PLUGINS)
    option(
        PJ_WASM_ENABLE_INGRESS_PROBE
        "Link internal observation probes used by browser acceptance tests"
        OFF)

    # Qt's wasm_multithread kit links one shared-memory module. Every object in
    # that module, including FetchContent dependencies with no Qt linkage, must
    # be compiled with the atomics/bulk-memory feature set selected by
    # Emscripten's -pthread flag.
    add_compile_options(-pthread)
    add_link_options(-pthread)

    # Conan packages in the desktop graph are native binaries. The wasm build
    # resolves its cross-compiled subset through a pinned FetchContent provider.
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/PjWasmDependencies.cmake)
endif()

# Foundation build options. PJ_BUILD_TESTS feeds both the plotjuggler_sdk SDK
# submodule and the pj_datastore module. The rest stay off for the PJ4 app
# build: the SDK's ported-plugins collection, and pj_datastore's parquet
# example (PJ_BUILD_PARQUET_IMPORT_EXAMPLE is now a pj_datastore option, pinned
# off here before that subdirectory is added).
# Default ON (local and CI builds need the suite); release/packaging
# pipelines pass -DPJ_BUILD_TESTS=OFF — they never run ctest, and the 271
# per-fixture static test binaries are ~17 GB of an otherwise ~5 GB tree.
if(EMSCRIPTEN)
    set(_PJ_BUILD_TESTS_DEFAULT OFF)
else()
    set(_PJ_BUILD_TESTS_DEFAULT ON)
endif()
set(PJ_BUILD_TESTS ${_PJ_BUILD_TESTS_DEFAULT} CACHE BOOL "Build foundation (SDK + pj_datastore) tests")
unset(_PJ_BUILD_TESTS_DEFAULT)
option(PJ_BUILD_DEMOS "Build the pj_scene3D standalone dev demos" ON)
# The widget family needs an OpenGL 4.5 core profile: withCoreGlFunctions() throws
# when it cannot resolve, and scene_hdr_fbo, gl/gpu_profiler and
# passes/pointcloud_aabb_reducer all depend on it. Apple caps OpenGL at 4.1, so the
# module cannot run on macOS regardless of whether it compiles. Off on APPLE until a
# 4.1 or Metal backend exists; pj_scene3d_core is Qt/GL-free and still builds, and
# consumers guard the widget target with TARGET_EXISTS.
if(APPLE)
    set(_PJ_BUILD_SCENE3D_WIDGETS_DEFAULT OFF)
else()
    set(_PJ_BUILD_SCENE3D_WIDGETS_DEFAULT ON)
endif()
option(PJ_BUILD_SCENE3D_WIDGETS "Build the pj_scene3D OpenGL widget family"
       ${_PJ_BUILD_SCENE3D_WIDGETS_DEFAULT})
unset(_PJ_BUILD_SCENE3D_WIDGETS_DEFAULT)
set(PJ_BUILD_PARQUET_IMPORT_EXAMPLE OFF CACHE BOOL "" FORCE)
set(PJ_BUILD_PORTED_PLUGINS OFF CACHE BOOL "" FORCE)

if(PJ_BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)
endif()

# ---------------------------------------------------------------------------
# Compiler warnings
# ---------------------------------------------------------------------------

if(MSVC)
    set(PJ_WARNING_FLAGS /W4 /WX /permissive-)
else()
    set(PJ_WARNING_FLAGS
        -Wall -Wextra -Wshadow -Wnon-virtual-dtor -Wold-style-cast
        -Wcast-qual -Wconversion -Woverloaded-virtual -Wpedantic
    )
    # wasm32 makes size_t 32-bit and surfaces the SDK's explicitly tracked
    # uint64_t ABI conversions. Keep the warnings visible, but do not turn the
    # cross-compile bring-up into an unrelated warning-cleanup project.
    #
    # Apple clang runs ahead of the compilers Linux and Windows CI pin, so it
    # diagnoses constructs they accept (e.g. -Wunneeded-internal-declaration on a
    # self-recursive static helper). Same rule as wasm: warnings stay visible but
    # do not gate the build. Re-enable once a macOS job pins a tracked toolchain.
    if(NOT EMSCRIPTEN AND NOT APPLE)
        list(APPEND PJ_WARNING_FLAGS -Werror)
    endif()
endif()

# Keep frame pointers in all PJ4-owned targets so `perf record --call-graph fp`
# can unwind without DWARF.
if(NOT MSVC)
    add_compile_options(-fno-omit-frame-pointer)
endif()

# ---------------------------------------------------------------------------
# Qt
# ---------------------------------------------------------------------------

if(EMSCRIPTEN)
    set(PJ_QT_REQUIRED_VERSION ${PJ_WASM_QT_VERSION})
    # The installed wasm kit is pinned by versions.env. OpenGLWidgets is
    # omitted: QOpenGLWidget is unsupported by Qt/WASM and accelerated browser
    # surfaces are ported to QRhiWidget/WebGL instead.
    find_package(Qt6 ${PJ_QT_REQUIRED_VERSION} REQUIRED COMPONENTS
        Core Gui GuiPrivate Widgets Concurrent Network Xml Svg SvgWidgets PrintSupport ShaderTools)
else()
    set(PJ_QT_REQUIRED_VERSION ${PJ_QT_VERSION})
    find_package(Qt6 ${PJ_QT_REQUIRED_VERSION} REQUIRED COMPONENTS Core Gui Widgets Network OpenGLWidgets Xml Svg SvgWidgets)
endif()
find_package(fmt REQUIRED)

# ---------------------------------------------------------------------------
# Foundation (Level 0): the plotjuggler_sdk SDK submodule (pj_base + pj_plugins)
# plus pj_datastore, the columnar storage engine. pj_datastore used to live in
# the plotjuggler_sdk submodule; it now lives in the
# app repo (plugins reach it only through the pj_base C ABI, never by linking
# it). pj_datastore MUST be added AFTER the submodule so the pj_base /
# pj_internal_fmt targets it links already exist.
# ---------------------------------------------------------------------------

add_subdirectory(plotjuggler_sdk)
if(EMSCRIPTEN)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/PjWasmPluginCommon.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/PjWasmCsv.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/PjWasmMcap.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/PjWasmRos.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/PjWasmProtobuf.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/PjWasmTransformEditor.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/PjWasmDummyStream.cmake)
endif()
# pj_datastore: columnar store + DerivedEngine + the data-processor execution
# substrate (DataProcessor base + ProcessorSisoAdapter, formerly pj_proc).
add_subdirectory(pj_datastore)
# pj_scripting: Luau filter engine for Data Processors (self-describing filter
# classes). Depends on pj_datastore (added above) + Conan Luau. See its CLAUDE.md.
if(NOT EMSCRIPTEN OR PJ_WASM_WITH_LUAU)
    add_subdirectory(pj_scripting)
endif()

# ---------------------------------------------------------------------------
# Vendored third-party
# ---------------------------------------------------------------------------

add_subdirectory(3rdparty/qwt)
add_subdirectory(3rdparty/nanocdr)

# Qt-Advanced-Docking submodule (PlotJuggler/Qt-Advanced-Docking-System fork
# tracking upstream master + the Wayland fix + WASM Unix guards on
# feature/wasm-unix-guards). Pin the
# version to skip the upstream root CMake's git_describe call (which fails in
# a submodule checkout), build static, and skip examples/demos.
set(ADS_VERSION "4.5.0" CACHE STRING "" FORCE)
set(BUILD_STATIC ON CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(3rdparty/Qt-Advanced-Docking)
add_library(qt_advanced_docking ALIAS qtadvanceddocking-qt6)

# ---------------------------------------------------------------------------
# Sanitizers (root-owned modules only)
# ---------------------------------------------------------------------------

# Opt-in and independent of build type: a fast RelWithDebInfo build can carry the
# sanitizer without a Debug rebuild of the whole Conan dependency graph (the TSan
# foundation job + `./build.sh --tsan` rely on this). ASan and TSan are mutually
# exclusive instrumentations.
if(PJ_ENABLE_SANITIZERS AND PJ_ENABLE_TSAN)
    message(FATAL_ERROR "PJ_ENABLE_SANITIZERS (ASan) and PJ_ENABLE_TSAN are mutually exclusive")
endif()
if(PJ_ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
    add_compile_definitions(PJ_ASAN_ACTIVE)
endif()
if(PJ_ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

# ---------------------------------------------------------------------------
# Root-owned PlotJuggler modules
# ---------------------------------------------------------------------------

# pj_scene2D depends on pj_datastore, which is now always built (above).
if(NOT EMSCRIPTEN OR PJ_WASM_WITH_SCENE2D)
    add_subdirectory(pj_scene2D/core)
    add_subdirectory(pj_scene2D/widgets)
endif()
if(PJ_BUILD_TOOLS)
    add_subdirectory(pj_scene2D/tools)
endif()
if(NOT EMSCRIPTEN OR PJ_WASM_WITH_SCENE3D)
    add_subdirectory(pj_scene3D/core)
    if(PJ_BUILD_SCENE3D_WIDGETS)
        add_subdirectory(pj_scene3D/widgets)
    endif()
endif()
if(EMSCRIPTEN AND PJ_WASM_SCENE3D_CAPABILITY_PROBE)
    # W12 is a retained measurement executable, not part of the product and not
    # an implicit request to build the production Scene3D renderer.
    add_subdirectory(pj_scene3D/wasm_probe)
endif()
if(TARGET pj_scene2d_widgets)
    add_compile_definitions(PJ_WITH_SCENE2D)
endif()
if(TARGET pj_scene3d_widgets)
    add_compile_definitions(PJ_WITH_SCENE3D)
endif()
# The demos link pj_scene3d_widgets, so they build only where that target exists.
if(PJ_BUILD_DEMOS AND NOT EMSCRIPTEN AND TARGET pj_scene3d_widgets)
    add_subdirectory(pj_scene3D/demos)
endif()
add_subdirectory(pj_dialog_host)
add_subdirectory(pj_runtime)
# After pj_runtime: pj_marketplace's extension_manager_test guards itself with
# if(TARGET mock_data_source_v2_plugin ...), and that fixture is defined by
# pj_runtime. Configured earlier, the guard silently evaluated false and the test
# executable was never created. Link order is unaffected — CMake derives build
# order from the dependency graph (pj_runtime links pj_marketplace), so only
# configure-time target visibility depends on the position of this call.
if(NOT EMSCRIPTEN OR PJ_WASM_WITH_MARKETPLACE)
    add_subdirectory(pj_marketplace)
endif()
add_subdirectory(pj_widgets)
add_subdirectory(pj_scene_common)
add_subdirectory(pj_plotting)

if(PJ4_BUILD_APP)
    add_subdirectory(pj_app)
endif()

# External GPL render helper (separate executable; PlotJuggler links none of it).
# Opt-in (PJ_BUILD_RASTER_HELPER, off by default) and only when the vendored
# engine source is present.
if(PJ_BUILD_RASTER_HELPER)
    if(EXISTS "${CMAKE_SOURCE_DIR}/3rdparty/doomgeneric/doomgeneric/doomgeneric.h")
        add_subdirectory(raster_helper)
    else()
        message(WARNING
            "PJ_BUILD_RASTER_HELPER=ON but 3rdparty/doomgeneric source is missing; "
            "run 'git submodule update --init --recursive'. Skipping pj-raster-helper.")
    endif()
endif()

if(TARGET pj-raster-helper)
    install(TARGETS pj-raster-helper RUNTIME DESTINATION thirdparty/retro)
    # GPLv2 / shareware compliance: the helper binary must ship with its license
    # text, the written offer for its corresponding source, and the WAD's
    # shareware terms. These travel next to the binary in the installed bundle.
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/retro/COPYING"
        "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/retro/SOURCE-OFFER.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/retro/SHAREWARE-LICENSE.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/retro/README.md"
        DESTINATION thirdparty/retro)
endif()
