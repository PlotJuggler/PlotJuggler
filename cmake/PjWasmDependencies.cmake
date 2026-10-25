# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0

# Cross-compiled dependency subset for Qt for WebAssembly. Desktop continues to
# resolve the same libraries from Conan; this file is included only when the Qt
# toolchain defines EMSCRIPTEN.
include_guard(GLOBAL)
include(FetchContent)

set(FETCHCONTENT_QUIET OFF)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

# A browser application does not build dependency tests/tools. Force this before
# population so a reconfigure cannot inherit BUILD_TESTING=ON from a dependency.
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

option(PJ_WASM_WITH_LUAU "Build Luau Data Processors in the wasm app" ON)
option(PJ_WASM_WITH_CSV "Build the official CSV importer into the wasm app" ON)
option(PJ_WASM_WITH_MCAP "Build the official MCAP container importer into the wasm app" ON)
option(PJ_WASM_WITH_ROS "Build the official ROS1/ROS2 message parser into the wasm app" ON)
option(PJ_WASM_WITH_PROTOBUF "Build the official Protobuf/Foxglove parser into the wasm app" ON)
option(PJ_WASM_WITH_TRANSFORM_EDITOR "Build the official Transform Editor toolbox into the wasm app" ON)
option(PJ_WASM_WITH_DUMMY_STREAM "Build the official Dummy Streamer into the wasm app" ON)
option(PJ_WASM_WITH_PJ_BRIDGE "Build the official PlotJuggler Bridge WebSocket streamer into the wasm app" ON)
option(PJ_WASM_WITH_JSON "Build the official JSON message parser into the wasm app" ON)
option(PJ_WASM_WITH_DATA_TAMER "Build the official DataTamer message parser into the wasm app" ON)
option(PJ_WASM_WITH_3D_LOADER "Build the official PCD/PLY point-cloud and mesh importer into the wasm app" ON)
option(PJ_WASM_WITH_QUATERNION "Build the official Quaternion toolbox into the wasm app" ON)
option(PJ_WASM_WITH_FFT "Build the official FFT toolbox into the wasm app" ON)
option(PJ_WASM_WITH_ANOMALY_DETECTOR "Build the official Anomaly Detector toolbox into the wasm app" ON)
option(PJ_WASM_WITH_SCENE2D "Build the accelerated Scene2D wasm port" ON)
option(PJ_WASM_WITH_SCENE3D "Build the accelerated Scene3D wasm port" ON)
option(PJ_WASM_WITH_VIDEO
    "Build software video decode (cross-compiled LGPL FFmpeg + dav1d) into the Scene2D wasm port" ON)
option(PJ_WASM_WITH_COMPRESSED_POINTCLOUDS
    "Build Draco and Cloudini compressed point-cloud support into the Scene3D wasm port" ON)
option(PJ_WASM_SCENE3D_CAPABILITY_PROBE "Build the standalone Scene3D QRhi capability probe" OFF)
option(PJ_WASM_WITH_MARKETPLACE "Build marketplace support in the wasm app" OFF)
# About 1.9 MB of Brotli download (docs/WASM_DEPLOYMENT.md); OFF drops on-demand Python from the browser.
option(PJ_WASM_WITH_PYTHON "Build on-demand Python scripts (a static CPython 3.14) into the wasm app" ON)

if(PJ_WASM_WITH_SCENE2D)
    include(${CMAKE_CURRENT_LIST_DIR}/PjWasmScene2DDependencies.cmake)
endif()

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
    GIT_SHALLOW TRUE
    OVERRIDE_FIND_PACKAGE
    SYSTEM)

set(FMT_TEST OFF CACHE BOOL "" FORCE)
set(FMT_DOC OFF CACHE BOOL "" FORCE)
FetchContent_Declare(fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 12.1.0
    GIT_SHALLOW TRUE
    OVERRIDE_FIND_PACKAGE
    SYSTEM)

set(FASTFLOAT_TEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(FastFloat
    GIT_REPOSITORY https://github.com/fastfloat/fast_float.git
    GIT_TAG v8.1.0
    GIT_SHALLOW TRUE
    OVERRIDE_FIND_PACKAGE
    SYSTEM)

FetchContent_Declare(tsl-robin-map
    GIT_REPOSITORY https://github.com/Tessil/robin-map.git
    GIT_TAG v1.4.0
    GIT_SHALLOW TRUE
    OVERRIDE_FIND_PACKAGE
    SYSTEM)

set(NANOARROW_IPC ON CACHE BOOL "" FORCE)
set(NANOARROW_NAMESPACE "PJ" CACHE STRING "" FORCE)
FetchContent_Declare(nanoarrow
    GIT_REPOSITORY https://github.com/apache/arrow-nanoarrow.git
    GIT_TAG apache-arrow-nanoarrow-0.7.0
    GIT_SHALLOW TRUE
    OVERRIDE_FIND_PACKAGE
    SYSTEM)

set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
    GIT_SHALLOW TRUE
    OVERRIDE_FIND_PACKAGE
    SYSTEM)


FetchContent_MakeAvailable(
    nlohmann_json
    fmt
    FastFloat
    tsl-robin-map
    nanoarrow
    glm)


if(PJ_WASM_WITH_CSV)
    # CSV timestamp parsing needs only date/date.h; keep the timezone library,
    # database, and upstream tests out of the browser binary.
    set(BUILD_TZ_LIB OFF CACHE BOOL "" FORCE)
    set(USE_SYSTEM_TZ_DB OFF CACHE BOOL "" FORCE)
    set(ENABLE_DATE_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(date
        GIT_REPOSITORY https://github.com/HowardHinnant/date.git
        GIT_TAG v3.0.3
        GIT_SHALLOW TRUE
        OVERRIDE_FIND_PACKAGE
        SYSTEM)

    FetchContent_MakeAvailable(date)
endif()

if(PJ_WASM_WITH_CSV OR PJ_WASM_WITH_MCAP OR PJ_WASM_WITH_ROS OR PJ_WASM_WITH_PROTOBUF OR
   PJ_WASM_WITH_TRANSFORM_EDITOR OR PJ_WASM_WITH_DUMMY_STREAM OR PJ_WASM_WITH_PJ_BRIDGE OR PJ_WASM_WITH_JSON OR
   PJ_WASM_WITH_DATA_TAMER OR PJ_WASM_WITH_3D_LOADER OR PJ_WASM_WITH_QUATERNION OR
   PJ_WASM_WITH_FFT OR PJ_WASM_WITH_ANOMALY_DETECTOR)
    # Source-only population shared by the narrowly-scoped static plugin
    # targets. Pin the reviewed upstream integration commit by immutable archive
    # and hash so PJ4 consumes the official sources without a local patch layer.
    FetchContent_Declare(pj_official_plugins
        URL https://codeload.github.com/PlotJuggler/pj-official-plugins/tar.gz/37e1bde18b8a425b9b64d7fe54c165464942b5eb
        URL_HASH SHA256=d5fcbc1e9a5a837e36fa2c1c64fa02908d74579671f413409ca86622a3aa5e04
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR _pj_source_only_no_cmakelists)
    FetchContent_MakeAvailable(pj_official_plugins)
    set(PJ_OFFICIAL_PLUGINS_SOURCE_DIR "${pj_official_plugins_SOURCE_DIR}" CACHE INTERNAL "")
endif()

if(PJ_WASM_WITH_PROTOBUF)
    # Match the official plugin's native dependency pin. The runtime parser uses
    # libprotobuf reflection and, since Protobuf 22, its in-process Importer and
    # parser. Build only that library: no libprotoc/code generators, protoc
    # executable, tests, examples, conformance tools, upb, or zlib support.
    set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(protobuf_BUILD_CONFORMANCE OFF CACHE BOOL "" FORCE)
    set(protobuf_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(protobuf_BUILD_PROTOC_BINARIES OFF CACHE BOOL "" FORCE)
    set(protobuf_BUILD_LIBPROTOC OFF CACHE BOOL "" FORCE)
    set(protobuf_BUILD_LIBUPB OFF CACHE BOOL "" FORCE)
    set(protobuf_WITH_ZLIB OFF CACHE BOOL "" FORCE)
    set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
    set(protobuf_ABSL_PROVIDER "module" CACHE STRING "" FORCE)
    set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
    set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(protobuf
        GIT_REPOSITORY https://github.com/protocolbuffers/protobuf.git
        GIT_TAG v33.5
        GIT_SHALLOW TRUE
        GIT_SUBMODULES_RECURSE TRUE
        OVERRIDE_FIND_PACKAGE
        SYSTEM)
    FetchContent_MakeAvailable(protobuf)
endif()

if(PJ_WASM_WITH_ROS)
    # Match parser_ros' pinned upstream graph. rosx is the wire/schema engine;
    # neither ROS middleware nor ament is needed in the browser.
    set(CMAKE_DISABLE_FIND_PACKAGE_ament_cmake TRUE)
    set(ROSX_HAS_JSON OFF CACHE BOOL "" FORCE)
    set(ROSX_PYTHON_BINDINGS OFF CACHE BOOL "" FORCE)
    set(BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(rosx_introspection
        URL https://github.com/facontidavide/rosx_introspection/archive/refs/tags/3.1.2.zip
        URL_HASH SHA256=0ffecc3b21e8cbe01097292e3e5f7bab67777f9fc4f943ae68006c8939dbc23b
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        OVERRIDE_FIND_PACKAGE
        SYSTEM)
    FetchContent_MakeAvailable(rosx_introspection)
    set_target_properties(rosx_introspection PROPERTIES POSITION_INDEPENDENT_CODE ON)
    get_target_property(_pj_rosx_includes rosx_introspection INTERFACE_INCLUDE_DIRECTORIES)
    if(_pj_rosx_includes)
        set_target_properties(rosx_introspection PROPERTIES
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_pj_rosx_includes}")
    endif()
    unset(_pj_rosx_includes)
endif()

if(PJ_WASM_WITH_ROS OR PJ_WASM_WITH_DATA_TAMER)
    # Header-only DataTamer schema/snapshot parser: parser_ros embeds it for its
    # DataTamer specialization, parser_data_tamer is built on it.
    FetchContent_Declare(data_tamer
        URL https://github.com/PickNikRobotics/data_tamer/archive/refs/tags/1.0.3.zip
        URL_HASH SHA256=0636b7a597be03977fcca6b070ca6190708fb9afbe5b102db12c3145e1489697
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR _pj_source_only_no_cmakelists
        SYSTEM)
    FetchContent_MakeAvailable(data_tamer)
endif()

if(PJ_WASM_WITH_MCAP OR PJ_WASM_WITH_PJ_BRIDGE OR (PJ_WASM_WITH_SCENE3D AND PJ_WASM_WITH_COMPRESSED_POINTCLOUDS))
    # Match the native Conan graph exactly. MCAP uses these libraries for chunk
    # decompression, the PlotJuggler Bridge zstd for its data frames, and
    # Cloudini the same pinned static targets for its second compression stage.
    # Populate them once when any of these browser features needs them.
    set(LZ4_BUILD_CLI OFF CACHE BOOL "" FORCE)
    set(LZ4_BUILD_LEGACY_LZ4C OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(lz4
        GIT_REPOSITORY https://github.com/lz4/lz4.git
        GIT_TAG v1.9.4
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR build/cmake
        OVERRIDE_FIND_PACKAGE
        SYSTEM)

    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
    set(ZSTD_DISABLE_ASM ON CACHE BOOL "" FORCE)
    FetchContent_Declare(zstd
        GIT_REPOSITORY https://github.com/facebook/zstd.git
        GIT_TAG v1.5.5
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR build/cmake
        OVERRIDE_FIND_PACKAGE
        SYSTEM)

    # LZ4 1.9.4 predates CMake 4's removal of pre-3.5 policy compatibility.
    # Scope the documented compatibility floor to these dependency directories;
    # PJ4 itself keeps its normal, newer policy baseline.
    set(_pj_saved_policy_version_minimum "${CMAKE_POLICY_VERSION_MINIMUM}")
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    FetchContent_MakeAvailable(lz4 zstd)
    if(_pj_saved_policy_version_minimum)
        set(CMAKE_POLICY_VERSION_MINIMUM "${_pj_saved_policy_version_minimum}")
    else()
        unset(CMAKE_POLICY_VERSION_MINIMUM)
    endif()
    unset(_pj_saved_policy_version_minimum)
    if(NOT TARGET LZ4::lz4_static AND TARGET lz4_static)
        add_library(LZ4::lz4_static ALIAS lz4_static)
    endif()
    if(NOT TARGET zstd::libzstd_static AND TARGET libzstd_static)
        add_library(zstd::libzstd_static ALIAS libzstd_static)
    endif()
endif()

if(PJ_WASM_WITH_SCENE3D)
    include(${CMAKE_CURRENT_LIST_DIR}/PjWasmScene3DDependencies.cmake)
endif()

if(PJ_WASM_WITH_LUAU)
    set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
    set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(LUAU_BUILD_WEB OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(Luau
        GIT_REPOSITORY https://github.com/luau-lang/luau.git
        GIT_TAG 0.700
        GIT_SHALLOW TRUE
        OVERRIDE_FIND_PACKAGE
        SYSTEM)
    FetchContent_MakeAvailable(Luau)

    if(NOT TARGET Luau::VM AND TARGET Luau.VM)
        add_library(Luau::VM ALIAS Luau.VM)
    endif()
    if(NOT TARGET Luau::Compiler AND TARGET Luau.Compiler)
        add_library(Luau::Compiler ALIAS Luau.Compiler)
    endif()

    # Native CodeGen emits llvm.clear_cache, which is unavailable on wasm. PJ4
    # uses the portable VM/compiler only.
    foreach(_unused IN ITEMS
            Luau.CodeGen Luau.Analysis Luau.EqSat Luau.Config Luau.Require
            Luau.CLI.lib)
        if(TARGET ${_unused})
            set_target_properties(${_unused} PROPERTIES EXCLUDE_FROM_ALL ON)
        endif()
    endforeach()
endif()

if(PJ_WASM_WITH_PYTHON)
    include(${CMAKE_CURRENT_LIST_DIR}/PjWasmPython.cmake)
endif()

if(PJ_WASM_WITH_LUAU OR PJ_WASM_WITH_FFT)
    # kissfft — pj_scripting's marker engine uses it for the bandPower primitive,
    # and the FFT toolbox for its spectrum. Pure C
    # with no OS calls, so it cross-compiles unchanged; it simply had no provider
    # here, because the wasm build resolves nothing through Conan or pixi.
    # Options mirror the Conan package and recipes/kissfft: static, float datatype.
    set(KISSFFT_STATIC ON CACHE BOOL "" FORCE)
    set(KISSFFT_DATATYPE "float" CACHE STRING "" FORCE)
    set(KISSFFT_TEST OFF CACHE BOOL "" FORCE)
    set(KISSFFT_TOOLS OFF CACHE BOOL "" FORCE)
    set(KISSFFT_PKGCONFIG OFF CACHE BOOL "" FORCE)
    # The source tree lands in a directory named kissfft so its parent also
    # serves the installed-package include layout (<kissfft/kiss_fftr.h>).
    FetchContent_Declare(kissfft
        GIT_REPOSITORY https://github.com/mborgerding/kissfft.git
        GIT_TAG 131.1.0
        GIT_SHALLOW TRUE
        SOURCE_DIR "${FETCHCONTENT_BASE_DIR}/kissfft-root/kissfft"
        OVERRIDE_FIND_PACKAGE
        SYSTEM)
    FetchContent_MakeAvailable(kissfft)
    get_filename_component(PJ_WASM_KISSFFT_PREFIXED_INCLUDE_DIR "${kissfft_SOURCE_DIR}" DIRECTORY)
    # Upstream already aliases kissfft::kissfft and kissfft::kissfft-<datatype>,
    # so pj_scripting's own non-Conan bridge finds its target and no-ops.
endif()
