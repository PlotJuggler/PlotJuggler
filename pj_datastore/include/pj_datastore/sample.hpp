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

/// One processor sample: a timestamp plus up to `kMaxChannels` values in a fixed inline array. SISO processors use
/// channel 0 only.
///
/// `raw_ts_ns` is the datastore's absolute time in nanoseconds, not display-relative: any display offset is applied
/// by the UI layers.
class Sample {
 public:
  static constexpr std::size_t kMaxChannels = 4;

  PJ::Timestamp raw_ts_ns = 0;

  Sample() = default;

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

  /// Unchecked: `i` must be below channelCount().
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
