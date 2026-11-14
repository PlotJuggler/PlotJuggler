// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/WidgetRuntimeHost.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pj_base/time_math.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/saturating_time.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_plugins/sdk/widget_plugin_base.hpp"
#include "pj_runtime/CanonicalObjectPayload.h"
#include "pj_runtime/ServiceRegistration.h"
#include "pj_runtime/WidgetValue.h"

namespace PJ {

namespace {

/// Display seconds -> stored nanoseconds, saturating instead of overflowing: the plugin passes
/// arbitrary finite doubles ("everything" is a legitimate query).
Timestamp toRawSaturating(double seconds, DisplayOffset offset) {
  constexpr Timestamp kMax = std::numeric_limits<Timestamp>::max();
  constexpr Timestamp kMin = std::numeric_limits<Timestamp>::min();
  const std::optional<int64_t> display_ns = secondsToNanoseconds(seconds);
  if (!display_ns) {
    return seconds < 0 ? kMin : kMax;
  }
  return saturatingAdd(*display_ns, offset.value.count());
}

/// The first stored instant whose delivered time (rawToDisplaySecondsAtOrAfter) is `seconds` or later; nullopt
/// when even the last instant is delivered before it. A widget queries back with the times it was handed, so a
/// range is cut where those times change, not at the instant nearest the bound: near 1.7e9 s a double steps
/// ~238 ns, and near 0 many doubles share a nanosecond.
std::optional<Timestamp> firstDeliveredAtOrAfter(double seconds, DisplayOffset offset) {
  constexpr Timestamp kMax = std::numeric_limits<Timestamp>::max();
  constexpr Timestamp kMin = std::numeric_limits<Timestamp>::min();
  const auto delivered = [offset](Timestamp raw) { return rawToDisplaySecondsAtOrAfter(raw, offset).value; };
  const Timestamp nearest = toRawSaturating(seconds, offset);
  if (nearest == kMax && delivered(kMax) < seconds) {
    return std::nullopt;  // saturated: no instant is that late
  }
  if (!(std::abs(seconds) < 9e9)) {
    return nearest;  // past every representable instant: the range is everything or nothing
  }
  // A delivered time is within a few double steps of the nearest one: bisect over that window.
  const double step = std::nextafter(std::abs(seconds), std::numeric_limits<double>::infinity()) - std::abs(seconds);
  const auto window = static_cast<Timestamp>(std::ceil(8.0 * step * 1e9)) + 8;
  if (nearest >= kMax - (2 * window) || nearest <= kMin + (2 * window)) {
    return nearest;
  }
  Timestamp before = nearest - window;  // delivered before `seconds`
  Timestamp at = nearest + window;      // delivered at `seconds` or later
  if (delivered(before) >= seconds || delivered(at) < seconds) {
    return nearest;
  }
  while (at - before > 1) {
    const Timestamp mid = before + ((at - before) / 2);
    (delivered(mid) >= seconds ? at : before) = mid;
  }
  return at;
}

/// The stored instants whose delivered times lie in [from, to], inclusive; empty when min > max.
Range<Timestamp> deliveredRange(const WidgetHistoryQuery& query, DisplayOffset offset) {
  constexpr Range<Timestamp> kEmpty{.min = 1, .max = 0};
  const std::optional<Timestamp> first = firstDeliveredAtOrAfter(query.from, offset);
  if (!first) {
    return kEmpty;
  }
  // The last instant is the one before the first delivered after `to`; none such means every later instant.
  const std::optional<Timestamp> past =
      firstDeliveredAtOrAfter(std::nextafter(query.to, std::numeric_limits<double>::infinity()), offset);
  if (!past) {
    return Range<Timestamp>{.min = *first, .max = std::numeric_limits<Timestamp>::max()};
  }
  if (*past == std::numeric_limits<Timestamp>::min()) {
    return kEmpty;
  }
  return Range<Timestamp>{.min = *first, .max = *past - 1};
}

/// Samples one call may deliver: `max_samples`, 0 meaning the default.
uint32_t effectiveCap(const WidgetHistoryQuery& query) {
  return query.max_samples == 0 ? WidgetRuntimeHost::kDefaultMaxSamples : query.max_samples;
}

/// The range count reported as `out_total`: saturated, the ABI field is 32-bit.
uint32_t saturatedTotal(std::size_t total) {
  return total > std::numeric_limits<uint32_t>::max() ? std::numeric_limits<uint32_t>::max()
                                                      : static_cast<uint32_t>(total);
}

/// Picks at most `cap` of `total` samples, evenly spaced and always including the first and the last,
/// so a decimated history still spans the whole requested range. Feed it indices in ascending order.
class Decimator {
 public:
  Decimator(std::size_t total, std::size_t cap) : total_(total), cap_(std::max<std::size_t>(cap, 1)) {}

  [[nodiscard]] bool take(std::size_t index) {
    if (total_ <= cap_) {
      return true;
    }
    if (index != next_target_) {
      return false;
    }
    ++j_;
    next_target_ = j_ < cap_ ? pick(j_, total_, cap_) : total_;  // total_ is never a valid index: nothing more is taken
    return true;
  }

  /// Index of the `j`-th of `cap` samples spread evenly over `total` (first and last included).
  [[nodiscard]] static std::size_t pick(std::size_t j, std::size_t total, std::size_t cap) {
    const std::size_t denominator = std::max<std::size_t>(cap - 1, 1);
    return static_cast<std::size_t>(static_cast<unsigned long long>(j) * (total - 1) / denominator);
  }

 private:
  std::size_t total_;
  std::size_t cap_;
  std::size_t j_ = 0;
  std::size_t next_target_ = 0;
};

}  // namespace

WidgetRuntimeHost::WidgetRuntimeHost(
    const DataReader& reader, const ObjectStore& objects, WireResolver resolver, Callbacks callbacks,
    ObjectPayloadEncoder encoder)
    : reader_(reader), objects_(objects), resolver_(std::move(resolver)), encoder_(std::move(encoder)) {
  bridge_.history = [this](const WidgetHistoryQuery& query, const WidgetValueSink& sink) {
    return history(query, sink);
  };
  bridge_.inputs_changed = std::move(callbacks.inputs_changed);
  bridge_.report_message = std::move(callbacks.report_message);
}

Status WidgetRuntimeHost::registerServices(ServiceRegistryBuilder& registry) {
  return registerRequiredService<sdk::WidgetHostService>(registry, bridge_.host());
}

Expected<uint32_t> WidgetRuntimeHost::history(const WidgetHistoryQuery& query, const WidgetValueSink& sink) const {
  if (!std::isfinite(query.from) || !std::isfinite(query.to)) {
    return unexpected(std::string("history range must be finite"));
  }
  const std::optional<ResolvedWire> wire = resolver_ ? resolver_(query.input, query.slot) : std::nullopt;
  if (!wire) {
    return unexpected("input " + std::to_string(query.input) + " slot " + std::to_string(query.slot) + " is not wired");
  }
  if (query.from > query.to) {
    return 0U;
  }
  const Range<Timestamp> range = deliveredRange(query, wire->target.offset);
  if (range.min > range.max) {
    return 0U;
  }
  switch (wire->target.kind) {
    case WidgetInputKind::kNumber:
      return historyNumber(query, *wire, range, sink);
    case WidgetInputKind::kText:
      return historyText(query, *wire, range, sink);
    case WidgetInputKind::kObject:
      return historyObject(query, *wire, range, sink);
  }
  return unexpected(std::string("unknown input kind"));
}

Expected<uint32_t> WidgetRuntimeHost::historyNumber(
    const WidgetHistoryQuery& query, const ResolvedWire& wire, const Range<Timestamp>& range,
    const WidgetValueSink& sink) const {
  const WireTarget& target = wire.target;
  auto series = reader_.series(target.topic_id, target.column);
  if (!series) {
    return unexpected(series.error());
  }
  const auto bounds = series->bounds(range);
  if (!bounds || bounds->sample_count == 0) {
    return 0U;
  }
  Decimator decimator(bounds->sample_count, effectiveCap(query));
  std::size_t index = 0;
  // The reader holds the engine lock while the cursor runs, so the sink executes under it: a widget
  // copies the sample and returns, it must not call back into the host from there.
  for (auto cursor = series->samples(range); cursor.valid(); cursor.advance()) {
    if (!decimator.take(index++)) {
      continue;
    }
    const SeriesSample sample = cursor.current();
    PJ_widget_value_t value = baseWidgetValue(
        query.input, query.slot, PJ_WIDGET_VALUE_NUMBER,
        rawToDisplaySecondsAtOrAfter(sample.timestamp, target.offset).value, wire.label);
    value.number = sample.value;
    if (!sink(value)) {
      break;
    }
  }
  return saturatedTotal(bounds->sample_count);
}

Expected<uint32_t> WidgetRuntimeHost::historyText(
    const WidgetHistoryQuery& query, const ResolvedWire& wire, const Range<Timestamp>& range,
    const WidgetValueSink& sink) const {
  const WireTarget& target = wire.target;
  const QueryRange rows_range{.topic_id = target.topic_id, .t_min = range.min, .t_max = range.max};

  // A row is a sample only when its cell in this column is a non-null string; the chunks before a
  // mid-stream column addition may not have the column at all.
  const auto is_sample = [&](const SampleRow& row) {
    if (target.column >= row.chunk->columns.size()) {
      return false;
    }
    const auto& descriptor = row.chunk->columns[target.column].descriptor;
    return descriptor != nullptr && descriptor->logical_type == PrimitiveType::kString &&
           !row.chunk->isNull(target.column, row.row_index);
  };

  // One cursor, so the merge over the chunks runs once: the first pass keeps the sample rows (no
  // string is read), the decimation needs their count before it picks. The cursor holds the engine
  // lock until it is destroyed, so the rows stay valid and the sink executes under that lock (see
  // historyNumber).
  auto cursor = reader_.rangeQuery(rows_range);
  if (!cursor) {
    return unexpected(cursor.error());
  }
  std::vector<SampleRow> rows;
  cursor->forEach([&](const SampleRow& row) {
    if (is_sample(row)) {
      rows.push_back(row);
    }
  });
  Decimator decimator(rows.size(), effectiveCap(query));
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (!decimator.take(i)) {
      continue;
    }
    const SampleRow& row = rows[i];
    const std::string_view text = row.chunk->readString(target.column, row.row_index);
    PJ_widget_value_t value = baseWidgetValue(
        query.input, query.slot, PJ_WIDGET_VALUE_TEXT, rawToDisplaySecondsAtOrAfter(row.timestamp, target.offset).value,
        wire.label);
    value.text = PJ_string_view_t{text.data(), text.size()};
    if (!sink(value)) {
      break;
    }
  }
  return saturatedTotal(rows.size());
}

Expected<uint32_t> WidgetRuntimeHost::historyObject(
    const WidgetHistoryQuery& query, const ResolvedWire& wire, const Range<Timestamp>& range,
    const WidgetValueSink& sink) const {
  const WireTarget& target = wire.target;
  const Timestamp to = range.max;
  // rangeByTime is (lo, hi]; the history range is inclusive on both ends.
  const Timestamp lo = saturatingSub(range.min, 1);

  // Count and choose the delivered entries by binary search over the sorted timestamps, so a call that
  // delivers few samples (a widget probing a range with max_samples 1) costs O(log n), not a copy of the
  // whole window. A chosen entry is named by its timestamp and its rank among the entries sharing it.
  std::size_t total = 0;
  std::vector<std::pair<Timestamp, std::size_t>> chosen;
  {
    const EntryTimestampsView stamps = objects_.entryTimestamps(target.object_topic);
    const Timestamp* first = std::upper_bound(stamps.begin(), stamps.end(), lo);
    const Timestamp* last = std::upper_bound(first, stamps.end(), to);
    total = static_cast<std::size_t>(last - first);
    if (total == 0) {
      return 0U;
    }
    const std::size_t cap = std::max<std::size_t>(effectiveCap(query), 1);
    if (total > cap) {
      chosen.reserve(cap);
      for (std::size_t j = 0; j < cap; ++j) {
        const std::size_t index = Decimator::pick(j, total, cap);
        const Timestamp stamp = first[index];
        const auto rank = static_cast<std::size_t>(first + index - std::lower_bound(first, first + index, stamp));
        chosen.emplace_back(stamp, rank);
      }
    }
  }

  // Resolved one at a time, outside any store lock: a lazy entry may refetch. An entry evicted since it was
  // counted, or whose fetch failed or did not encode, has no payload to deliver and is skipped.
  // Returns false when the sink stops the delivery; `delivered` says whether the entry reached the sink.
  const auto deliver = [&](SequentialUID uid, bool& delivered) {
    const auto entry = resolveEncodedObject(objects_, target, uid, encoder_);
    delivered = entry.has_value();
    if (!entry) {
      return true;
    }
    PJ_widget_value_t value = baseWidgetValue(
        query.input, query.slot, PJ_WIDGET_VALUE_OBJECT,
        rawToDisplaySecondsAtOrAfter(entry->timestamp, target.offset).value, wire.label);
    setObjectPayload(value, entry->payload, target.object_type);
    return sink(value);
  };

  if (chosen.empty()) {
    // A streaming push between the count and this read can grow the window: never deliver more than the
    // `total` (<= max_samples) that was counted. A skipped entry leaves the count, so a widget comparing
    // the total with what it received sees a complete history.
    const auto entries = objects_.rangeByTime(target.object_topic, lo, to);
    const std::size_t count = std::min(entries.size(), total);
    std::size_t skipped = 0;
    for (std::size_t i = 0; i < count; ++i) {
      bool delivered = false;
      if (!deliver(entries[i].uid, delivered)) {
        break;
      }
      skipped += delivered ? 0 : 1;
    }
    return saturatedTotal(total - skipped);
  }
  // Decimated: the total stays the range's, a skipped pick is not replaced. The picks ascend, so those
  // sharing a stamp are adjacent: its entries are read once, not once per pick (all entries on one stamp
  // would otherwise cost cap x total).
  std::optional<Timestamp> read_stamp;
  std::vector<OrderedEntries::TimeRangeEntry> at_stamp;
  for (const auto& [stamp, rank] : chosen) {
    if (read_stamp != stamp) {
      at_stamp = objects_.rangeByTime(target.object_topic, saturatingSub(stamp, 1), stamp);
      read_stamp = stamp;
    }
    bool delivered = false;
    if (rank < at_stamp.size() && !deliver(at_stamp[rank].uid, delivered)) {
      break;
    }
  }
  return saturatedTotal(total);
}

}  // namespace PJ
