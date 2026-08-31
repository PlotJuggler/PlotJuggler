// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A valid plugin requiring a deliberately future SDK. The manifest is valid
// SemVer and therefore survives SDK discovery; PJ4 must reject it at admission,
// staged promotion, and runtime loading.

#include "mock_data_source_vtable.h"

extern "C" PJ_DATA_SOURCE_EXPORT const uint32_t pj_plugin_abi_version = PJ_ABI_VERSION;

extern "C" PJ_DATA_SOURCE_EXPORT const PJ_data_source_vtable_t* PJ_get_data_source_vtable() noexcept {
  static const PJ_data_source_vtable_t vt = pj_mock::makeMockDataSourceVtable(
      R"({"id":"newer-sdk","name":"Newer SDK","version":"1.0.0","min_sdk_required":"99.0.0"})");
  return &vt;
}
