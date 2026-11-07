#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <string_view>

#include "pj_plugins/widget_protocol.h"

namespace PJ {

/// The fields every `PJ_widget_value_t` the host hands a widget carries; the caller fills the payload
/// of `kind`. `label` is referenced, not copied: it must outlive the delivery.
[[nodiscard]] inline PJ_widget_value_t baseWidgetValue(
    std::uint32_t input, std::uint32_t slot, PJ_widget_value_kind_t kind, double time, std::string_view label) {
  PJ_widget_value_t value{};
  value.input = input;
  value.slot = slot;
  value.kind = kind;
  value.time = time;
  value.label = PJ_string_view_t{label.data(), label.size()};
  return value;
}

}  // namespace PJ
