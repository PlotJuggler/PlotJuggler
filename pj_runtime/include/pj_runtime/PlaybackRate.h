#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <algorithm>
#include <cmath>

namespace PJ {

/// The playback speed range and step, defined once: the speed scrubber takes its bounds, step and decimals from them,
/// PlaybackEngine snaps every rate to them (so a plugin or the MCP `playback` tool cannot play at a speed the scrubber
/// cannot show), and the tool snaps before the host sees the value. A header of its own so the Qt-free agent tools can
/// include it.
inline constexpr double kMinPlaybackRate = 0.1;
inline constexpr double kMaxPlaybackRate = 10.0;
inline constexpr double kPlaybackRateStep = 0.1;

/// `rate` rounded to kPlaybackRateStep, then clamped to [kMinPlaybackRate, kMaxPlaybackRate]: the rate the speed
/// scrubber can show. Dividing by the reciprocal keeps the result free of float noise (0.3, not 0.30000000000000004).
/// A finite `rate` only.
inline double snapPlaybackRate(double rate) {
  return std::clamp(
      std::round(rate / kPlaybackRateStep) / (1.0 / kPlaybackRateStep), kMinPlaybackRate, kMaxPlaybackRate);
}

}  // namespace PJ
