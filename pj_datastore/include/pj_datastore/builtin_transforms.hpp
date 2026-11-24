#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Built-in ISISOTransform implementations; the calculate()/reset() contract is in incremental_engine.hpp.

#include <string>

#include "pj_base/types.hpp"
#include "pj_datastore/incremental_engine.hpp"

namespace PJ {

/// d(value)/dt in units per second, as float64. The first row is suppressed (no previous sample); a timestamp that
/// does not advance gives 0. A text input fails the transform.
class DerivativeTransform : public ISISOTransform {
  PJ::Timestamp prev_time_ = 0;
  double prev_value_ = 0.0;
  bool has_prev_ = false;
  std::string error_;

 public:
  void reset() override;

  [[nodiscard]] StorageKind outputKind(StorageKind input_kind) const override;

  bool calculate(PJ::Timestamp time, const VarValue& input, PJ::Timestamp& out_time, VarValue& out_value) override;

  [[nodiscard]] bool failed() const override {
    return !error_.empty();
  }
  [[nodiscard]] const std::string& error() const override {
    return error_;
  }
};

}  // namespace PJ
