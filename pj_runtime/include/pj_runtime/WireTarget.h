#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <string_view>

#include "pj_base/types.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_plugins/widget_types.hpp"
#include "pj_runtime/Time.h"

namespace PJ {

/// Where one wire of a Custom Widget panel (input + slot) reads from. Holds no string it owns, so
/// copying one per tick allocates nothing.
struct WireTarget {
  WidgetInputKind kind = WidgetInputKind::kNumber;
  /// kNumber / kText: the scalar topic and the column of the series.
  TopicId topic_id = 0;
  std::size_t column = 0;
  /// kObject: the object topic.
  ObjectTopicId object_topic{};
  /// kObject: the type name handed to the plugin to pick its codec. Static storage (`sdk::name`).
  std::string_view object_type_name;
  /// The dataset's display offset, to move between stored absolute time and the display axis.
  DisplayOffset offset{};
};

}  // namespace PJ
