# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0

include_guard(GLOBAL)

if(NOT EMSCRIPTEN OR NOT PJ_WASM_WITH_ROS)
    return()
endif()
if(NOT TARGET rosx_introspection)
    message(FATAL_ERROR "PJ_WASM_WITH_ROS requires the pinned rosx_introspection target")
endif()

pj_wasm_require_official_plugin_sources(_pj_ros_dir parser_ros
        ros_parser.cpp ros_specializations.cpp ros_builtin_object_handlers.cpp
        ros_marker_handlers.cpp ros_yolo_handlers.cpp ros_parser_plugin.cpp ros_parser_internal.hpp
        ros_parser_dialog.hpp manifest.json ros_parser_options.ui)
set(_pj_common_dir "${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/common")

include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedUi.cmake")
include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedManifest.cmake")

add_library(pj_wasm_ros_parser STATIC
    "${_pj_ros_dir}/ros_parser.cpp"
    "${_pj_ros_dir}/ros_specializations.cpp"
    "${_pj_ros_dir}/ros_builtin_object_handlers.cpp"
    "${_pj_ros_dir}/ros_marker_handlers.cpp"
    "${_pj_ros_dir}/ros_yolo_handlers.cpp"
    "${_pj_ros_dir}/ros_parser_plugin.cpp")
target_compile_features(pj_wasm_ros_parser PRIVATE cxx_std_20)
target_include_directories(pj_wasm_ros_parser PRIVATE "${_pj_ros_dir}")
target_link_libraries(pj_wasm_ros_parser PRIVATE
    plotjuggler_sdk::plugin_sdk
    nlohmann_json::nlohmann_json
    rosx_introspection
    pj_wasm_data_tamer_parser
    pj_wasm_laser_scan
    pj_wasm_grid_map
    pj_wasm_pointcloud_color)
if(PJ_WASM_ENABLE_INGRESS_PROBE)
    target_compile_definitions(pj_wasm_ros_parser PRIVATE PJ_WASM_ENABLE_INGRESS_PROBE)
endif()

pj_embed_manifest(pj_wasm_ros_parser
    MANIFEST_FILE "${_pj_ros_dir}/manifest.json"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/ros_manifest.hpp"
    VAR_NAME kRosManifest)
pj_embed_ui(pj_wasm_ros_parser
    UI_FILE "${_pj_ros_dir}/ros_parser_options.ui"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/ros_parser_options_ui.hpp"
    VAR_NAME kRosParserOptionsUi)

pj_wasm_finalize_official_plugin_targets(pj_wasm_ros_parser)
