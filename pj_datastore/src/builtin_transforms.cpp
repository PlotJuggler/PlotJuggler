// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/builtin_transforms.hpp"

namespace PJ {

void DerivativeTransform::reset() {
  has_prev_ = false;
  prev_value_ = 0.0;
  prev_time_ = 0;
}

StorageKind DerivativeTransform::outputKind(StorageKind /*input_kind*/) const {
  return StorageKind::kFloat64;
}

bool DerivativeTransform::calculate(
    PJ::Timestamp time, const VarValue& input, PJ::Timestamp& out_time, VarValue& out_value) {
  double v = std::visit(
      [](const auto& val) -> double {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, std::string>) {
          return 0.0;
        } else {
          return static_cast<double>(val);
        }
      },
      input);

  if (!has_prev_) {
    prev_time_ = time;
    prev_value_ = v;
    has_prev_ = true;
    return false;
  }

  double dt = static_cast<double>(time - prev_time_) * 1e-9;  // ns → seconds
  out_time = time;
  out_value = (dt > 0.0) ? (v - prev_value_) / dt : 0.0;

  prev_time_ = time;
  prev_value_ = v;
  return true;
}

}  // namespace PJ
