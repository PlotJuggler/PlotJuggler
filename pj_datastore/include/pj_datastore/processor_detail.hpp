#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <type_traits>
#include <variant>
#include <vector>

#include "pj_base/span.hpp"
#include "pj_datastore/column_buffer.hpp"       // PJ::StorageKind
#include "pj_datastore/incremental_engine.hpp"  // PJ::VarValue

namespace PJ::proc::detail {

/// Output kind of a processor that preserves its input kind; kFloat64 when no input kind is declared (a topic
/// registered before any data).
inline std::vector<PJ::StorageKind> mirrorInputKind(PJ::Span<const PJ::StorageKind> in) {
  if (in.empty()) {
    return {PJ::StorageKind::kFloat64};
  }
  return {in[0]};
}

/// int64 and uint64 widen to double; a string is 0.0.
inline double toDouble(const PJ::VarValue& v) {
  return std::visit(
      [](auto&& arg) -> double {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::string>) {
          return 0.0;
        } else {
          return static_cast<double>(arg);
        }
      },
      v);
}

}  // namespace PJ::proc::detail
