# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0

include_guard(GLOBAL)

if(NOT EMSCRIPTEN OR NOT PJ_WASM_WITH_CSV)
    return()
endif()
pj_wasm_require_official_plugin_sources(_pj_csv_dir data_load_csv
        csv_parser.cpp timestamp_parsing.cpp csv_source.cpp csv_dialog.cpp
        manifest.json dataload_csv.ui datetimehelp.ui)

include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedUi.cmake")
include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedManifest.cmake")

add_library(pj_wasm_csv_parser STATIC
    "${_pj_csv_dir}/csv_parser.cpp"
    "${_pj_csv_dir}/timestamp_parsing.cpp")
target_compile_features(pj_wasm_csv_parser PUBLIC cxx_std_20)
target_include_directories(pj_wasm_csv_parser PUBLIC "${_pj_csv_dir}")
target_link_libraries(pj_wasm_csv_parser PUBLIC date::date plotjuggler_sdk::base)

add_library(pj_wasm_csv_source STATIC
    "${_pj_csv_dir}/csv_source.cpp"
    "${_pj_csv_dir}/csv_dialog.cpp")
target_compile_features(pj_wasm_csv_source PRIVATE cxx_std_20)
target_include_directories(pj_wasm_csv_source PRIVATE "${_pj_csv_dir}")
target_link_libraries(pj_wasm_csv_source PRIVATE
    plotjuggler_sdk::plugin_sdk
    pj_wasm_csv_parser
    nlohmann_json::nlohmann_json)

pj_embed_manifest(pj_wasm_csv_source
    MANIFEST_FILE "${_pj_csv_dir}/manifest.json"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/csv_manifest.hpp"
    VAR_NAME kCsvManifest)
pj_embed_ui(pj_wasm_csv_source
    UI_FILE "${_pj_csv_dir}/dataload_csv.ui"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/dataload_csv_ui.hpp"
    VAR_NAME kDataLoadCsvUi)
pj_embed_ui(pj_wasm_csv_source
    UI_FILE "${_pj_csv_dir}/datetimehelp.ui"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/datetimehelp_ui.hpp"
    VAR_NAME kDateTimeHelpUi)

pj_wasm_finalize_official_plugin_targets(pj_wasm_csv_parser pj_wasm_csv_source)
