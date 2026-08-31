// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A loadable DSO whose required manifest version is non-SemVer. Discovery must
// keep an already-installed copy manageable, while every admission/load path
// rejects it before activation.

#include "mock_data_source_vtable.h"

extern "C" PJ_DATA_SOURCE_EXPORT const uint32_t pj_plugin_abi_version = PJ_ABI_VERSION;

extern "C" PJ_DATA_SOURCE_EXPORT const PJ_data_source_vtable_t* PJ_get_data_source_vtable() noexcept {
  static const PJ_data_source_vtable_t vt =
      pj_mock::makeMockDataSourceVtable(R"({"id":"malformed-version","name":"Malformed Version","version":"4.1"})");
  return &vt;
}
