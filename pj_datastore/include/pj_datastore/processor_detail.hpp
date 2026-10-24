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

/// A passthrough or magnitude preserves the input column type; with no declared
/// input kind (a topic registered before any data), fall back to kFloat64.
inline std::vector<PJ::StorageKind> mirrorInputKind(PJ::Span<const PJ::StorageKind> in) {
  if (in.empty()) {
    return {PJ::StorageKind::kFloat64};
  }
  return {in[0]};
}

/// Coerce a VarValue's numeric arm to double for filter math (double/int64/uint64
/// widen; string is not numeric → 0.0). The shared input-reading path so every
/// builtin treats its channel value identically.
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
