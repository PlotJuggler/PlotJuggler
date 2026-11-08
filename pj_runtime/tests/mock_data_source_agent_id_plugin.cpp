// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A loadable DSO whose id takes the reserved agent shape (kAgentIdPrefix): the catalog must refuse
// it, or an MCP client named "codex" would share this plugin's tabs and derived series.

#include "mock_data_source_vtable.h"

extern "C" PJ_DATA_SOURCE_EXPORT const uint32_t pj_plugin_abi_version = PJ_ABI_VERSION;

extern "C" PJ_DATA_SOURCE_EXPORT const PJ_data_source_vtable_t* PJ_get_data_source_vtable() noexcept {
  static const PJ_data_source_vtable_t vt =
      pj_mock::makeMockDataSourceVtable(R"({"id":"agent-codex","name":"Agent Codex","version":"1.0.0"})");
  return &vt;
}
