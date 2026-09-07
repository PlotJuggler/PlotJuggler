#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

#include "pj_base/plugin_data_api.h"
#include "pj_base/sdk/semver.hpp"
#include "pj_base/sdk/version.hpp"

namespace PJ {

// The admission claims carried by one plugin descriptor. Version is required
// for real descriptors and must be strict SemVer. A zero ABI and an empty
// floor mean "not declared" so older manifests remain admissible until the
// protocol loader performs its own ABI-symbol validation. The manifest's
// min_plotjuggler_version is deprecated and no longer gates: abi_major is the
// generation gate, and app-level feature gaps degrade rather than block.
struct PluginCompatibilityRequirements {
  std::string_view version;
  uint32_t abi_major = 0;
  std::string_view min_sdk_required;
};

// The host contract against which admission, seeding, and runtime loading all
// evaluate the same claims. Tests may supply an explicit context; production
// callers normally use currentPluginHostCompatibility().
struct PluginHostCompatibility {
  uint32_t abi_major = PJ_ABI_VERSION;
  std::string_view sdk_version = sdkVersion();
};

struct PluginCompatibilityResult {
  bool ok = true;
  std::string reason;
};

// Evaluate every admission dimension in fail-closed order. Plugin values are
// strict SemVer 2.0.0.
[[nodiscard]] inline PluginCompatibilityResult evaluatePluginCompatibility(
    const PluginCompatibilityRequirements& requirements, const PluginHostCompatibility& host) {
  if (!requirements.version.empty()) {
    const auto version = SemVer::parse(requirements.version);
    if (!version) {
      return {false, "Invalid plugin version \"" + std::string(requirements.version) + "\""};
    }
  }

  if (requirements.abi_major != 0 && requirements.abi_major != host.abi_major) {
    return {
        false,
        "Plugin ABI " + std::to_string(requirements.abi_major) + " does not match host ABI " +
            std::to_string(host.abi_major),
    };
  }

  if (!requirements.min_sdk_required.empty()) {
    const auto minimum = SemVer::parse(requirements.min_sdk_required);
    if (!minimum) {
      return {false, "Invalid minimum SDK version \"" + std::string(requirements.min_sdk_required) + "\""};
    }
    const auto current = SemVer::parse(host.sdk_version);
    if (!current) {
      return {false, "This build reports an invalid SDK version \"" + std::string(host.sdk_version) + "\""};
    }
    if (current->compare(*minimum) == std::strong_ordering::less) {
      return {
          false,
          "Requires PlotJuggler SDK " + std::string(requirements.min_sdk_required) + " or newer (this build uses " +
              std::string(host.sdk_version) + ")",
      };
    }
  }

  return {};
}

// Feature-completeness note, deliberately SEPARATE from the binary gate above:
// it must never affect ok/reason — install enforcement and admission are
// fail-closed on that result, while this is display-only. `suggested` is the
// manifest's suggested_sdk_version (the full-feature floor). Returns the
// human-readable note when the host's SDK is older than the claim, empty when
// full-featured, absent, or unparseable (fail-open: a bad claim never blocks
// and never warns).
[[nodiscard]] inline std::string evaluateFeatureCompleteness(
    std::string_view suggested, std::string_view host_sdk = sdkVersion()) {
  if (suggested.empty()) {
    return {};
  }
  const auto wanted = SemVer::parse(suggested);
  const auto current = SemVer::parse(host_sdk);
  if (!wanted || !current) {
    return {};
  }
  if (current->compare(*wanted) == std::strong_ordering::less) {
    return "Works on this version with reduced features; full features need a newer PlotJuggler (SDK " +
           std::string(suggested) + " or newer, this build uses " + std::string(host_sdk) + ")";
  }
  return {};
}

[[nodiscard]] inline PluginHostCompatibility currentPluginHostCompatibility() {
  return {PJ_ABI_VERSION, sdkVersion()};
}

}  // namespace PJ
