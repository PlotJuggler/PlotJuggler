// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Linked into plugins/arrow.wasm, not into the app: hands the app the plugins' static vtable getters, the same ones
// BuiltinPlugins.cpp registers when they are linked in.

#include <emscripten/emscripten.h>

#include "arrow_side_module_abi.h"

extern const PJ_data_source_vtable_t* pj_static_get_data_source_vtable_ParquetSource() noexcept;
extern const PJ_dialog_vtable_t* pj_static_get_dialog_vtable_ParquetDialog() noexcept;
extern const PJ_message_parser_vtable_t* pj_static_get_message_parser_vtable_ArrowParser() noexcept;
extern const PJ_toolbox_vtable_t* pj_static_get_toolbox_vtable_DataExporterToolbox() noexcept;
extern const PJ_dialog_vtable_t* pj_static_get_dialog_vtable_DataExporterDialog() noexcept;

// The module's one export: everything else it links is built with hidden visibility (cmake/PjWasmArrow.cmake).
extern "C" EMSCRIPTEN_KEEPALIVE PjArrowSidePlugins pj_arrow_side_plugins() {
  return {
      .parquet_source = pj_static_get_data_source_vtable_ParquetSource(),
      .parquet_dialog = pj_static_get_dialog_vtable_ParquetDialog(),
      .arrow_parser = pj_static_get_message_parser_vtable_ArrowParser(),
      .data_exporter = pj_static_get_toolbox_vtable_DataExporterToolbox(),
      .data_exporter_dialog = pj_static_get_dialog_vtable_DataExporterDialog(),
  };
}
