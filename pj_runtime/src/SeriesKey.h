#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <string_view>

namespace PJ {

/// Whether the series key `key` is rooted at `topic`: the topic itself or a field
/// below a slash. The slash boundary keeps similarly prefixed topics apart (`imu` is
/// not `imu_raw`). An empty `topic` roots nothing.
inline bool seriesKeyBelongsToTopic(std::string_view key, std::string_view topic) {
  return !topic.empty() &&
         (key == topic || (key.size() > topic.size() && key.starts_with(topic) && key[topic.size()] == '/'));
}

}  // namespace PJ
