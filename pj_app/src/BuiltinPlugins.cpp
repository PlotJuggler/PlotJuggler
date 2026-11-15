// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "BuiltinPlugins.h"

#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
extern const PJ_data_source_vtable_t* pj_static_get_data_source_vtable_WasmIngressProbeSource() noexcept;
extern const PJ_dialog_vtable_t* pj_static_get_dialog_vtable_WasmIngressProbeDialog() noexcept;
#endif

#ifdef PJ_WASM_ENABLE_MCAP_PROBE_PARSER
extern const PJ_message_parser_vtable_t* pj_static_get_message_parser_vtable_WasmMcapProbeParser() noexcept;
#endif

#ifdef __EMSCRIPTEN__
// The official plugins' getters and pj_wasm_official_plugins(), generated from
// cmake/PjWasmOfficialPlugins.cmake.
#include "pj_wasm_official_plugins.inc"
#endif

namespace pj_app {

PJ::StaticPluginSet builtInPlugins() {
  PJ::StaticPluginSet plugins;
#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
  plugins.data_sources.emplace_back(
      pj_static_get_data_source_vtable_WasmIngressProbeSource(), pj_static_get_dialog_vtable_WasmIngressProbeDialog());
#endif
#ifdef PJ_WASM_ENABLE_MCAP_PROBE_PARSER
  plugins.message_parsers.emplace_back(pj_static_get_message_parser_vtable_WasmMcapProbeParser());
#endif
#ifdef __EMSCRIPTEN__
  pj_wasm_official_plugins(plugins);
#endif
  return plugins;
}

}  // namespace pj_app
