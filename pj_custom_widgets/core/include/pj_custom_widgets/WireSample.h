#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "pj_base/types.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_plugins/widget_protocol.h"
#include "pj_runtime/Time.h"
#include "pj_runtime/WireTarget.h"

namespace PJ {

/// The latest sample of one wire at or before an instant, in the stored (absolute) time frame.
///
/// Scalar kinds carry their value. An object sample carries only its identity until
/// `fetchObjectPayload` resolves the bytes, so a tick that changed nothing never pays for a lazy
/// fetch.
struct WireSample {
  PJ_widget_value_kind_t kind = PJ_WIDGET_VALUE_NONE;
  Timestamp stamp = 0;  ///< stored timestamp of the sample
  double number = 0.0;
  std::string text;
  SequentialUID uid;                          ///< object samples: the entry that was found
  std::optional<ResolvedObjectEntry> object;  ///< object samples after fetchObjectPayload
};

/// Reads the latest sample of `target` at or before the absolute instant `at`. A wire with no
/// sample, a failed read or an unknown topic yields kind NONE.
[[nodiscard]] WireSample readWireSample(
    const DataReader& reader, const ObjectStore& objects, const WireTarget& target, Timepoint at);

/// readWireSample into `sample`, which holds the previous read of the SAME target. A text sample
/// whose row stamp did not change keeps its text: the string is read only when the row moved.
void refreshWireSample(
    const DataReader& reader, const ObjectStore& objects, const WireTarget& target, Timepoint at, WireSample& sample);

/// Resolves the payload of an object sample. An entry that was evicted since `readWireSample`, or
/// whose lazy fetch failed, turns the sample into NONE. No-op for other kinds.
void fetchObjectPayload(const ObjectStore& objects, const WireTarget& target, WireSample& sample);

/// Folds the sample (kind, stamp, the number or the object entry uid) and the display offset into
/// `hash`. A text sample folds its stamp only: its row stamp names the string. Two ticks that fold to
/// the same value would hand the widget the same data, so the second is skipped.
[[nodiscard]] std::uint64_t mixWireSample(std::uint64_t hash, const WireSample& sample, DisplayOffset offset) noexcept;

/// Builds the ABI value of a sample. Every pointer in it refers to `sample`, `target` and `label`, so
/// they must outlive the call that delivers it. `label` is set for every kind, NONE included.
[[nodiscard]] PJ_widget_value_t makeWidgetValue(
    std::uint32_t input, std::uint32_t slot, const WireTarget& target, const WireSample& sample,
    std::string_view label);

}  // namespace PJ
