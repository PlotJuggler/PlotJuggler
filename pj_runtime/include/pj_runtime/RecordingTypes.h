// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>

namespace PJ {

/// One delegated-ingest parser binding as the sink writes it: the verbatim
/// signature the plugin handed to ensureParserBinding. It carries no source
/// identity — a recording holds exactly one source, so that identity is a
/// file-level fact, not a per-channel one. Parser POLICY — timestamp field,
/// array limits — is deliberately absent: it is interpretation and lives in the
/// layout (D9).
struct RecordedBinding {
  std::string topic;
  std::string encoding;  ///< parser encoding, e.g. "cdr", "protobuf", "json"
  std::string type_name;
  std::string schema_bytes;  ///< may be empty
};

/// Written into the file-level `pj.recording` metadata record at open().
struct RecordingInfo {
  std::string app_version;
  /// Ties the files of one Record press together: every source of a capture
  /// writes the same token, so a batch is recognisable without its folder.
  std::string capture_id;
  std::string source_display_name;
  std::string source_plugin_id;
  uint32_t capture_ordinal = 0;  ///< 1-based position of this source in its capture
  std::string started_utc;       ///< ISO-8601
};

/// Final state of a recording, also written into `pj.recording` on close.
struct RecordingSummary {
  std::string stopped_utc;
  /// Why the recording ended: `stopped` (the user), `source_ended` (its source
  /// went away), `truncated` (a failure ended the data) or `shutdown`. Recorded
  /// in the file, so a reader can tell a deliberate end from a lost one.
  std::string terminal_cause;
  bool truncated = false;
  std::string truncated_reason;
  /// The sink could not finalize: no footer, or bytes that are not durable.
  /// The file still opens by a linear scan, and `mcap recover` rebuilds its
  /// index. A truncated recording is not this: it closes into a complete file.
  bool file_incomplete = false;
  uint64_t messages = 0;
  uint64_t payload_bytes = 0;  ///< message payloads written, NOT the file size
  /// Messages the queue's byte budget forced out, the largest losing first, so
  /// a message bigger than the whole budget is never recorded. Distinct from
  /// the skipped count a recording's owner reports: skipped never reached the
  /// recorder, dropped was evicted once it had. Each drop is a hole in its
  /// channel, never a reordering.
  uint64_t dropped_messages = 0;
};

/// Live counters for the status label.
struct RecordingStats {
  uint64_t messages = 0;
  uint64_t payload_bytes = 0;
  uint64_t dropped_messages = 0;  ///< see RecordingSummary::dropped_messages
};

/// What the runtime host does after a tap call. kStopRecording detaches the
/// tap for good — the recording ended, ingest continues untouched. A message
/// the recorder drops is not a verdict: the tap stays attached.
enum class TapVerdict { kContinue, kStopRecording };

}  // namespace PJ
