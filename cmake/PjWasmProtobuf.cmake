# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0

include_guard(GLOBAL)

if(NOT EMSCRIPTEN OR NOT PJ_WASM_WITH_PROTOBUF)
    return()
endif()
if(NOT TARGET protobuf::libprotobuf)
    message(FATAL_ERROR "PJ_WASM_WITH_PROTOBUF requires the pinned libprotobuf target")
endif()

pj_wasm_require_official_plugin_sources(_pj_protobuf_dir parser_protobuf
        protobuf_parser.cpp protobuf_parser_dialog.hpp
        foxglove_pointcloud_codec.cpp foxglove_pointcloud_codec.hpp
        foxglove_object_codecs.cpp foxglove_object_codecs.hpp
        foxglove_voxelgrid_codec.cpp foxglove_voxelgrid_codec.hpp
        foxglove_grid_codec.cpp foxglove_grid_codec.hpp
        manifest.json protobuf_parser_options.ui)
set(_pj_common_dir "${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/common")

include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedUi.cmake")
include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedManifest.cmake")

add_library(pj_wasm_base64 INTERFACE)
target_include_directories(pj_wasm_base64 INTERFACE
    "${_pj_common_dir}/base64/include")

add_library(pj_wasm_protobuf_parser STATIC
    "${_pj_protobuf_dir}/protobuf_parser.cpp"
    "${_pj_protobuf_dir}/foxglove_pointcloud_codec.cpp"
    "${_pj_protobuf_dir}/foxglove_object_codecs.cpp"
    "${_pj_protobuf_dir}/foxglove_voxelgrid_codec.cpp"
    "${_pj_protobuf_dir}/foxglove_grid_codec.cpp")
target_compile_features(pj_wasm_protobuf_parser PRIVATE cxx_std_20)
target_include_directories(pj_wasm_protobuf_parser PRIVATE "${_pj_protobuf_dir}")
target_link_libraries(pj_wasm_protobuf_parser PRIVATE
    plotjuggler_sdk::plugin_sdk
    protobuf::libprotobuf
    nlohmann_json::nlohmann_json
    pj_wasm_base64
    pj_wasm_laser_scan
    pj_wasm_grid_map
    pj_wasm_pointcloud_color)

pj_embed_manifest(pj_wasm_protobuf_parser
    MANIFEST_FILE "${_pj_protobuf_dir}/manifest.json"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/protobuf_manifest.hpp"
    VAR_NAME kProtobufManifest)
pj_embed_ui(pj_wasm_protobuf_parser
    UI_FILE "${_pj_protobuf_dir}/protobuf_parser_options.ui"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/protobuf_parser_options_ui.hpp"
    VAR_NAME kProtobufParserOptionsUi)

pj_wasm_finalize_official_plugin_targets(pj_wasm_protobuf_parser)
