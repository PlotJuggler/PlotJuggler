// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A data-source mock whose image stays resident after dlclose, two ways: the
// target links with -z nodelete (DF_1_NODELETE — compiler-independent), and
// PinAnchor's static member has vague linkage and default visibility, so GCC
// emits it as STB_GNU_UNIQUE and the dynamic relocation in `pin_anchor` binds
// it at dlopen, which also marks the DSO NODELETE — the mechanism real plugin
// builds pin by. The precondition for extension_manager_test's
// staged-promotion coverage.
//
// Compiled twice (PIN_MOCK_VERSION "1.0.0" / "2.0.0") so tests can stage a
// version upgrade of the same id.

#include "mock_data_source_vtable.h"

template <typename T>
struct PinAnchor {
  static int value;
};
template <typename T>
int PinAnchor<T>::value = 0;

int* pin_anchor = &PinAnchor<int>::value;

extern "C" PJ_DATA_SOURCE_EXPORT const uint32_t pj_plugin_abi_version = PJ_ABI_VERSION;

extern "C" PJ_DATA_SOURCE_EXPORT const PJ_data_source_vtable_t* PJ_get_data_source_vtable() noexcept {
  static const PJ_data_source_vtable_t vt = pj_mock::makeMockDataSourceVtable(
      R"({"id":"pinning-data-source","name":"Pinning DataSource","version":")" PIN_MOCK_VERSION R"("})");
  return &vt;
}
