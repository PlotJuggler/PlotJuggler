// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A second DSO for the mock-data-source 3.0.0 extension that needs PlotJuggler
// 4.0.0. Paired with the existing 5.0.0-floor fixture, it proves that a
// multi-DSO extension adopts its strictest compatible floor instead of requiring
// every component to repeat identical metadata.

#include "mock_data_source_vtable.h"

extern "C" PJ_DATA_SOURCE_EXPORT const uint32_t pj_plugin_abi_version = PJ_ABI_VERSION;

extern "C" PJ_DATA_SOURCE_EXPORT const PJ_data_source_vtable_t* PJ_get_data_source_vtable() noexcept {
  static const PJ_data_source_vtable_t vt = pj_mock::makeMockDataSourceVtable(
      R"({"id":"mock-data-source","name":"Mock DataSource helper","version":"3.0.0","min_sdk_required":"0.1.0"})");
  return &vt;
}
