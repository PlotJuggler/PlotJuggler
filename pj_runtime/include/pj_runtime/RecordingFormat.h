// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string_view>

namespace PJ {

// The on-disk contract of a recorded MCAP file, shared with the
// `data_load_mcap` replay path: the writer emits these names and the loader
// keys on them, so changing one is a file-format break, not a rename.

/// File-level metadata record holding the recording's start and final facts.
/// A recording holds exactly ONE source, so the source identity lives here and
/// channels carry no PJ metadata at all.
inline constexpr std::string_view kRecordingMetadataName = "pj.recording";

// The `terminal_cause` field of the closing record: why this recording ended.
// A reader distinguishes "the user stopped it" from "its source went away" and
// from a failure that ended the data.
inline constexpr std::string_view kTerminalCauseStopped = "stopped";
inline constexpr std::string_view kTerminalCauseSourceEnded = "source_ended";
inline constexpr std::string_view kTerminalCauseTruncated = "truncated";
inline constexpr std::string_view kTerminalCauseShutdown = "shutdown";

/// Written as the `version` field of every `pj.recording` record.
inline constexpr uint32_t kRecordingFormatVersion = 1;

}  // namespace PJ
