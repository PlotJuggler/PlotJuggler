#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The one entry point of the browser's Arrow side module (plugins/arrow.wasm, cmake/PjWasmArrow.cmake), shared by
// the module (arrow_side_module.cpp) and the app that dlopen()s it (ArrowSideModule.cpp). Plain C: the two are
// separate WebAssembly modules, each with its own copy of the plugin SDK, and meet only at these vtables.

#include "pj_base/data_source_protocol.h"
#include "pj_base/message_parser_protocol.h"
#include "pj_base/toolbox_protocol.h"

extern "C" {

// Every pointer has static storage duration in the module, which is never unloaded.
struct PjArrowSidePlugins {
  const PJ_data_source_vtable_t* parquet_source;
  const PJ_dialog_vtable_t* parquet_dialog;
  const PJ_message_parser_vtable_t* arrow_parser;
  const PJ_toolbox_vtable_t* data_exporter;
  const PJ_dialog_vtable_t* data_exporter_dialog;
};

// dlsym() name of the entry point.
#define PJ_ARROW_SIDE_ENTRY "pj_arrow_side_plugins"
using PjArrowSideEntry = PjArrowSidePlugins (*)();
}
