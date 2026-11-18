#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

namespace PJ {

/// The playback speed range, defined once: PlaybackEngine clamps every rate to it (so a plugin or the MCP `playback`
/// tool cannot play at a speed the speed scrubber cannot show), the scrubber takes its bounds from it, and the tool
/// clamps to it before the host sees the value. A header of its own so the Qt-free agent tools can include it.
inline constexpr double kMinPlaybackRate = 0.1;
inline constexpr double kMaxPlaybackRate = 10.0;

}  // namespace PJ
