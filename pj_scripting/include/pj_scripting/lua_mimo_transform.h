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

/// A self-describing script filter installed as a multi-input / multi-output
/// `PJ::IMIMOTransform` node in the `IncrementalEngine`. The engine joins the N inputs
/// and calls `calculate()` once per joined sample; this wraps a Luau
/// `FilterInstance` whose `calculate(self, t, v, v1..vN-1)` returns M values.
///
/// It mirrors `LuaSisoTransform` for the N→M case (same time contract, sticky
/// failure, and output-kind coercion). The SISO transform stays the 1→1 path;
/// this is the general one (SISO is N=M=1 through here too, but the engine keeps
/// the dedicated SISO node for single-input filters).
///
/// TIME CONTRACT (critical, identical to LuaSisoTransform): the script sees
/// `t` = SECONDS SINCE SESSION START. The session start (int64 ns) is captured
/// from the first sample and subtracted on the int64 spine BEFORE the double cast.
///
/// JOIN SEMANTICS are the engine node's concern, NOT this class: `inputs` arrive
/// already aligned to one timestamp. Whether that alignment is an exact-timestamp
/// join (`addMimoTransform`) or a nearest-sample-to-primary join is decided by
/// which engine node installs this transform; the script and this wrapper are
/// agnostic to it.
class LuaMimoTransform : public PJ::IMIMOTransform {
 public:
  /// Builds and primes a live instance from `klass` + `params_json`. `num_outputs`
  /// is the count of declared output topics (M); `calculate` must return exactly
  /// that many values or the node sticky-fails and suppresses.
  LuaMimoTransform(
      std::shared_ptr<ScriptEngine> engine, FilterClass klass, std::string params_json, std::size_t num_outputs);

  void reset() override;
  [[nodiscard]] std::vector<PJ::StorageKind> outputKinds(PJ::Span<const PJ::StorageKind> input_kinds) const override;
  [[nodiscard]] bool calculate(
      PJ::Timestamp time, PJ::Span<const PJ::VarValue> inputs, PJ::Timestamp& out_time,
      std::vector<PJ::VarValue>& output) override;

  /// Sticky failed state (script error / no instance). Part of the
  /// IMIMOTransform contract: the engine distinguishes a sticky failure (staged
  /// batch invalidated) from ordinary row suppression through this override.
  [[nodiscard]] bool failed() const override {
    return !instance_ || instance_->failed();
  }
  [[nodiscard]] const std::string& error() const override {
    return error_;
  }
  /// The module source this filter was parsed from (for layout `<source_fallback>`).
  [[nodiscard]] const std::string& sourceText() const noexcept {
    return klass_.source;
  }

 private:
  void rebuildInstance();  ///< create a fresh instance from klass_ + params_json_

  /// How a script's double output maps to a stored column kind (one mode for all
  /// M outputs — the class schema carries a single `output` kind).
  enum class OutputMode { kDouble, kInt64, kMirror };
  [[nodiscard]] PJ::VarValue makeOutputValue(double computed, const PJ::VarValue& mirror_input) const;

  std::shared_ptr<ScriptEngine> engine_;
  FilterClass klass_;
  std::string params_json_;
  std::size_t num_outputs_;
  OutputMode out_mode_ = OutputMode::kDouble;
  std::unique_ptr<FilterInstance> instance_;
  std::string error_;

  bool has_session_start_ = false;
  PJ::Timestamp session_start_ns_ = 0;
};

}  // namespace PJ::scripting
