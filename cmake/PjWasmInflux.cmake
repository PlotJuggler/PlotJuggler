# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0

include_guard(GLOBAL)

if(NOT EMSCRIPTEN OR NOT PJ_WASM_WITH_INFLUX)
    return()
endif()
pj_wasm_require_official_plugin_sources(_pj_influx_dir parser_influx influx_parser.cpp manifest.json)

include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedManifest.cmake")

add_library(pj_wasm_influx_parser STATIC "${_pj_influx_dir}/influx_parser.cpp")
target_compile_features(pj_wasm_influx_parser PRIVATE cxx_std_20)
target_link_libraries(pj_wasm_influx_parser PRIVATE plotjuggler_sdk::plugin_sdk)

pj_embed_manifest(pj_wasm_influx_parser
    MANIFEST_FILE "${_pj_influx_dir}/manifest.json"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/influx_manifest.hpp"
    VAR_NAME kInfluxManifest)

pj_wasm_finalize_official_plugin_targets(pj_wasm_influx_parser)
