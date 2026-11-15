# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0

# The official Foxglove Bridge streaming source (data_stream_foxglove_bridge) in
# the browser, over the browser's own WebSocket (the shared pj_wasm_websocket
# target). Its messages are decoded by the ROS, Protobuf and JSON parsers already
# in the module. A page served over https:// can open only wss://, so a robot's
# plain ws:// foxglove_bridge needs TLS in front of it (docs/WASM_HOSTING.md).
#
# Symbol audit (root CLAUDE.md), emsdk llvm-nm --defined-only --extern-only on
# libpj_wasm_foxglove_bridge.a against every other archive of pj_app's link, the
# Qt wasm_multithread archives and the sysroot archives: its two strong
# definitions (the static getters) exist nowhere else; the PJ::WebSocket inlines
# it shares with libpj_wasm_pj_bridge.a (DialogConnection, Endpoint,
# DropWarning) come from the same common/websocket header, so they are one
# definition. Repeat it when the pin or this recipe changes.
include_guard(GLOBAL)

if(NOT EMSCRIPTEN OR NOT PJ_WASM_WITH_FOXGLOVE_BRIDGE)
    return()
endif()

pj_wasm_require_official_plugin_sources(_pj_foxglove_dir data_stream_foxglove_bridge
    foxglove_source.cpp foxglove_dialog.hpp foxglove_protocol.hpp foxglove_topic_subscription.hpp
    foxglove_client.ui manifest.json)

include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedUi.cmake")
include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedManifest.cmake")

add_library(pj_wasm_foxglove_bridge STATIC "${_pj_foxglove_dir}/foxglove_source.cpp")
target_compile_features(pj_wasm_foxglove_bridge PRIVATE cxx_std_20)
target_include_directories(pj_wasm_foxglove_bridge PRIVATE "${_pj_foxglove_dir}")
target_link_libraries(pj_wasm_foxglove_bridge PRIVATE
    plotjuggler_sdk::plugin_sdk
    nlohmann_json::nlohmann_json
    pj_wasm_base64
    pj_wasm_websocket)

pj_embed_manifest(pj_wasm_foxglove_bridge
    MANIFEST_FILE "${_pj_foxglove_dir}/manifest.json"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/foxglove_manifest.hpp"
    VAR_NAME kFoxgloveManifest)
pj_embed_ui(pj_wasm_foxglove_bridge
    UI_FILE "${_pj_foxglove_dir}/foxglove_client.ui"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/foxglove_client_ui.hpp"
    VAR_NAME kFoxgloveClientUi)

pj_wasm_finalize_official_plugin_targets(pj_wasm_foxglove_bridge)
