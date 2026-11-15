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
# The official plugins' PJ_WASM_WITH_<NAME> options.
include(${CMAKE_CURRENT_LIST_DIR}/PjWasmOfficialPlugins.cmake)
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


if(PJ_WASM_WITH_CSV OR PJ_WASM_WITH_PARQUET)
    # CSV timestamp parsing needs only date/date.h, and the Parquet loader
    # compiles date's tz.cpp itself; keep date's own timezone library, database,
    # and upstream tests out of the browser binary.
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

# The official plugins' sources and their wasm.cmake files (PjWasmOfficialPlugins.cmake): a
# source-only tarball, fetched whatever plugins are enabled.
# Pin the reviewed upstream commit by immutable archive and hash so PJ4 consumes the official
# sources without a local patch layer.
FetchContent_Declare(pj_official_plugins
    URL https://codeload.github.com/PlotJuggler/pj-official-plugins/tar.gz/598e966243ef6e75b29cadd5d145559d3eb504c2
    URL_HASH SHA256=039cae0272d4f5f889c938f87573af7d89add9ccb8888de3082ba8f1d5c896c9
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR _pj_source_only_no_cmakelists)
FetchContent_MakeAvailable(pj_official_plugins)
set(PJ_OFFICIAL_PLUGINS_SOURCE_DIR "${pj_official_plugins_SOURCE_DIR}" CACHE INTERNAL "")

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
        URL https://github.com/facontidavide/rosx_introspection/archive/refs/tags/3.1.3.zip
        URL_HASH SHA256=d12637fc579164c302067f0f6f6beca0c5739a89983c91317eb3f7be5d67a599
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

if(PJ_WASM_WITH_ULOG)
    # data_load_ulog's pinned PX4 ULog reader (its CPM pin).
    FetchContent_Declare(ulog_cpp
        URL https://github.com/PX4/ulog_cpp/archive/refs/tags/v1.0.1.zip
        URL_HASH SHA256=bb556d4c515f12707be816061c6ca7c0a8125917a0c4911ef1770ed9e1a85cb0
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SYSTEM)
    FetchContent_MakeAvailable(ulog_cpp)
    set_target_properties(ulog_cpp PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if(PJ_WASM_WITH_CANDUMP OR PJ_WASM_WITH_BLF OR PJ_WASM_WITH_MF4)
    # The CAN loaders' shared DBC decoder (common/can_dbc) builds dbc_parser_cpp
    # at the plugins' pin; the plugins' own function applies their fixes to it.
    FetchContent_Declare(dbc_parser_cpp
        URL https://github.com/LinuxDevon/dbc_parser_cpp/archive/2c8acdbd2d1ea33545f99f9a0d23e10b7f8e0852.tar.gz
        URL_HASH SHA256=91e12166d83c56f1ff945f6c94f14a7fe80e105219111f984645614ef8e09c57
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR _pj_source_only_no_cmakelists)
    # dbc_parser_cpp is compiled against the plugins' fast_float pin (header-only),
    # not the app's newer fast_float, so it parses exactly as the shipped plugins.
    FetchContent_Declare(dbc_fast_float
        URL https://github.com/fastfloat/fast_float/archive/1ea4f27b2aeee2859a1354a3c24cff52a116cad1.tar.gz
        URL_HASH SHA256=06f860e3adb994b9c8523bb53a76a335c4182e27fe5a077246146d283efdc9da
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR _pj_source_only_no_cmakelists)
    FetchContent_MakeAvailable(dbc_parser_cpp dbc_fast_float)
endif()

if(PJ_WASM_WITH_BLF OR PJ_WASM_WITH_MF4)
    # Emscripten's sysroot zlib, as for Scene2D's libpng.
    find_package(ZLIB 1.3.1 EXACT REQUIRED)
endif()

if(PJ_WASM_WITH_BLF)
    # data_load_blf's pinned BLF reader, hardened by the plugin's own function.
    FetchContent_Declare(lblf
        URL https://github.com/PetStr/lblf/archive/a453f188f67637a75e470c272cc76b475080f778.tar.gz
        URL_HASH SHA256=1ed25162dcab2316d7d8b896ac6d85f9d2f97ca2bfdf40b32165c0e4405046ff
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR _pj_source_only_no_cmakelists)
    FetchContent_MakeAvailable(lblf)
endif()

if(PJ_WASM_WITH_MF4)
    # data_load_mf4's pinned MDF reader and the XML parser it needs (the
    # plugins' Conan version). Only the expat library is built.
    FetchContent_Declare(mdflib
        URL https://github.com/ihedvall/mdflib/archive/5ad85af2c943a1ab810d0768d3873246a3920ebf.tar.gz
        URL_HASH SHA256=700e11196d570cb480cddc0258d7a1e74adc0562f2d2dfb3ee60f124f79c8cd4
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR _pj_source_only_no_cmakelists)
    set(EXPAT_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(EXPAT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(EXPAT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(EXPAT_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(EXPAT_BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
    set(EXPAT_BUILD_PKGCONFIG OFF CACHE BOOL "" FORCE)
    set(EXPAT_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(EXPAT_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(expat
        URL https://github.com/libexpat/libexpat/releases/download/R_2_10_0/expat-2.10.0.tar.gz
        URL_HASH SHA256=5466a341f9c692ef7a958a6881311be597f8e1ce345984cf78414f54793dea4c
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SYSTEM)
    FetchContent_MakeAvailable(mdflib expat)
    if(NOT TARGET EXPAT::EXPAT)
        add_library(EXPAT::EXPAT ALIAS expat)
    endif()
endif()

if(PJ_WASM_WITH_ROS OR PJ_WASM_WITH_DATA_TAMER)
    # Header-only DataTamer schema/snapshot parser: parser_ros embeds it for its
    # DataTamer specialization, parser_data_tamer is built on it.
    FetchContent_Declare(data_tamer
        URL https://github.com/facontidavide/data_tamer/archive/refs/tags/2.0.0.zip
        URL_HASH SHA256=0a8caef329d641b2cc72fa68211af37a36759614807add4bdba2ed2bf2fc8963
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR _pj_source_only_no_cmakelists
        SYSTEM)
    FetchContent_MakeAvailable(data_tamer)
    add_library(data_tamer_parser INTERFACE)
    target_include_directories(data_tamer_parser SYSTEM INTERFACE "${data_tamer_SOURCE_DIR}/data_tamer_cpp/include")
endif()

set(PJ_WASM_WITH_ARROW OFF)
if(PJ_WASM_WITH_ARROW_PARSER OR PJ_WASM_WITH_PARQUET OR PJ_WASM_WITH_DATA_EXPORTER)
    set(PJ_WASM_WITH_ARROW ON)
endif()

if(PJ_WASM_WITH_MCAP OR PJ_WASM_WITH_PJ_BRIDGE OR (PJ_WASM_WITH_SCENE3D AND PJ_WASM_WITH_COMPRESSED_POINTCLOUDS) OR
   PJ_WASM_WITH_ARROW)
    # Match the native Conan graph exactly. MCAP uses these libraries for chunk
    # decompression, the PlotJuggler Bridge zstd for its data frames, Arrow both
    # for its Parquet/IPC codecs, and Cloudini the same pinned static targets for
    # its second compression stage.
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
        # Zstd's build/cmake target does not publish its source-tree include directory (build
        # interface only: the target is in zstd's install export).
        target_include_directories(libzstd_static SYSTEM INTERFACE "$<BUILD_INTERFACE:${zstd_SOURCE_DIR}/lib>")
        add_library(zstd::libzstd_static ALIAS libzstd_static)
    endif()
endif()

if(PJ_WASM_WITH_FFT)
    include(${CMAKE_SOURCE_DIR}/pj_scripting/cmake/PjPocketfft.cmake)
endif()

if(PJ_WASM_WITH_SCENE3D)
    include(${CMAKE_CURRENT_LIST_DIR}/PjWasmScene3DDependencies.cmake)
endif()

if(PJ_WASM_WITH_ARROW)
    include(${CMAKE_CURRENT_LIST_DIR}/PjWasmArrowDependencies.cmake)
    # The Arrow and Parquet names the plugins link natively.
    add_library(Arrow::arrow_static ALIAS pj_wasm_arrow)
    add_library(Parquet::parquet_static ALIAS pj_wasm_arrow)
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
