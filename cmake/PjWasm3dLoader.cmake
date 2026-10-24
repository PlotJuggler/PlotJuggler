# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0

include_guard(GLOBAL)

if(NOT EMSCRIPTEN OR NOT PJ_WASM_WITH_3D_LOADER)
    return()
endif()
pj_wasm_require_official_plugin_sources(_pj_3d_dir data_load_3d
    loader_3d.cpp cloud_common.cpp pcd_reader.cpp ply_reader.cpp
    contrib/liblzf/lzf_d.c manifest.json)

include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedManifest.cmake")

# Only the LZF decompressor: the compressor serves the upstream test fixtures.
enable_language(C)
add_library(pj_wasm_lzf_d STATIC
    "${_pj_3d_dir}/contrib/liblzf/lzf_d.c")
target_include_directories(pj_wasm_lzf_d PUBLIC "${_pj_3d_dir}/contrib/liblzf")
set_target_properties(pj_wasm_lzf_d PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(pj_wasm_3d_loader STATIC
    "${_pj_3d_dir}/loader_3d.cpp"
    "${_pj_3d_dir}/cloud_common.cpp"
    "${_pj_3d_dir}/pcd_reader.cpp"
    "${_pj_3d_dir}/ply_reader.cpp")
target_compile_features(pj_wasm_3d_loader PRIVATE cxx_std_20)
target_include_directories(pj_wasm_3d_loader PRIVATE "${_pj_3d_dir}")
target_link_libraries(pj_wasm_3d_loader PRIVATE
    plotjuggler_sdk::plugin_sdk
    nlohmann_json::nlohmann_json
    pj_wasm_lzf_d)

pj_embed_manifest(pj_wasm_3d_loader
    MANIFEST_FILE "${_pj_3d_dir}/manifest.json"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/data_load_3d_manifest.hpp"
    VAR_NAME kDataLoad3dManifest)

pj_wasm_finalize_official_plugin_targets(pj_wasm_3d_loader)
