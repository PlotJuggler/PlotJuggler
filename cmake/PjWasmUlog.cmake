# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0

include_guard(GLOBAL)

if(NOT EMSCRIPTEN OR NOT PJ_WASM_WITH_ULOG)
    return()
endif()
pj_wasm_require_official_plugin_sources(_pj_ulog_dir data_load_ulog
    ulog_source.cpp ulog_params_dialog.cpp ulog_container.cpp ulog_params.ui manifest.json)

include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedUi.cmake")
include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedManifest.cmake")

add_library(pj_wasm_ulog_source STATIC
    "${_pj_ulog_dir}/ulog_source.cpp"
    "${_pj_ulog_dir}/ulog_params_dialog.cpp"
    "${_pj_ulog_dir}/ulog_container.cpp")
target_compile_features(pj_wasm_ulog_source PRIVATE cxx_std_20)
target_include_directories(pj_wasm_ulog_source PRIVATE "${_pj_ulog_dir}")
target_link_libraries(pj_wasm_ulog_source PRIVATE
    plotjuggler_sdk::plugin_sdk
    ulog_cpp::ulog_cpp
    nlohmann_json::nlohmann_json)

pj_embed_manifest(pj_wasm_ulog_source
    MANIFEST_FILE "${_pj_ulog_dir}/manifest.json"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/ulog_manifest.hpp"
    VAR_NAME kUlogManifest)
pj_embed_ui(pj_wasm_ulog_source
    UI_FILE "${_pj_ulog_dir}/ulog_params.ui"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/ulog_params_ui.hpp"
    VAR_NAME kULogParamsUi)

pj_wasm_finalize_official_plugin_targets(pj_wasm_ulog_source)
