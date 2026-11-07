# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0

# The official PlotJuggler Bridge streaming source (data_stream_pj_bridge) in the
# browser. It connects through the browser's own WebSocket
# (pj_bridge_websocket_wasm.cpp on <emscripten/websocket.h>); the native
# IXWebSocket backend is never compiled here, so no socket library enters the
# module. Data frames are zstd-compressed and reuse the pinned Zstd target.
#
# Symbol audit (root CLAUDE.md), emsdk llvm-nm -A --defined-only --extern-only
# on libpj_wasm_pj_bridge.a against every archive and object of pj_app's link
# plus the multithread sysroot archives: its strong definitions (the two static
# getters, PJ::BridgeProtocol::*, the pj_bridge_websocket EM_JS entries) exist
# nowhere else, and the few hundred symbols it shares are weak instantiations of
# the libc++, nlohmann_json and SDK headers every plugin compiles alike; a shared weak symbol from a plugin's own
# inline variable or function would be a silent ODR merge, so those must stay
# uniquely named. Zstd stays one copy (libzstd.a), and no IXWebSocket symbol is
# referenced. Repeat it when the pin or this recipe changes.
include_guard(GLOBAL)

if(NOT EMSCRIPTEN OR NOT PJ_WASM_WITH_PJ_BRIDGE)
    return()
endif()
if(NOT TARGET zstd::libzstd_static OR NOT EXISTS "${zstd_SOURCE_DIR}/lib/zstd.h")
    message(FATAL_ERROR "PJ_WASM_WITH_PJ_BRIDGE requires the pinned Zstd target and source")
endif()

pj_wasm_require_official_plugin_sources(_pj_bridge_dir data_stream_pj_bridge
    pj_bridge_source.cpp pj_bridge_protocol.cpp pj_bridge_websocket_wasm.cpp
    pj_bridge_websocket.hpp pj_bridge_dialog.hpp pj_bridge_topic_subscription.hpp pj_bridge_protocol.hpp
    websocket_client.ui manifest.json)

include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedUi.cmake")
include("${PJ_OFFICIAL_PLUGINS_SOURCE_DIR}/cmake/EmbedManifest.cmake")

add_library(pj_wasm_pj_bridge STATIC
    "${_pj_bridge_dir}/pj_bridge_source.cpp"
    "${_pj_bridge_dir}/pj_bridge_protocol.cpp"
    "${_pj_bridge_dir}/pj_bridge_websocket_wasm.cpp")
target_compile_features(pj_wasm_pj_bridge PRIVATE cxx_std_20)
target_include_directories(pj_wasm_pj_bridge PRIVATE "${_pj_bridge_dir}")
# The same Zstd include workaround as PjWasmMcap.cmake.
target_include_directories(pj_wasm_pj_bridge SYSTEM PRIVATE "${zstd_SOURCE_DIR}/lib")
target_link_libraries(pj_wasm_pj_bridge PRIVATE
    plotjuggler_sdk::plugin_sdk
    nlohmann_json::nlohmann_json
    zstd::libzstd_static)
# Emscripten links its WebSocket JS library only when asked; the final
# executable inherits this, or emscripten_websocket_* stays undefined.
target_link_options(pj_wasm_pj_bridge INTERFACE "-lwebsocket.js")

pj_embed_manifest(pj_wasm_pj_bridge
    MANIFEST_FILE "${_pj_bridge_dir}/manifest.json"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/pj_bridge_manifest.hpp"
    VAR_NAME kPjBridgeManifest)
pj_embed_ui(pj_wasm_pj_bridge
    UI_FILE "${_pj_bridge_dir}/websocket_client.ui"
    HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/websocket_client_ui.hpp"
    VAR_NAME kWebSocketClientUi)

pj_wasm_finalize_official_plugin_targets(pj_wasm_pj_bridge)
