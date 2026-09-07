// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A data-source mock sharing the id "mock-data-source" with the other mocks, at a
// LOW version (1.5.0) but declaring a min_sdk_required (99.0.0) that no test
// host satisfies. Paired with mock_data_source_incompatible (v3.0.0, same min) it
// lets a test place incompatible candidates both below and above a compatible one.

#include "mock_data_source_vtable.h"

extern "C" PJ_DATA_SOURCE_EXPORT const uint32_t pj_plugin_abi_version = PJ_ABI_VERSION;

extern "C" PJ_DATA_SOURCE_EXPORT const PJ_data_source_vtable_t* PJ_get_data_source_vtable() noexcept {
  static const PJ_data_source_vtable_t vt = pj_mock::makeMockDataSourceVtable(
      R"({"id":"mock-data-source","name":"Mock DataSource","version":"1.5.0","min_sdk_required":"99.0.0"})");
  return &vt;
}
