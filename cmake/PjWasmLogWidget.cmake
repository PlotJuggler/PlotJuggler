# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0

include_guard(GLOBAL)

if(NOT EMSCRIPTEN OR NOT PJ_WASM_WITH_LOG_WIDGET)
    return()
endif()
pj_wasm_require_official_plugin_sources(_pj_log_widget_dir widget_log
    log_filter.cpp log_loader.cpp log_store.cpp
    log_viewer_plugin.cpp log_viewer_widget.cpp log_table_model.cpp log_viewer.ui manifest.json)

include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedManifest.cmake")

# The first Widget-family plugin of the browser build: it links the app's own Qt, so the host's
# Qt-version and compiler-family admission checks hold by construction.
add_library(pj_wasm_log_widget STATIC
    "${_pj_log_widget_dir}/log_filter.cpp"
    "${_pj_log_widget_dir}/log_loader.cpp"
    "${_pj_log_widget_dir}/log_store.cpp"
    "${_pj_log_widget_dir}/log_viewer_plugin.cpp"
    "${_pj_log_widget_dir}/log_viewer_widget.cpp"
    "${_pj_log_widget_dir}/log_table_model.cpp"
    "${_pj_log_widget_dir}/log_viewer.ui")
set_target_properties(pj_wasm_log_widget PROPERTIES AUTOMOC ON AUTOUIC ON)
target_compile_features(pj_wasm_log_widget PRIVATE cxx_std_20)
target_include_directories(pj_wasm_log_widget PRIVATE
    "${_pj_log_widget_dir}"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(pj_wasm_log_widget PRIVATE
    plotjuggler_sdk::plugin_sdk
    Qt6::Widgets
    nlohmann_json::nlohmann_json)

pj_embed_manifest(pj_wasm_log_widget
    MANIFEST_FILE "${_pj_log_widget_dir}/manifest.json"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/log_viewer_manifest.hpp"
    VAR_NAME kLogViewerManifest)

pj_wasm_finalize_official_plugin_targets(pj_wasm_log_widget)
