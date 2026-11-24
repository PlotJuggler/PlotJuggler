// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "pj_datastore/incremental_engine.hpp"  // PJ::IMIMOTransform, VarValue, StorageKind, Timestamp, Span
#include "pj_scripting/filter_class.h"
#include "pj_scripting/script_engine.h"

namespace PJ::scripting {

/// A script filter as a multi-input / multi-output `PJ::IMIMOTransform` node of the `IncrementalEngine`: the engine
/// joins the N inputs and calls it once per joined sample, and the script's `calculate(self, t, v, v1..vN-1)` returns
/// M values. Time contract, sticky failure and output-kind coercion are those of `LuaSisoTransform`.
/// A one-input, one-output filter takes the dedicated SISO node (`LuaSisoTransform`) instead.
///
/// The join (exact-timestamp or nearest-sample-to-primary) is decided by the engine node that installs it; `inputs`
/// arrive already aligned to one timestamp.
class LuaMimoTransform : public PJ::IMIMOTransform {
 public:
  /// Builds a live instance from `klass` + `params_json`. `calculate` must return exactly `num_outputs` values (the
  /// declared output topics); any other count emits no row and sets `error()`.
  LuaMimoTransform(
      std::shared_ptr<ScriptEngine> engine, FilterClass klass, std::string params_json, std::size_t num_outputs);

  void reset() override;
  [[nodiscard]] std::vector<PJ::StorageKind> outputKinds(PJ::Span<const PJ::StorageKind> input_kinds) const override;
  [[nodiscard]] bool calculate(
      PJ::Timestamp time, PJ::Span<const PJ::VarValue> inputs, PJ::Timestamp& out_time,
      std::vector<PJ::VarValue>& output) override;

  /// Sticky failure (script error or no instance): the engine tells it from a suppressed row through this and
  /// invalidates the staged batch.
  [[nodiscard]] bool failed() const override {
    return !instance_ || instance_->failed();
  }
  [[nodiscard]] const std::string& error() const override {
    return error_;
  }
  /// The module source, embedded in a layout as `<source_fallback>`.
  [[nodiscard]] const std::string& sourceText() const noexcept {
    return klass_.source;
  }

 private:
  void rebuildInstance();  ///< create a fresh instance from klass_ + params_json_

  /// How the outputs map to a column kind: one mode for all M outputs, the class's single `output`.
  enum class OutputMode { kDouble, kInt64, kMirror, kString };
  [[nodiscard]] PJ::VarValue makeOutputValue(double computed, const PJ::VarValue& mirror_input) const;

  std::shared_ptr<ScriptEngine> engine_;
  FilterClass klass_;
  std::string params_json_;
  std::size_t num_outputs_;
  OutputMode out_mode_ = OutputMode::kDouble;
  std::unique_ptr<FilterInstance> instance_;
  std::string error_;
  /// calculate()'s inputs as the script sees them, reused across samples; its text views point into that call's
  /// inputs only.
  std::vector<FilterInstance::InputValue> script_inputs_;

  bool has_session_start_ = false;
  PJ::Timestamp session_start_ns_ = 0;
};

}  // namespace PJ::scripting
