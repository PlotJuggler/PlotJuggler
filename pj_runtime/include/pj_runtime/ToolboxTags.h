#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <algorithm>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace PJ {

// Manifest `tags` that change where the shell offers a toolbox. The Toolbox
// menu, the Sources panel's Cloud list and the feature catalog's entry points
// all read these and must agree on what each tag means.

/// Listed in the Sources panel's Cloud tab instead of the Toolbox menu.
inline constexpr std::string_view kToolboxTagCloud = "cloud";
/// Listed nowhere; the toolbox is launched from its own entry point (e.g. the
/// Transform Editor, from the "+" in Custom Series).
inline constexpr std::string_view kToolboxTagHidden = "hidden";

/// The string entries of a parsed manifest's "tags" array. A non-object
/// manifest or a missing/malformed array has no tags.
[[nodiscard]] inline std::vector<std::string> manifestTags(const nlohmann::json& manifest) {
  std::vector<std::string> tags;
  if (!manifest.is_object()) {
    return tags;
  }
  const auto it = manifest.find("tags");
  if (it == manifest.end() || !it->is_array()) {
    return tags;
  }
  for (const auto& entry : *it) {
    if (entry.is_string()) {
      tags.push_back(entry.get<std::string>());
    }
  }
  return tags;
}

/// True when a parsed manifest's "tags" array holds `tag`.
[[nodiscard]] inline bool manifestHasTag(const nlohmann::json& manifest, std::string_view tag) {
  const auto tags = manifestTags(manifest);
  return std::find(tags.begin(), tags.end(), tag) != tags.end();
}

}  // namespace PJ
