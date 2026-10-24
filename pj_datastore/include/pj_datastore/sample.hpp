#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "pj_base/types.hpp"                    // PJ::Timestamp
#include "pj_datastore/incremental_engine.hpp"  // PJ::VarValue

namespace PJ::proc {

/// One co-timestamped processor sample: an absolute timestamp plus N channel
/// values. SISO filters use exactly one channel (`value()`); MIMO (deferred)
/// uses up to `kMaxChannels`.
///
/// Values are stored INLINE — the engine adapter builds a `Sample` per input on
/// the hot path, so a heap allocation per sample (a `std::vector` member) would
/// be a real cost; the fixed inline array avoids it while keeping the N->M shape.
///
/// `raw_ts_ns` is the absolute int64-ns spine, NOT a display-relative time: the
/// datastore is display-base-agnostic, so eager processors key off `raw_ts_ns`
/// and any session-relative value is computed only in the dialog/preview layer.
class Sample {
 public:
  /// Inline channel capacity. Covers SISO (1) and the deferred quaternion (4-in).
  static constexpr std::size_t kMaxChannels = 4;

  PJ::Timestamp raw_ts_ns = 0;

  Sample() = default;

  /// Build a single-channel (SISO) sample — the dominant path.
  static Sample scalar(PJ::Timestamp t, PJ::VarValue v) {
    Sample s;
    s.raw_ts_ns = t;
    s.values_[0] = std::move(v);
    s.count_ = 1;
    return s;
  }

  [[nodiscard]] std::size_t channelCount() const {
    return count_;
  }
  void setChannelCount(std::size_t n) {
    count_ = static_cast<std::uint8_t>(n);
  }

  /// Channel accessor; channel 0 is the SISO value. Caller ensures `i` < `channelCount()`.
  [[nodiscard]] const PJ::VarValue& value(std::size_t i = 0) const {
    return values_[i];
  }
  PJ::VarValue& value(std::size_t i = 0) {
    return values_[i];
  }

 private:
  std::array<PJ::VarValue, kMaxChannels> values_{};
  std::uint8_t count_ = 0;
};

}  // namespace PJ::proc
