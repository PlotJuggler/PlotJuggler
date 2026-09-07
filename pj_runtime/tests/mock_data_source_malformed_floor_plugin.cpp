// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A loadable DSO whose manifest carries a malformed PlotJuggler floor. The SDK
// intentionally leaves that application-owned field to the host, so this
// fixture proves every PJ4 path fails closed without making the installed
// payload disappear from marketplace management.

#include "mock_data_source_vtable.h"

extern "C" PJ_DATA_SOURCE_EXPORT const uint32_t pj_plugin_abi_version = PJ_ABI_VERSION;

extern "C" PJ_DATA_SOURCE_EXPORT const PJ_data_source_vtable_t* PJ_get_data_source_vtable() noexcept {
  static const PJ_data_source_vtable_t vt = pj_mock::makeMockDataSourceVtable(
      R"({"id":"malformed-floor","name":"Malformed Floor","version":"1.0.0","min_sdk_required":"4.1"})");
  return &vt;
}
