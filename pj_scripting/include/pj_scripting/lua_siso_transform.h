// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pj_datastore/data_processor.hpp"
#include "pj_scripting/filter_class.h"
#include "pj_scripting/script_engine.h"

namespace PJ::scripting {

/// A self-describing script filter installed as a host-internal
/// `PJ::proc::DataProcessor`, so the existing `ProcessorSisoAdapter` wraps it as a
/// `IncrementalEngine` node with ZERO new streaming plumbing — and the eager
/// integral-eviction gate re-proves through it unchanged.
///
/// TIME CONTRACT (critical): the script sees `t` = SECONDS SINCE SESSION START,
/// not absolute epoch. The session start (int64 ns) is captured from the first
/// sample and subtracted on the int64 spine BEFORE the double cast — absolute
/// epoch ns in a double quantizes to ~256 ns and would corrupt every dt-based
/// filter via catastrophic cancellation. Output timestamps are reconstructed
/// back onto the absolute int64 spine.
///
/// `reset()` = construct-new-and-swap (rebuild the instance via the engine), the
/// engine's pre-batch-recompute reset; a script runtime error fails the node
/// (sticky `failed_`), never silently corrupts a stateful accumulator.
class LuaSisoTransform : public proc::DataProcessor {
 public:
  /// Builds and primes a live instance from `klass` + `params_json`. If the
  /// engine cannot create the instance the node starts failed (suppresses).
  LuaSisoTransform(std::shared_ptr<ScriptEngine> engine, FilterClass klass, std::string params_json);

  // ---- identity / shape ----
  [[nodiscard]] const char* id() const override;
  [[nodiscard]] const char* bracketLabel() const override;
  [[nodiscard]] proc::TraitMask traits() const override;
  [[nodiscard]] bool isStreamSafe() const override;

  // ---- lifecycle ----
  void reset() override;

  // ---- processing ----
  [[nodiscard]] std::optional<proc::Sample> calculateNextPoint(const proc::Sample& in) override;

  // ---- typed output ----
  [[nodiscard]] std::vector<PJ::StorageKind> outputKinds(PJ::Span<const PJ::StorageKind> in) const override;

  // ---- params (recipe seam) ----
  [[nodiscard]] std::string saveParams() const override;
  void loadParams(const std::string& json) override;

  /// True if the node is in a sticky failed state (script error / no instance).
  [[nodiscard]] bool failed() const override;
  [[nodiscard]] const std::string& error() const override;

  /// The full module source this filter was parsed from — what a layout embeds as
  /// `<source_fallback>` so it can restore on a machine without the filter
  /// installed. Authoritative regardless of whether the class came from the
  /// catalogue or an embedded source, so re-saving a restored filter keeps it.
  [[nodiscard]] const std::string& sourceText() const noexcept {
    return klass_.source;
  }

 private:
  void rebuildInstance();  ///< create a fresh instance from klass_ + params_json_

  /// How the script's double output maps to a stored column kind (from the
  /// class's `output` field): plain double, coerced int64, or mirror-the-input.
  enum class OutputMode { kDouble, kInt64, kMirror };
  OutputMode out_mode_ = OutputMode::kDouble;

  /// Wrap the script's double result in the right VarValue per `out_mode_`
  /// (int64 coercion is finite-guarded + clamped; mirror copies the input kind).
  [[nodiscard]] PJ::VarValue makeOutputValue(double computed, const PJ::VarValue& input) const;

  std::shared_ptr<ScriptEngine> engine_;
  FilterClass klass_;
  std::string params_json_;
  std::unique_ptr<FilterInstance> instance_;
  std::string error_;
  std::string bracket_label_;  ///< cached so bracketLabel() can return const char*

  bool has_session_start_ = false;
  PJ::Timestamp session_start_ns_ = 0;
};

}  // namespace PJ::scripting
