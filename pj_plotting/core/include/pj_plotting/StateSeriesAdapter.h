#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <QtGlobal>
#include <limits>
#include <vector>

#include "pj_base/type_tree.hpp"  // PrimitiveType
#include "pj_runtime/CurveDescriptor.h"

namespace PJ {

class SessionManager;

/// One contiguous run of an unchanged state label, in raw store nanoseconds (no display offset). Half-open
/// [t_start_raw_ns, t_end_raw_ns); the trailing run carries kOpenEnd and the consumer pins it to the display-range max
/// (bands always reach the right edge).
struct RawStateSegment {
  static constexpr qint64 kOpenEnd = std::numeric_limits<qint64>::max();
  qint64 t_start_raw_ns = 0;
  qint64 t_end_raw_ns = 0;
  QString value;
};

/// Reads one discrete series (string, integer or bool column) and run-length-encodes it into RawStateSegments. Each
/// sample becomes its canonical label (strings verbatim, integers in decimal, bool "true" / "false"); the formatting is
/// bijective, so label equality is value equality and everything downstream stays type-agnostic.
/// Every rebuild() re-scans the topic's whole retained range, with no watermark and no cached TopicChunk*, so it stays
/// correct under out-of-order chunk merges, retention eviction and dataset replaces (each readString view is copied
/// out inside the cursor callback). Discrete series are low-rate; an incremental path is the upgrade if this scan
/// shows on a hot path.
///
/// Run-length rules: equal-timestamp samples resolve to the last in cursor order (zero-duration runs collapse); a null
/// sample closes the current run (a gap until the next non-null); a value change closes the run at the new sample's
/// timestamp.
class StateSeriesAdapter {
 public:
  /// `source` must identify a discrete column of primitive `logical_type` (the caller checks
  /// CatalogModel::isDiscreteKey). A sample that cannot be read as that type (a fractional value in a float column, a
  /// kUnspecified column, a chunk sealed before the column appeared) is null.
  StateSeriesAdapter(SessionManager* session, CurveDescriptor source, PrimitiveType logical_type);

  /// Re-scans the store and rebuilds segments(). A failed range query keeps the previous segments (stale beats blank
  /// mid-stream); a missing topic yields none. Returns whether the scan succeeded.
  bool rebuild();

  /// Drops all segments (dataset replace or removal); the next rebuild() reads from scratch.
  void clear();

  [[nodiscard]] const std::vector<RawStateSegment>& segments() const noexcept {
    return segments_;
  }
  [[nodiscard]] const CurveDescriptor& source() const noexcept {
    return source_;
  }
  [[nodiscard]] PrimitiveType logicalType() const noexcept {
    return logical_type_;
  }

 private:
  SessionManager* session_ = nullptr;
  CurveDescriptor source_;
  PrimitiveType logical_type_ = PrimitiveType::kUnspecified;
  std::vector<RawStateSegment> segments_;
};

}  // namespace PJ
