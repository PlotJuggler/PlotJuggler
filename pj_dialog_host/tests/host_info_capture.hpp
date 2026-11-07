// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <pj_plugins/sdk/dialog_plugin_base.hpp>
#include <string>

/// What set_host_info delivered before a test plugin's first ui_content(): the engine
/// announces itself once after construction. Shared by the dialog and panel test plugins.
struct HostInfoCapture {
  bool seen = false;
  uint64_t capabilities = 0;
  std::string sdk_version;

  void record(const std::optional<PJ::DialogHostInfo>& info) {
    if (info.has_value()) {
      seen = true;
      capabilities = info->capabilities;
      sdk_version = info->sdk_version;
    }
  }
};
