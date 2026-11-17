#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <compare>
#include <cstdint>

namespace PJ {

/// Stable entry identity: 0 is the invalid sentinel, real ids start at 1 and increase. Unlike a deque index, an id is
/// not reused when retention evicts old entries.
///
/// Ids come from one process-wide counter, so consecutive entries of a topic are not consecutive integers: do not
/// iterate a topic by incrementing; ObjectStore::drainNewSince() drains by cursor.
struct SequentialUID {
  static constexpr uint64_t kInvalidValue = 0;
  static constexpr uint64_t kFirstValidValue = 1;

  uint64_t value = kInvalidValue;

  [[nodiscard]] bool valid() const {
    return value != kInvalidValue;
  }

  /// Thread-safe.
  [[nodiscard]] static SequentialUID getNext() noexcept;

  friend auto operator<=>(SequentialUID, SequentialUID) = default;
};

}  // namespace PJ
