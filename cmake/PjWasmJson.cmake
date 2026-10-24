# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0

include_guard(GLOBAL)

if(NOT EMSCRIPTEN OR NOT PJ_WASM_WITH_JSON)
    return()
endif()
pj_wasm_require_official_plugin_sources(_pj_json_dir parser_json
    json_parser.cpp json_parser_dialog.hpp json_parser_options.ui manifest.json)

include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedUi.cmake")
include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedManifest.cmake")

add_library(pj_wasm_json_parser STATIC
    "${_pj_json_dir}/json_parser.cpp")
target_compile_features(pj_wasm_json_parser PRIVATE cxx_std_20)
target_include_directories(pj_wasm_json_parser PRIVATE "${_pj_json_dir}")
target_link_libraries(pj_wasm_json_parser PRIVATE
    plotjuggler_sdk::plugin_sdk
    nlohmann_json::nlohmann_json)

pj_embed_ui(pj_wasm_json_parser
    UI_FILE "${_pj_json_dir}/json_parser_options.ui"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/json_parser_options_ui.hpp"
    VAR_NAME kJsonParserOptionsUi)
pj_embed_manifest(pj_wasm_json_parser
    MANIFEST_FILE "${_pj_json_dir}/manifest.json"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/json_manifest.hpp"
    VAR_NAME kJsonManifest)

pj_wasm_finalize_official_plugin_targets(pj_wasm_json_parser)
