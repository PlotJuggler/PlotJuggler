// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/StateSeriesAdapter.h"

#include <limits>
#include <optional>
#include <string_view>
#include <utility>

#include "pj_datastore/chunk.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_runtime/SessionManager.h"
using namespace Qt::StringLiterals;

namespace PJ {
namespace {

/// The canonical label for one non-null sample, or nullopt for a type outside
/// the discrete family OR a chunk whose column disagrees with the expected
/// type (treated as null by the caller — the per-chunk check keeps the failure
/// mode uniform: without it a mismatched string column would throw through
/// readString while numeric/bool columns silently fabricate 0/false). Caller
/// has already bounds-checked column_index against the chunk. Bool goes
/// through readBool — packed-bool encodings return 0 through the numeric
/// reads — and unsigned types through the uint64 read so values above 2^53
/// stay exact.
[[nodiscard]] std::optional<QString> formatStateLabel(
    const TopicChunk& chunk, std::size_t column_index, std::size_t row_index, PrimitiveType logical_type) {
  const auto& descriptor = chunk.columns[column_index].descriptor;
  if (descriptor == nullptr || descriptor->logical_type != logical_type) {
    return std::nullopt;
  }
  switch (logical_type) {
    case PrimitiveType::kString: {
      // Copy the string_view out IMMEDIATELY — it views chunk-internal
      // dictionary memory that must not outlive the cursor. An empty string is
      // a valid label, distinct from null.
      const std::string_view view = chunk.readString(column_index, row_index);
      return QString::fromUtf8(view.data(), static_cast<qsizetype>(view.size()));
    }
    case PrimitiveType::kBool:
      return chunk.readBool(column_index, row_index) ? u"true"_s : u"false"_s;
    case PrimitiveType::kInt8:
    case PrimitiveType::kInt16:
    case PrimitiveType::kInt32:
    case PrimitiveType::kInt64:
      return QString::number(chunk.readNumericAsInt64(column_index, row_index));
    case PrimitiveType::kUint8:
    case PrimitiveType::kUint16:
    case PrimitiveType::kUint32:
    case PrimitiveType::kUint64:
      return QString::number(chunk.readNumericAsUint64(column_index, row_index));
    case PrimitiveType::kFloat32:
    case PrimitiveType::kFloat64: {
      // A float column is only discrete while it holds whole numbers
      // (ScalarFieldPayload::whole_numbers_only): label them as integers. A
      // fractional value (the catalog has not caught up yet) reads as null.
      const double value = chunk.readNumericAsDouble(column_index, row_index);
      if (!encoding::isWholeInt64(value)) {
        return std::nullopt;
      }
      return QString::number(static_cast<qlonglong>(value));
    }
    case PrimitiveType::kUnspecified:
      return std::nullopt;
  }
  return std::nullopt;
}

}  // namespace

StateSeriesAdapter::StateSeriesAdapter(SessionManager* session, CurveDescriptor source, PrimitiveType logical_type)
    : session_(session), source_(std::move(source)), logical_type_(logical_type) {}

bool StateSeriesAdapter::rebuild() {
  if (session_ == nullptr) {
    return false;
  }

  auto cursor_or = session_->createReader().rangeQuery(
      QueryRange{
          .topic_id = source_.topic_id,
          .t_min = std::numeric_limits<Timestamp>::min(),
          .t_max = std::numeric_limits<Timestamp>::max(),
      });
  if (!cursor_or.has_value()) {
    return false;  // stale beats blank: keep the previous segments
  }

  // Pass 1 (inline): collapse equal-timestamp samples to the LAST in cursor
  // order, then RLE on value changes. `pending` holds the not-yet-flushed
  // sample; `open` marks a run in progress (closed by a null or a change).
  std::vector<RawStateSegment> segments;
  bool has_pending = false;
  qint64 pending_ts = 0;
  bool pending_null = false;
  QString pending_value;

  const auto flush_pending = [&segments, &has_pending, &pending_ts, &pending_null, &pending_value]() {
    if (!has_pending) {
      return;
    }
    const bool open = !segments.empty() && segments.back().t_end_raw_ns == RawStateSegment::kOpenEnd;
    if (pending_null) {
      if (open) {
        segments.back().t_end_raw_ns = pending_ts;  // null closes the run (gap follows)
      }
    } else if (open && segments.back().value == pending_value) {
      // Same value: the run simply continues.
    } else {
      if (open) {
        segments.back().t_end_raw_ns = pending_ts;
      }
      segments.push_back(
          {.t_start_raw_ns = pending_ts, .t_end_raw_ns = RawStateSegment::kOpenEnd, .value = pending_value});
    }
    has_pending = false;
  };

  cursor_or->forEach([&](const SampleRow& row) {
    const qint64 ts = row.timestamp;
    // A chunk sealed before this column appeared has fewer columns; probing
    // isNull there would read out of bounds, so short-circuit to null first.
    const bool in_chunk = source_.column_index < row.chunk->columns.size();
    bool is_null = !in_chunk || row.chunk->isNull(source_.column_index, row.row_index);
    QString value;
    if (!is_null) {
      std::optional<QString> label = formatStateLabel(*row.chunk, source_.column_index, row.row_index, logical_type_);
      if (label.has_value()) {
        value = std::move(*label);
      } else {
        is_null = true;  // unreadable as the declared type → same as a null sample
      }
    }
    if (has_pending && ts == pending_ts) {
      // Equal timestamp: last write wins; the earlier sample never materializes
      // (zero-duration runs collapse away).
      pending_null = is_null;
      pending_value = std::move(value);
      return;
    }
    flush_pending();
    has_pending = true;
    pending_ts = ts;
    pending_null = is_null;
    pending_value = std::move(value);
  });
  flush_pending();

  segments_ = std::move(segments);
  return true;
}

void StateSeriesAdapter::clear() {
  segments_.clear();
}

}  // namespace PJ
