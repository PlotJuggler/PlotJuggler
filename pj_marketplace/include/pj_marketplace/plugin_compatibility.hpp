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
// for real descriptors and must be strict SemVer. A zero ABI and empty floors
// mean "not declared" so older manifests remain admissible until the protocol
// loader performs its own ABI-symbol validation.
struct PluginCompatibilityRequirements {
  std::string_view version;
  uint32_t abi_major = 0;
  std::string_view min_sdk_required;
  std::string_view min_plotjuggler_version;
};

// The host contract against which admission, seeding, and runtime loading all
// evaluate the same claims. Tests may supply an explicit context; production
// callers normally use currentPluginHostCompatibility().
struct PluginHostCompatibility {
  uint32_t abi_major = PJ_ABI_VERSION;
  std::string_view sdk_version = sdkVersion();
  std::string_view plotjuggler_version;
};

struct PluginCompatibilityResult {
  bool ok = true;
  std::string reason;
};

// Evaluate every admission dimension in fail-closed order. Plugin values are
// strict SemVer 2.0.0. The application build is allowed one leading v/V
// because PJ4 accepts release tags in that form and strips the prefix when it
// configures project(VERSION); plugin manifests receive no such normalization.
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

  if (!requirements.min_plotjuggler_version.empty()) {
    const auto minimum = SemVer::parse(requirements.min_plotjuggler_version);
    if (!minimum) {
      return {
          false,
          "Invalid minimum PlotJuggler version \"" + std::string(requirements.min_plotjuggler_version) + "\"",
      };
    }

    std::string_view reported_host = host.plotjuggler_version;
    if (!reported_host.empty() && (reported_host.front() == 'v' || reported_host.front() == 'V')) {
      reported_host.remove_prefix(1);
    }
    const auto current = SemVer::parse(reported_host);
    if (!current) {
      return {
          false,
          "This build reports an invalid PlotJuggler version \"" + std::string(host.plotjuggler_version) + "\"",
      };
    }
    if (current->compare(*minimum) == std::strong_ordering::less) {
      return {
          false,
          "Requires PlotJuggler " + std::string(requirements.min_plotjuggler_version) + " or newer (this build is " +
              std::string(host.plotjuggler_version) + ")",
      };
    }
  }

  return {};
}

[[nodiscard]] inline PluginHostCompatibility currentPluginHostCompatibility(std::string_view plotjuggler_version) {
  return {PJ_ABI_VERSION, sdkVersion(), plotjuggler_version};
}

}  // namespace PJ
