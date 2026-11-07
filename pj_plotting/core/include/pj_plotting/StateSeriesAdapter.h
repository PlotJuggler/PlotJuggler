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

/// One contiguous run of an unchanged state label, in RAW store nanoseconds
/// (no display offset applied). Half-open [t_start_raw_ns, t_end_raw_ns); the
/// trailing run carries kOpenEnd and is pinned to the display-range max by the
/// consumer (bands always reach the right edge, "so far" semantics).
struct RawStateSegment {
  static constexpr qint64 kOpenEnd = std::numeric_limits<qint64>::max();
  qint64 t_start_raw_ns = 0;
  qint64 t_end_raw_ns = 0;
  QString value;
};

/// Reads ONE discrete series (string / integer / bool column) out of the
/// datastore and run-length-encodes it into RawStateSegments. Each sample is
/// formatted into its canonical label at read time (strings verbatim, integers
/// in decimal, bool as "true"/"false") — the formatting is bijective, so label
/// equality is value equality and everything downstream stays type-agnostic.
/// Full-rebuild model: every rebuild() re-scans the topic's whole retained
/// range — no watermark, no cached TopicChunk* — so it is correct by
/// construction under out-of-order chunk merges, retention eviction, and
/// dataset replaces (every readString string_view is copied out inside the
/// cursor callback). Discrete series are low-rate; if profiling ever shows
/// this scan on a hot path, an incremental fast path is a later optimization.
///
/// RLE semantics (the widget-family contract): equal-timestamp samples resolve
/// to the LAST in cursor order (zero-duration runs collapse away); a null
/// sample closes the current run (gap until the next non-null); a value change
/// closes the run at the new sample's timestamp.
class StateSeriesAdapter {
 public:
  /// `source` must identify a discrete column of primitive `logical_type` (the
  /// caller validates via CatalogModel::isDiscreteKey before constructing). A
  /// sample whose value cannot be read as that type — a fractional value in a
  /// float column, a kUnspecified column, or a chunk sealed before the column
  /// appeared — is treated as null.
  StateSeriesAdapter(SessionManager* session, CurveDescriptor source, PrimitiveType logical_type);

  /// Re-scan the store and rebuild segments(). On a failed range query the
  /// previous segments are kept (stale beats blank mid-stream); a missing topic
  /// yields empty segments. Returns whether the scan succeeded.
  bool rebuild();

  /// Drop all cached segments (dataset replace / removal). The next rebuild()
  /// re-reads from scratch.
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
