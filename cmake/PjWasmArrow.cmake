# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0

# parser_arrow, data_load_parquet and toolbox_data_exporter on the Arrow C++
# libraries that PjWasmArrowDependencies.cmake builds (target pj_wasm_arrow).
include_guard(GLOBAL)

if(NOT EMSCRIPTEN OR NOT PJ_WASM_WITH_ARROW)
    return()
endif()

include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedUi.cmake")
include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedManifest.cmake")

if(PJ_WASM_WITH_ARROW_PARSER)
    pj_wasm_require_official_plugin_sources(_pj_arrow_dir parser_arrow src/arrow_parser.cpp manifest.json)
    add_library(pj_wasm_arrow_parser STATIC
        "${_pj_arrow_dir}/src/mosaico_objects.cpp"
        "${_pj_arrow_dir}/src/ipc_decoder.cpp"
        "${_pj_arrow_dir}/src/table_shaper.cpp"
        "${_pj_arrow_dir}/src/arrow_parser.cpp")
    target_compile_features(pj_wasm_arrow_parser PRIVATE cxx_std_20)
    target_include_directories(pj_wasm_arrow_parser PRIVATE "${_pj_arrow_dir}/src")
    target_link_libraries(pj_wasm_arrow_parser PRIVATE
        plotjuggler_sdk::plugin_sdk nanoarrow::nanoarrow nlohmann_json::nlohmann_json)
    target_link_libraries(pj_wasm_arrow_parser PUBLIC pj_wasm_arrow)
    pj_embed_manifest(pj_wasm_arrow_parser
        MANIFEST_FILE "${_pj_arrow_dir}/manifest.json"
        HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/arrow_manifest.hpp"
        VAR_NAME kArrowManifest)
    pj_wasm_finalize_official_plugin_targets(pj_wasm_arrow_parser)
endif()

if(PJ_WASM_WITH_PARQUET)
    pj_wasm_require_official_plugin_sources(_pj_parquet_dir data_load_parquet
        parquet_source.cpp dataload_parquet.ui manifest.json)
    add_library(pj_wasm_parquet_source STATIC
        "${_pj_parquet_dir}/parquet_source.cpp"
        "${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/common/arrow_helpers/arrow_helpers.cpp"
        # date::locate_zone needs the tz library, which PJ4's wasm build leaves out.
        # Built here against the (absent) OS tz database, no remote download.
        "${date_SOURCE_DIR}/src/tz.cpp")
    target_compile_features(pj_wasm_parquet_source PRIVATE cxx_std_20)
    target_compile_definitions(pj_wasm_parquet_source PRIVATE USE_OS_TZDB=1 HAS_REMOTE_API=0)
    target_include_directories(pj_wasm_parquet_source PRIVATE
        "${_pj_parquet_dir}" "${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/common/arrow_helpers/include")
    target_link_libraries(pj_wasm_parquet_source PRIVATE
        plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json date::date)
    target_link_libraries(pj_wasm_parquet_source PUBLIC pj_wasm_arrow)
    pj_embed_ui(pj_wasm_parquet_source
        UI_FILE "${_pj_parquet_dir}/dataload_parquet.ui"
        HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/dataload_parquet_ui.hpp"
        VAR_NAME kDataLoadParquetUi)
    pj_embed_manifest(pj_wasm_parquet_source
        MANIFEST_FILE "${_pj_parquet_dir}/manifest.json"
        HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/parquet_manifest.hpp"
        VAR_NAME kParquetManifest)
    pj_wasm_finalize_official_plugin_targets(pj_wasm_parquet_source)
endif()

if(PJ_WASM_WITH_DATA_EXPORTER)
    pj_wasm_require_official_plugin_sources(_pj_exporter_dir toolbox_data_exporter
        plugin_entry.cpp exporter_dialog.ui manifest.json)
    add_library(pj_wasm_data_exporter STATIC
        "${_pj_exporter_dir}/arrow_series_reader.cpp"
        "${_pj_exporter_dir}/data_exporter.cpp"
        "${_pj_exporter_dir}/export_core.cpp"
        "${_pj_exporter_dir}/plugin_entry.cpp")
    target_compile_features(pj_wasm_data_exporter PRIVATE cxx_std_20)
    target_include_directories(pj_wasm_data_exporter PRIVATE "${_pj_exporter_dir}")
    target_link_libraries(pj_wasm_data_exporter PRIVATE
        plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json)
    target_link_libraries(pj_wasm_data_exporter PUBLIC pj_wasm_arrow)
    pj_embed_ui(pj_wasm_data_exporter
        UI_FILE "${_pj_exporter_dir}/exporter_dialog.ui"
        HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/exporter_dialog_ui.hpp"
        VAR_NAME kDataExporterDialogUi)
    pj_embed_manifest(pj_wasm_data_exporter
        MANIFEST_FILE "${_pj_exporter_dir}/manifest.json"
        HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/data_exporter_manifest.hpp"
        VAR_NAME kDataExporterManifest)
    pj_wasm_finalize_official_plugin_targets(pj_wasm_data_exporter)
endif()
