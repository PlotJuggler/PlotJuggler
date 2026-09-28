# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0

# Cross-compiled dependencies owned exclusively by the Scene3D browser port.
# PR 2 needs only the optional compressed-point-cloud graph; mesh import enters
# with the model/rendering slice in PR 3.
include_guard(GLOBAL)

# Keep browser mesh import format-compatible with the native Scene3D path,
# while building only the four importers PlotJuggler exposes. This is Assimp
# 5.4.3, matching the native Conan graph; exporters, tools, tests, and install
# rules remain outside the browser application graph.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ZLIB OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_DRACO OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_DRACO_STATIC OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_COLLADA_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_OBJ_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_STL_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_GLTF_IMPORTER ON CACHE BOOL "" FORCE)
FetchContent_Declare(assimp
    URL https://codeload.github.com/assimp/assimp/tar.gz/c35200e38ea8f058812b83de2ef32c6093b0ece2
    URL_HASH SHA256=300fd8614af364bc750706c2b33cb66dee2d99eeb5e10f38cc979ddc9b80051a
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    OVERRIDE_FIND_PACKAGE
    SYSTEM)
FetchContent_MakeAvailable(assimp)
# Assimp uses exceptions internally even for successful imports. Emscripten
# disables catching in optimized translation units unless explicitly enabled.
target_compile_options(assimp PRIVATE -fexceptions)

if(PJ_WASM_WITH_COMPRESSED_POINTCLOUDS)
    # Match the native graph (Draco 1.5.7 + Cloudini 1.4.1). The common wasm
    # dependency file has already provided the shared LZ4 and Zstd targets.
    set(DRACO_JS_GLUE OFF CACHE BOOL "" FORCE)
    set(DRACO_MESH_COMPRESSION OFF CACHE BOOL "" FORCE)
    set(DRACO_POINT_CLOUD_COMPRESSION ON CACHE BOOL "" FORCE)
    set(DRACO_PREDICTIVE_EDGEBREAKER OFF CACHE BOOL "" FORCE)
    set(DRACO_STANDARD_EDGEBREAKER OFF CACHE BOOL "" FORCE)
    set(DRACO_TESTS OFF CACHE BOOL "" FORCE)
    set(DRACO_WASM OFF CACHE BOOL "" FORCE)
    set(DRACO_UNITY_PLUGIN OFF CACHE BOOL "" FORCE)
    set(DRACO_ANIMATION_ENCODING OFF CACHE BOOL "" FORCE)
    set(DRACO_MAYA_PLUGIN OFF CACHE BOOL "" FORCE)
    set(DRACO_TRANSCODER_SUPPORTED OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(draco
        URL https://github.com/google/draco/archive/refs/tags/1.5.7.tar.gz
        URL_HASH SHA256=bf6b105b79223eab2b86795363dfe5e5356050006a96521477973aba8f036fe1
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        OVERRIDE_FIND_PACKAGE
        SYSTEM)
    FetchContent_Declare(cloudini
        URL https://github.com/facontidavide/cloudini/archive/refs/tags/1.4.1.tar.gz
        URL_HASH SHA256=0fb83aa71c665fc73fdaaf3eefb2f16282f554f3d326c626437467acb203f59b
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR _pj_source_only_no_cmakelists
        SYSTEM)

    # Draco checks the legacy EMSCRIPTEN environment variable even with its JS
    # glue disabled. Scope that compatibility value to dependency population.
    if(DEFINED ENV{EMSCRIPTEN})
        set(_pj_had_emscripten_env TRUE)
        set(_pj_saved_emscripten_env "$ENV{EMSCRIPTEN}")
    else()
        set(_pj_had_emscripten_env FALSE)
    endif()
    if(NOT EXISTS "$ENV{EMSCRIPTEN}")
        get_filename_component(_pj_emscripten_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
        set(ENV{EMSCRIPTEN} "${_pj_emscripten_dir}")
    endif()
    FetchContent_MakeAvailable(draco cloudini)
    if(_pj_had_emscripten_env)
        set(ENV{EMSCRIPTEN} "${_pj_saved_emscripten_env}")
    else()
        unset(ENV{EMSCRIPTEN})
    endif()
    unset(_pj_emscripten_dir)
    unset(_pj_had_emscripten_env)
    unset(_pj_saved_emscripten_env)

    if(TARGET draco_static)
        target_include_directories(draco_static SYSTEM INTERFACE
            $<BUILD_INTERFACE:${draco_SOURCE_DIR}/src>
            $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}>)
    endif()

    add_library(pj_wasm_cloudini STATIC
        ${cloudini_SOURCE_DIR}/cloudini_lib/src/chunk_writer.cpp
        ${cloudini_SOURCE_DIR}/cloudini_lib/src/cloudini.cpp
        ${cloudini_SOURCE_DIR}/cloudini_lib/src/codec_common.cpp
        ${cloudini_SOURCE_DIR}/cloudini_lib/src/field_encoder.cpp
        ${cloudini_SOURCE_DIR}/cloudini_lib/src/field_decoder.cpp
        ${cloudini_SOURCE_DIR}/cloudini_lib/src/ros_msg_utils.cpp
        ${cloudini_SOURCE_DIR}/cloudini_lib/src/v4_codec.cpp
        ${cloudini_SOURCE_DIR}/cloudini_lib/src/v5_codec.cpp
        ${cloudini_SOURCE_DIR}/cloudini_lib/src/v6_codec.cpp)
    target_compile_features(pj_wasm_cloudini PUBLIC cxx_std_20)
    target_include_directories(pj_wasm_cloudini SYSTEM PUBLIC
        ${cloudini_SOURCE_DIR}/cloudini_lib/include
        PRIVATE ${zstd_SOURCE_DIR}/lib)
    target_link_libraries(pj_wasm_cloudini PRIVATE
        LZ4::lz4_static
        zstd::libzstd_static)
    add_library(cloudini::cloudini ALIAS pj_wasm_cloudini)
endif()
