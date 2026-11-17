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

/// A script filter as a `PJ::proc::DataProcessor`, which `ProcessorSisoAdapter` wraps as an `IncrementalEngine` node.
///
/// Time: the script sees `t` in seconds since the first sample the node processed, not absolute time. The origin is
/// subtracted on the int64 spine before the double cast: an epoch-magnitude double quantizes to about 256 ns and every
/// dt-based filter would see noise. Output timestamps are rebuilt on the absolute spine.
///
/// `reset()` rebuilds the instance (a fresh VM is the only guaranteed-clean reset) and the time origin is captured
/// again from the next sample. A script runtime error fails the node for good (`failed()`); it never corrupts a
/// stateful accumulator silently.
class LuaSisoTransform : public proc::DataProcessor {
 public:
  /// Builds a live instance from `klass` + `params_json`; if the engine cannot, the node starts failed (suppresses).
  LuaSisoTransform(std::shared_ptr<ScriptEngine> engine, FilterClass klass, std::string params_json);

  [[nodiscard]] const char* id() const override;
  [[nodiscard]] const char* bracketLabel() const override;
  [[nodiscard]] proc::TraitMask traits() const override;
  [[nodiscard]] bool isStreamSafe() const override;

  void reset() override;

  [[nodiscard]] std::optional<proc::Sample> calculateNextPoint(const proc::Sample& in) override;

  [[nodiscard]] std::vector<PJ::StorageKind> outputKinds(PJ::Span<const PJ::StorageKind> in) const override;

  [[nodiscard]] std::string saveParams() const override;
  void loadParams(const std::string& json) override;

  [[nodiscard]] bool failed() const override;
  [[nodiscard]] const std::string& error() const override;

  /// The module source this filter was parsed from: what a layout embeds as `<source_fallback>` to restore on a machine
  /// without the filter, whether the class came from the catalogue or an embedded source.
  [[nodiscard]] const std::string& sourceText() const noexcept {
    return klass_.source;
  }

 private:
  void rebuildInstance();  ///< create a fresh instance from klass_ + params_json_

  /// How the script's double output maps to a column kind (the class's `output`): double, int64, or the input's kind.
  enum class OutputMode { kDouble, kInt64, kMirror };
  OutputMode out_mode_ = OutputMode::kDouble;

  /// Integer outputs clamp to the type's range and a non-finite value becomes 0; mirror keeps the input's kind.
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
