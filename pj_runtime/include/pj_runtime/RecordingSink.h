// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <filesystem>

#include "pj_base/expected.hpp"
#include "pj_base/span.hpp"
#include "pj_runtime/RecordingTypes.h"

namespace PJ {

/// Where a Recorder writes: open, then any mix of addChannel/write, then close.
/// Strictly sequenced but not on one thread — open() runs on the thread that
/// starts the recording, close() on the one that stops it (after the writer was
/// joined), everything between on the writer thread. The recorder's thread start
/// and join order those handoffs, so a sink needs no locking of its own.
class RecordingSink {
 public:
  virtual ~RecordingSink() = default;
  [[nodiscard]] virtual Status open(const std::filesystem::path& path, const RecordingInfo& info) = 0;
  /// Returns the sink's channel id for later write() calls.
  [[nodiscard]] virtual Expected<uint16_t> addChannel(const RecordedBinding& binding) = 0;
  [[nodiscard]] virtual Status write(uint16_t channel, int64_t log_time_ns, Span<const uint8_t> bytes) = 0;
  [[nodiscard]] virtual Status close(const RecordingSummary& summary) = 0;
};

}  // namespace PJ
