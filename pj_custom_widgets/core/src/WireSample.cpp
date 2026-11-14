// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_custom_widgets/WireSample.h"

#include <string_view>
#include <utility>

#include "pj_datastore/query.hpp"
#include "pj_runtime/CanonicalObjectPayload.h"
#include "pj_runtime/WidgetValue.h"
#include "pj_widgets/Fnv1a.h"

namespace PJ {

namespace {

WireSample readNumber(const DataReader& reader, const WireTarget& target, Timestamp at) {
  WireSample sample;
  auto series = reader.series(target.topic_id, target.column);
  if (!series) {
    return sample;
  }
  const auto found = series->sampleAtOrBeforeTime(at);
  if (!found) {
    return sample;
  }
  sample.kind = PJ_WIDGET_VALUE_NUMBER;
  sample.stamp = found->timestamp;
  sample.number = found->value;
  return sample;
}

/// The stamp is read first because latestStringAt copies the string: a row `sample` already holds
/// costs no copy on a tick that changed nothing.
void refreshText(const DataReader& reader, const WireTarget& target, Timestamp at, WireSample& sample) {
  const QueryPoint point{target.topic_id, at};
  auto row = reader.latestAt(point);
  if (!row || !row->has_value()) {
    sample = WireSample{};
    return;
  }
  const Timestamp stamp = (*row)->timestamp;
  if (sample.kind == PJ_WIDGET_VALUE_TEXT && sample.stamp == stamp) {
    return;
  }
  auto text = reader.latestStringAt(point, target.column);
  if (!text || !text->has_value() || !(*text)->value.has_value()) {
    sample = WireSample{};
    return;
  }
  sample = WireSample{};
  sample.kind = PJ_WIDGET_VALUE_TEXT;
  sample.stamp = stamp;
  sample.text = std::move(*(*text)->value);
}

/// An entry `sample` already holds keeps its payload, so a push of the same entry fetches nothing.
void refreshObject(const ObjectStore& objects, const WireTarget& target, Timestamp at, WireSample& sample) {
  const auto entry = objects.latestEntryIdAt(target.object_topic, at);
  if (!entry) {
    sample = WireSample{};
    return;
  }
  if (sample.kind == PJ_WIDGET_VALUE_OBJECT && sample.uid.value == entry->uid.value) {
    return;
  }
  sample = WireSample{};
  sample.kind = PJ_WIDGET_VALUE_OBJECT;
  sample.stamp = entry->timestamp;
  sample.uid = entry->uid;
}

}  // namespace

WireSample readWireSample(
    const DataReader& reader, const ObjectStore& objects, const WireTarget& target, Timepoint at) {
  WireSample sample;
  refreshWireSample(reader, objects, target, at, sample);
  return sample;
}

void refreshWireSample(
    const DataReader& reader, const ObjectStore& objects, const WireTarget& target, Timepoint at, WireSample& sample) {
  const Timestamp raw = toRaw(at);
  switch (target.kind) {
    case WidgetInputKind::kNumber:
      sample = readNumber(reader, target, raw);
      return;
    case WidgetInputKind::kText:
      refreshText(reader, target, raw, sample);
      return;
    case WidgetInputKind::kObject:
      refreshObject(objects, target, raw, sample);
      return;
  }
  sample = WireSample{};
}

void fetchObjectPayload(
    const ObjectStore& objects, const WireTarget& target, const ObjectPayloadEncoder& encoder, WireSample& sample) {
  if (sample.kind != PJ_WIDGET_VALUE_OBJECT || sample.object) {
    return;
  }
  auto entry = resolveEncodedObject(objects, target, sample.uid, encoder);
  if (!entry) {
    sample = WireSample{};
    return;
  }
  sample.object = std::move(entry);
}

std::uint64_t mixWireSample(std::uint64_t hash, const WireSample& sample, DisplayOffset offset) noexcept {
  hash = fnv1aMix(hash, static_cast<std::uint64_t>(sample.kind));
  if (sample.kind == PJ_WIDGET_VALUE_NONE) {
    return hash;
  }
  hash = fnv1aMix(hash, static_cast<std::uint64_t>(sample.stamp));
  hash = fnv1aMix(hash, static_cast<std::uint64_t>(offset.value.count()));
  switch (sample.kind) {
    case PJ_WIDGET_VALUE_NUMBER:
      return fnv1aMixBits(hash, sample.number);
    case PJ_WIDGET_VALUE_OBJECT:
      return fnv1aMix(hash, sample.uid.value);
    default:
      return hash;
  }
}

PJ_widget_value_t makeWidgetValue(
    std::uint32_t input, std::uint32_t slot, const WireTarget& target, const WireSample& sample,
    std::string_view label) {
  const double time =
      sample.kind == PJ_WIDGET_VALUE_NONE ? 0.0 : rawToDisplaySecondsAtOrAfter(sample.stamp, target.offset).value;
  PJ_widget_value_t value = baseWidgetValue(input, slot, sample.kind, time, label);
  switch (sample.kind) {
    case PJ_WIDGET_VALUE_NUMBER:
      value.number = sample.number;
      break;
    case PJ_WIDGET_VALUE_TEXT:
      value.text = PJ_string_view_t{sample.text.data(), sample.text.size()};
      break;
    case PJ_WIDGET_VALUE_OBJECT:
      if (sample.object) {
        setObjectPayload(value, sample.object->payload, target.object_type);
      }
      break;
    default:
      break;
  }
  return value;
}

}  // namespace PJ
