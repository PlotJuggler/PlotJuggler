// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "pj_base/builtin/point_cloud.hpp"
#include "pj_base/expected.hpp"
#include "pj_scripting/memory_budget.h"
#include "point_cloud_ops.h"

// The record walk and the compaction every point-cloud operation shares, in a header so the tests can drive
// them with a counting predicate. `cloud` must have passed the layout checks of point_cloud_ops.cpp.
namespace PJ::scripting::native {

/// True when `index` is a poll point (every 65536 records) and the run's cancel check reports a cancel.
inline bool cancelledAt(const NativeLimits& limits, std::size_t index) {
  return (index & 0xFFFF) == 0 && limits.cancelled && limits.cancelled();
}

/// Calls `visit(record, index)` for each of the `records` records in index order, walking each row by pointer
/// increments, with no per-record division. Polls `limits.cancelled` every 65536 records and returns false,
/// having stopped, once it reports a cancel. `data` is `cloud.data` or a same-layout copy of it; it may be
/// empty (null) when `records` is 0, so no pointer arithmetic happens then.
template <class Byte, class Visit>
bool forEachRecord(
    const sdk::PointCloud& cloud, Byte* data, std::size_t records, const NativeLimits& limits, Visit&& visit) {
  if (records == 0) {
    return true;
  }
  const bool organized = cloud.row_step != 0 && cloud.height > 1;
  const std::size_t rows = organized ? cloud.height : 1;
  const std::size_t columns = organized ? cloud.width : records;
  std::size_t index = 0;
  for (std::size_t row = 0; row < rows; ++row) {
    Byte* record = data + row * (organized ? cloud.row_step : 0);
    for (std::size_t column = 0; column < columns; ++column, ++index, record += cloud.point_step) {
      if (cancelledAt(limits, index)) {
        return false;
      }
      visit(record, index);
    }
  }
  return true;
}

/// Copies the `kept` records whose bit is set in `mask` into a compacted cloud (height 1), charged and
/// allocated at its exact size. It visits only the set bits (a per-record bit test mispredicts on a real
/// scan and cost twice the predicate pass) and stops at the last kept one, so a cancel that arrives once
/// the output is complete never turns it into an error.
[[nodiscard]] Expected<sdk::PointCloud> copyMarked(
    const sdk::PointCloud& cloud, const std::vector<uint64_t>& mask, std::size_t kept, const NativeLimits& limits);

/// Copies every record for which `keep(record)` holds into a compacted cloud (height 1). `keep` runs once per
/// record, into a bitmap (one bit per record, charged to the budget while the copy runs), so the output is
/// charged and allocated at its exact size: a small region of a large scan fits a small budget.
template <class Keep>
[[nodiscard]] Expected<sdk::PointCloud> compactWhere(
    const sdk::PointCloud& cloud, std::size_t records, const NativeLimits& limits, Keep&& keep) {
  const std::size_t words = (records + 63) / 64;
  MemoryReservation mask_charge(limits.budget);
  if (Status reserved = mask_charge.grow(words * sizeof(uint64_t)); !reserved) {
    return unexpected(reserved.error());
  }
  std::vector<uint64_t> mask(words);
  std::size_t kept = 0;
  uint64_t word = 0;
  const bool finished =
      forEachRecord(cloud, cloud.data.data(), records, limits, [&](const uint8_t* record, std::size_t index) {
        const bool hit = keep(record);
        word |= uint64_t{hit} << (index % 64);
        kept += static_cast<std::size_t>(hit);
        if (index % 64 == 63) {
          mask[index / 64] = word;
          word = 0;
        }
      });
  if (!finished) {
    return unexpected(std::string(kCancelledError));
  }
  if (records % 64 != 0) {
    mask[records / 64] = word;
  }
  return copyMarked(cloud, mask, kept, limits);
}

}  // namespace PJ::scripting::native
