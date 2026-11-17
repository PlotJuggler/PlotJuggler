// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <pj_plugins/dialog_protocol.h>

#include <QCoreApplication>
#include <cstdint>
#include <pj_base/sdk/version.hpp>
#include <pj_plugins/host/dialog_handle.hpp>
#include <string>
#include <string_view>

namespace PJ::detail {

/// Tells a freshly constructed dialog the SDK and application versions and the protocol features the host supports
/// (`capabilities`, PJ_DIALOG_HOST_* bits). Called once from the engine's constructor, before the first
/// ui_content() / widget_data(). A dialog that predates `set_host_info` or rejects it keeps reading "no host
/// info": not an error.
inline void announceHostInfo(DialogHandle& handle, uint64_t capabilities) {
  const std::string_view sdk_version = sdkVersion();
  const std::string app_version = QCoreApplication::applicationVersion().toStdString();
  PJ_dialog_host_info_t info{};
  info.struct_size = sizeof(info);
  info.sdk_version = PJ_string_view_t{sdk_version.data(), sdk_version.size()};
  info.plotjuggler_version = PJ_string_view_t{app_version.data(), app_version.size()};
  info.capabilities = capabilities;
  (void)handle.setHostInfo(info);
}

}  // namespace PJ::detail
