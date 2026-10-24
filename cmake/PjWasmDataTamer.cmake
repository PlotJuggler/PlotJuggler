# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0

include_guard(GLOBAL)

if(NOT EMSCRIPTEN OR NOT PJ_WASM_WITH_DATA_TAMER)
    return()
endif()
pj_wasm_require_official_plugin_sources(_pj_data_tamer_dir parser_data_tamer
    data_tamer_parser.cpp manifest.json)

include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedManifest.cmake")

add_library(pj_wasm_data_tamer STATIC
    "${_pj_data_tamer_dir}/data_tamer_parser.cpp")
target_compile_features(pj_wasm_data_tamer PRIVATE cxx_std_20)
target_include_directories(pj_wasm_data_tamer PRIVATE "${_pj_data_tamer_dir}")
target_link_libraries(pj_wasm_data_tamer PRIVATE
    plotjuggler_sdk::plugin_sdk
    pj_wasm_data_tamer_parser)
# data_tamer_parser.hpp defines TypeField::operator== out of class without
# `inline`, so parser_ros and this plugin, each including it, would emit two
# strong definitions into one static binary. Renaming the header's namespace in
# this plugin's translation unit keeps the copies distinct.
target_compile_definitions(pj_wasm_data_tamer PRIVATE DataTamerParser=PjWasmDataTamerPluginParser)

pj_embed_manifest(pj_wasm_data_tamer
    MANIFEST_FILE "${_pj_data_tamer_dir}/manifest.json"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/data_tamer_manifest.hpp"
    VAR_NAME kDataTamerManifest)

pj_wasm_finalize_official_plugin_targets(pj_wasm_data_tamer)
