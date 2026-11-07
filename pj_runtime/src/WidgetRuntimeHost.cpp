// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/WidgetRuntimeHost.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "pj_base/time_math.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/saturating_time.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_plugins/sdk/widget_plugin_base.hpp"
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

/// Samples one call may deliver: `max_samples`, 0 meaning the default, never above the hard cap.
uint32_t effectiveCap(const WidgetHistoryQuery& query) {
  return query.max_samples == 0 ? WidgetRuntimeHost::kDefaultMaxSamples
                                : std::min(query.max_samples, WidgetRuntimeHost::kMaxSamplesLimit);
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
    next_target_ = j_ < cap_ ? target(j_) : total_;  // total_ is never a valid index: nothing more is taken
    return true;
  }

 private:
  [[nodiscard]] std::size_t target(std::size_t j) const {
    const std::size_t denominator = std::max<std::size_t>(cap_ - 1, 1);
    return static_cast<std::size_t>(static_cast<unsigned long long>(j) * (total_ - 1) / denominator);
  }

  std::size_t total_;
  std::size_t cap_;
  std::size_t j_ = 0;
  std::size_t next_target_ = 0;
};

}  // namespace

WidgetRuntimeHost::WidgetRuntimeHost(
    const DataReader& reader, const ObjectStore& objects, WireResolver resolver, Callbacks callbacks)
    : reader_(reader), objects_(objects), resolver_(std::move(resolver)) {
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
  switch (wire->target.kind) {
    case WidgetInputKind::kNumber:
      return historyNumber(query, *wire, sink);
    case WidgetInputKind::kText:
      return historyText(query, *wire, sink);
    case WidgetInputKind::kObject:
      return historyObject(query, *wire, sink);
  }
  return unexpected(std::string("unknown input kind"));
}

Expected<uint32_t> WidgetRuntimeHost::historyNumber(
    const WidgetHistoryQuery& query, const ResolvedWire& wire, const WidgetValueSink& sink) const {
  const WireTarget& target = wire.target;
  auto series = reader_.series(target.topic_id, target.column);
  if (!series) {
    return unexpected(series.error());
  }
  const Range<Timestamp> range{toRawSaturating(query.from, target.offset), toRawSaturating(query.to, target.offset)};
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
        query.input, query.slot, PJ_WIDGET_VALUE_NUMBER, rawToDisplaySeconds(sample.timestamp, target.offset).value,
        wire.label);
    value.number = sample.value;
    if (!sink(value)) {
      break;
    }
  }
  return saturatedTotal(bounds->sample_count);
}

Expected<uint32_t> WidgetRuntimeHost::historyText(
    const WidgetHistoryQuery& query, const ResolvedWire& wire, const WidgetValueSink& sink) const {
  const WireTarget& target = wire.target;
  const QueryRange range{
      target.topic_id, toRawSaturating(query.from, target.offset), toRawSaturating(query.to, target.offset)};

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
  auto cursor = reader_.rangeQuery(range);
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
        query.input, query.slot, PJ_WIDGET_VALUE_TEXT, rawToDisplaySeconds(row.timestamp, target.offset).value,
        wire.label);
    value.text = PJ_string_view_t{text.data(), text.size()};
    if (!sink(value)) {
      break;
    }
  }
  return saturatedTotal(rows.size());
}

Expected<uint32_t> WidgetRuntimeHost::historyObject(
    const WidgetHistoryQuery& query, const ResolvedWire& wire, const WidgetValueSink& sink) const {
  const WireTarget& target = wire.target;
  const Timestamp from = toRawSaturating(query.from, target.offset);
  const Timestamp to = toRawSaturating(query.to, target.offset);
  // rangeByTime is (lo, hi]; the history range is inclusive on both ends.
  const Timestamp lo = saturatingSub(from, 1);
  const auto entries = objects_.rangeByTime(target.object_topic, lo, to);
  if (entries.empty()) {
    return 0U;
  }
  Decimator decimator(entries.size(), effectiveCap(query));
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (!decimator.take(i)) {
      continue;
    }
    // Resolved one at a time, outside any store lock: a lazy entry may refetch. An entry evicted
    // since rangeByTime, or whose fetch failed, has no payload to deliver and is skipped.
    const auto entry = objects_.at(target.object_topic, entries[i].uid);
    if (!entry || entry->fetch_failed) {
      continue;
    }
    PJ_widget_value_t value = baseWidgetValue(
        query.input, query.slot, PJ_WIDGET_VALUE_OBJECT, rawToDisplaySeconds(entry->timestamp, target.offset).value,
        wire.label);
    value.bytes = PJ_bytes_view_t{entry->payload.bytes.data(), entry->payload.bytes.size()};
    value.type_name = PJ_string_view_t{target.object_type_name.data(), target.object_type_name.size()};
    if (!sink(value)) {
      break;
    }
  }
  return saturatedTotal(entries.size());
}

}  // namespace PJ
