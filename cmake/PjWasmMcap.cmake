# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0

include_guard(GLOBAL)

if(NOT EMSCRIPTEN OR NOT PJ_WASM_WITH_MCAP)
    return()
endif()
if(NOT TARGET LZ4::lz4_static OR NOT TARGET zstd::libzstd_static)
    message(FATAL_ERROR "PJ_WASM_WITH_MCAP requires the pinned LZ4 and Zstd targets")
endif()
if(NOT EXISTS "${zstd_SOURCE_DIR}/lib/zstd.h")
    message(FATAL_ERROR "Pinned Zstd source is missing ${zstd_SOURCE_DIR}/lib/zstd.h")
endif()

pj_wasm_require_official_plugin_sources(_pj_mcap_dir data_load_mcap
        mcap_source.cpp mcap_dialog.hpp mcap_helpers.hpp mcap_dataset_metadata.hpp manifest.json dialog_mcap.ui
        contrib/mcap/reader.hpp contrib/mcap/parallel_reader.hpp
        contrib/mcap/message_byte_store.hpp contrib/mcap/unchunked_payload_store.hpp)

include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedUi.cmake")
include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedManifest.cmake")

add_library(pj_wasm_mcap_source STATIC "${_pj_mcap_dir}/mcap_source.cpp")
target_compile_features(pj_wasm_mcap_source PRIVATE cxx_std_20)
# Zstd 1.5.5's CMake target does not publish its source-tree include directory
# consistently when consumed through FetchContent. Keep that workaround local
# to the wasm-only MCAP target; the native build continues to use Conan.
target_include_directories(pj_wasm_mcap_source SYSTEM PRIVATE
    "${_pj_mcap_dir}/contrib"
    "${zstd_SOURCE_DIR}/lib")
target_link_libraries(pj_wasm_mcap_source PRIVATE
    plotjuggler_sdk::plugin_sdk
    nlohmann_json::nlohmann_json
    LZ4::lz4_static
    zstd::libzstd_static)

pj_embed_manifest(pj_wasm_mcap_source
    MANIFEST_FILE "${_pj_mcap_dir}/manifest.json"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/mcap_manifest.hpp"
    VAR_NAME kMcapManifest)
pj_embed_ui(pj_wasm_mcap_source
    UI_FILE "${_pj_mcap_dir}/dialog_mcap.ui"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/dialog_mcap_ui.hpp"
    VAR_NAME kDialogMcapUi)

pj_wasm_finalize_official_plugin_targets(pj_wasm_mcap_source)
