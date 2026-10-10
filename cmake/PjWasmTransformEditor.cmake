# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0

include_guard(GLOBAL)

if(NOT EMSCRIPTEN OR NOT PJ_WASM_WITH_TRANSFORM_EDITOR)
    return()
endif()
pj_wasm_require_official_plugin_sources(_pj_transform_editor_dir toolbox_transform_editor
    transform_editor_plugin.cpp transform_editor_dialog.ui transform_editor_help.ui
    function_library.ui save_function_name.ui overwrite_function.ui manifest.json)

include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedUi.cmake")
include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedManifest.cmake")

add_library(pj_wasm_transform_editor STATIC
    "${_pj_transform_editor_dir}/transform_editor_plugin.cpp")
target_compile_features(pj_wasm_transform_editor PRIVATE cxx_std_20)
target_include_directories(pj_wasm_transform_editor PRIVATE
    "${_pj_transform_editor_dir}"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(pj_wasm_transform_editor PRIVATE
    plotjuggler_sdk::plugin_sdk
    nlohmann_json::nlohmann_json)

pj_embed_ui(pj_wasm_transform_editor
    UI_FILE "${_pj_transform_editor_dir}/transform_editor_dialog.ui"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/transform_editor_dialog_ui.hpp"
    VAR_NAME kTransformEditorDialogUi)
pj_embed_ui(pj_wasm_transform_editor
    UI_FILE "${_pj_transform_editor_dir}/transform_editor_help.ui"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/transform_editor_help_ui.hpp"
    VAR_NAME kHelpDialogUi)
pj_embed_ui(pj_wasm_transform_editor
    UI_FILE "${_pj_transform_editor_dir}/function_library.ui"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/function_library_ui.hpp"
    VAR_NAME kFunctionLibraryUi)
pj_embed_ui(pj_wasm_transform_editor
    UI_FILE "${_pj_transform_editor_dir}/save_function_name.ui"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/save_function_name_ui.hpp"
    VAR_NAME kSaveNameUi)
pj_embed_ui(pj_wasm_transform_editor
    UI_FILE "${_pj_transform_editor_dir}/overwrite_function.ui"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/overwrite_function_ui.hpp"
    VAR_NAME kOverwriteUi)
pj_embed_manifest(pj_wasm_transform_editor
    MANIFEST_FILE "${_pj_transform_editor_dir}/manifest.json"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/transform_editor_manifest.hpp"
    VAR_NAME kTransformEditorManifest)

pj_wasm_finalize_official_plugin_targets(pj_wasm_transform_editor)
