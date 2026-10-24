# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0

include_guard(GLOBAL)

if(NOT EMSCRIPTEN OR NOT PJ_WASM_WITH_FFT)
    return()
endif()
if(NOT TARGET kissfft::kissfft)
    message(FATAL_ERROR "PJ_WASM_WITH_FFT requires the pinned kissfft target")
endif()
pj_wasm_require_official_plugin_sources(_pj_fft_dir toolbox_fft
    fft_plugin.cpp fft_algorithm.cpp fft_algorithm.hpp fft_dialog.ui manifest.json)

include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedUi.cmake")
include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedManifest.cmake")

add_library(pj_wasm_fft STATIC
    "${_pj_fft_dir}/fft_plugin.cpp"
    "${_pj_fft_dir}/fft_algorithm.cpp")
target_compile_features(pj_wasm_fft PRIVATE cxx_std_20)
target_include_directories(pj_wasm_fft PRIVATE "${_pj_fft_dir}")
target_include_directories(pj_wasm_fft SYSTEM PRIVATE "${PJ_WASM_KISSFFT_PREFIXED_INCLUDE_DIR}")
target_link_libraries(pj_wasm_fft PRIVATE
    plotjuggler_sdk::plugin_sdk
    nlohmann_json::nlohmann_json
    kissfft::kissfft)

pj_embed_ui(pj_wasm_fft
    UI_FILE "${_pj_fft_dir}/fft_dialog.ui"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/fft_dialog_ui.hpp"
    VAR_NAME kFftDialogUi)
pj_embed_manifest(pj_wasm_fft
    MANIFEST_FILE "${_pj_fft_dir}/manifest.json"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/fft_manifest.hpp"
    VAR_NAME kFftManifest)

pj_wasm_finalize_official_plugin_targets(pj_wasm_fft)
