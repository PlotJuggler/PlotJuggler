#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Inert Qt-free Widget plugin vtable for the runtime-catalog tests. It creates no widget object
// (create_qwidget returns null), so it can run in a binary that links no Qt GUI module. The Qt build it
// claims in build_info() is chosen by the caller, which is how a mismatching library is faked.

#include <cstdint>
#include <cstring>

#include "pj_plugins/widget_protocol.h"

namespace pj_mock {

struct MockWidgetBuild {
  uint32_t qt_major;
  uint32_t qt_minor;
  const char* compiler;
};

inline void* createWidget(PJ_string_view_t type) noexcept {
  const bool known = (type.size == 5 && std::memcmp(type.data, "gauge", 5) == 0) ||
                     (type.size == 5 && std::memcmp(type.data, "table", 5) == 0);
  return known ? new int(0) : nullptr;
}

inline void destroyWidget(void* ctx) noexcept {
  delete static_cast<int*>(ctx);
}

inline bool bindWidget(void*, PJ_service_registry_t, PJ_error_t*) noexcept {
  return true;
}

inline bool widgetInputs(void*, const PJ_widget_input_t** out_inputs, uint32_t* out_count, PJ_error_t*) noexcept {
  *out_inputs = nullptr;
  *out_count = 0;
  return true;
}

inline PJ_qwidget_t createQWidget(void*, PJ_qwidget_t, PJ_error_t*) noexcept {
  return nullptr;
}

inline void widgetOnUpdate(void*, const PJ_widget_value_t*, uint32_t, double) noexcept {}

inline bool widgetSaveConfig(void*, PJ_string_view_t* out_json, PJ_error_t*) noexcept {
  *out_json = PJ_string_view_t{"{}", 2};
  return true;
}

inline bool widgetLoadConfig(void*, PJ_string_view_t, PJ_error_t*) noexcept {
  return true;
}

inline const void* widgetExtension(void*, PJ_string_view_t) noexcept {
  return nullptr;
}

inline PJ_widget_build_info_t makeBuildInfo(const MockWidgetBuild& build) {
  PJ_widget_build_info_t info{};
  info.struct_size = sizeof(PJ_widget_build_info_t);
  info.qt_major = build.qt_major;
  info.qt_minor = build.qt_minor;
  info.qt_patch = 0;
  info.flags = 0;
  info.compiler = PJ_string_view_t{build.compiler, std::strlen(build.compiler)};
  info.qt_version_str = PJ_string_view_t{"test", 4};
  return info;
}

/// Vtable filled with the stubs above. `build_info` and `manifest` are the only per-plugin parts.
inline PJ_widget_vtable_t makeMockWidgetVtable(
    const char* manifest, const PJ_widget_build_info_t* (*build_info)() noexcept) {
  PJ_widget_vtable_t vt{};
  vt.protocol_version = PJ_WIDGET_PLUGIN_PROTOCOL_VERSION;
  vt.struct_size = sizeof(PJ_widget_vtable_t);
  vt.create = &createWidget;
  vt.destroy = &destroyWidget;
  vt.manifest_json = manifest;
  vt.build_info = build_info;
  vt.bind = &bindWidget;
  vt.inputs = &widgetInputs;
  vt.create_qwidget = &createQWidget;
  vt.on_update = &widgetOnUpdate;
  vt.save_config = &widgetSaveConfig;
  vt.load_config = &widgetLoadConfig;
  vt.get_plugin_extension = &widgetExtension;
  return vt;
}

}  // namespace pj_mock
