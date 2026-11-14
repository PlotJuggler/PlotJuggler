# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0

include_guard(GLOBAL)

if(NOT EMSCRIPTEN OR NOT PJ_WASM_WITH_NAVMAP)
    return()
endif()
pj_wasm_require_official_plugin_sources(_pj_navmap_dir parser_navmap
    navmap_parser.cpp navmap_msg_decoder.cpp navmap_mesh.cpp contrib/nanocdr/nanocdr.hpp manifest.json)

include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedManifest.cmake")

add_library(pj_wasm_navmap_parser STATIC
    "${_pj_navmap_dir}/navmap_parser.cpp"
    "${_pj_navmap_dir}/navmap_msg_decoder.cpp"
    "${_pj_navmap_dir}/navmap_mesh.cpp")
target_compile_features(pj_wasm_navmap_parser PRIVATE cxx_std_20)
target_include_directories(pj_wasm_navmap_parser PRIVATE "${_pj_navmap_dir}")
# The vendored nanocdr is byte-identical to 3rdparty/nanocdr, so the inline
# definitions it shares with rosx_introspection's copy agree (no ODR conflict).
# Its constexpr endianness probe reads an inactive union member, which Clang
# rejects by default.
target_include_directories(pj_wasm_navmap_parser SYSTEM PRIVATE "${_pj_navmap_dir}/contrib")
target_compile_options(pj_wasm_navmap_parser PRIVATE -Wno-invalid-constexpr)
target_link_libraries(pj_wasm_navmap_parser PRIVATE
    plotjuggler_sdk::plugin_sdk
    nlohmann_json::nlohmann_json)

pj_embed_manifest(pj_wasm_navmap_parser
    MANIFEST_FILE "${_pj_navmap_dir}/manifest.json"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/navmap_manifest.hpp"
    VAR_NAME kNavMapManifest)

pj_wasm_finalize_official_plugin_targets(pj_wasm_navmap_parser)
