# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0

include_guard(GLOBAL)

if(NOT EMSCRIPTEN OR NOT PJ_WASM_WITH_ANOMALY_DETECTOR)
    return()
endif()
# The toolbox carries no script engine: it submits its rule through the host's
# pj.data_processors.v1 service, so only the Luau-free helper layer is built.
pj_wasm_require_official_plugin_sources(_pj_anomaly_dir toolbox_anomaly_detector
    anomaly_detector.cpp core/anomaly_helpers.cpp core/anomaly_helpers.hpp
    anomaly_detector_dialog.ui manifest.json)

include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedUi.cmake")
include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedManifest.cmake")

add_library(pj_wasm_anomaly_detector STATIC
    "${_pj_anomaly_dir}/anomaly_detector.cpp"
    "${_pj_anomaly_dir}/core/anomaly_helpers.cpp")
target_compile_features(pj_wasm_anomaly_detector PRIVATE cxx_std_20)
target_include_directories(pj_wasm_anomaly_detector PRIVATE
    "${_pj_anomaly_dir}"
    "${_pj_anomaly_dir}/core")
target_link_libraries(pj_wasm_anomaly_detector PRIVATE
    plotjuggler_sdk::plugin_sdk
    nlohmann_json::nlohmann_json
    pj_wasm_toolbox_preview)

pj_embed_manifest(pj_wasm_anomaly_detector
    MANIFEST_FILE "${_pj_anomaly_dir}/manifest.json"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/anomaly_detector_manifest.hpp"
    VAR_NAME kAnomalyDetectorManifest)
pj_embed_ui(pj_wasm_anomaly_detector
    UI_FILE "${_pj_anomaly_dir}/anomaly_detector_dialog.ui"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/anomaly_detector_dialog_ui.hpp"
    VAR_NAME kAnomalyDetectorDialogUi)

pj_wasm_finalize_official_plugin_targets(pj_wasm_anomaly_detector)
