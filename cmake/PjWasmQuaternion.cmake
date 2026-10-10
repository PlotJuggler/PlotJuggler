# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0

include_guard(GLOBAL)

if(NOT EMSCRIPTEN OR NOT PJ_WASM_WITH_QUATERNION)
    return()
endif()
pj_wasm_require_official_plugin_sources(_pj_quaternion_dir toolbox_quaternion
    quaternion_plugin.cpp quaternion_to_rpy.hpp quaternion_dialog.ui manifest.json)

include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedUi.cmake")
include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedManifest.cmake")

add_library(pj_wasm_quaternion STATIC
    "${_pj_quaternion_dir}/quaternion_plugin.cpp")
target_compile_features(pj_wasm_quaternion PRIVATE cxx_std_20)
target_include_directories(pj_wasm_quaternion PRIVATE "${_pj_quaternion_dir}")
target_link_libraries(pj_wasm_quaternion PRIVATE
    plotjuggler_sdk::plugin_sdk
    nlohmann_json::nlohmann_json)

pj_embed_manifest(pj_wasm_quaternion
    MANIFEST_FILE "${_pj_quaternion_dir}/manifest.json"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/quaternion_manifest.hpp"
    VAR_NAME kQuaternionManifest)
pj_embed_ui(pj_wasm_quaternion
    UI_FILE "${_pj_quaternion_dir}/quaternion_dialog.ui"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/quaternion_dialog_ui.hpp"
    VAR_NAME kQuaternionDialogUi)

pj_wasm_finalize_official_plugin_targets(pj_wasm_quaternion)
