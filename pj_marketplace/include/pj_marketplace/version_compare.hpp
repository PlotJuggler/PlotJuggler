#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

/**
 * @file version_compare.hpp
 * @brief Ordering for plugin package versions.
 *
 * Admission accepts only concrete SemVer 2.0.0 versions. This helper also has
 * to sort recovery/UI data that may predate that invariant, so valid versions
 * use the SDK's canonical SemVer precedence, a valid version outranks an
 * invalid one, and two invalid strings use a deterministic byte-wise fallback.
 * Invalid input never becomes installable merely because it can be displayed.
 */

#include <compare>
#include <string_view>

#include "pj_base/sdk/semver.hpp"

namespace PJ {

[[nodiscard]] inline bool isPluginVersionValid(std::string_view version) {
  return SemVer::isValid(version);
}

// Returns <0, 0, or >0 like strcmp. Build metadata compares equal because SDK
// SemVer equality follows precedence equivalence. The invalid-data fallback is
// intentionally not SemVer-like; it exists only to keep already-corrupt rows
// and recovery decisions stable after admission has rejected the value.
[[nodiscard]] inline int comparePluginVersions(std::string_view lhs, std::string_view rhs) {
  const auto lhs_version = SemVer::parse(lhs);
  const auto rhs_version = SemVer::parse(rhs);

  if (lhs_version && rhs_version) {
    const std::strong_ordering order = lhs_version->compare(*rhs_version);
    return order == std::strong_ordering::less ? -1 : (order == std::strong_ordering::greater ? 1 : 0);
  }
  if (static_cast<bool>(lhs_version) != static_cast<bool>(rhs_version)) {
    return lhs_version ? 1 : -1;
  }
  const int fallback = lhs.compare(rhs);
  return fallback < 0 ? -1 : (fallback > 0 ? 1 : 0);
}

}  // namespace PJ
