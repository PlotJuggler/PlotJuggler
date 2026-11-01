// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Widget plugin fixture for the runtime-catalog tests. One source builds several libraries; CMake picks
// the manifest id, the Qt build the library claims in build_info(), and the compiler family through
// definitions. The manifest declares one module outside the shipped set on purpose.

#include "mock_widget_vtable.h"
#include "pj_base/plugin_data_api.h"

#ifndef PJ_TEST_WIDGET_ID
#error "PJ_TEST_WIDGET_ID must be defined"
#endif
#ifndef PJ_TEST_WIDGET_QT_MAJOR
#error "PJ_TEST_WIDGET_QT_MAJOR must be defined"
#endif
#ifndef PJ_TEST_WIDGET_QT_MINOR
#error "PJ_TEST_WIDGET_QT_MINOR must be defined"
#endif
#ifndef PJ_TEST_WIDGET_COMPILER
#error "PJ_TEST_WIDGET_COMPILER must be defined"
#endif

namespace {

const PJ_widget_build_info_t* buildInfo() noexcept {
  static const PJ_widget_build_info_t info =
      pj_mock::makeBuildInfo({PJ_TEST_WIDGET_QT_MAJOR, PJ_TEST_WIDGET_QT_MINOR, PJ_TEST_WIDGET_COMPILER});
  return &info;
}

}  // namespace

extern "C" PJ_WIDGET_EXPORT const uint32_t pj_plugin_abi_version = PJ_ABI_VERSION;

extern "C" PJ_WIDGET_EXPORT const PJ_widget_vtable_t* PJ_get_widget_vtable() noexcept {
  static const PJ_widget_vtable_t vt = pj_mock::makeMockWidgetVtable(
      R"({"id":")" PJ_TEST_WIDGET_ID R"(","name":"Test Widgets","version":"1.2.3",)"
      R"("qt_modules":["Core","Gui","Widgets","Qt3DCore"],"widgets":[)"
      R"({"type":"gauge","name":"Gauge","icon":"<svg/>","settings":[{"key":"min","type":"number","default":0}]},)"
      R"({"type":"table","name":"Table"}]})",
      &buildInfo);
  return &vt;
}
