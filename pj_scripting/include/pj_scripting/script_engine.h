// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_base/span.hpp"
#include "pj_scripting/filter_class.h"
#include "pj_scripting/sandbox.h"

namespace PJ::scripting {

/// A live, stateful filter instance: one script VM per instance, so two filters never alias. Created by
/// `ScriptEngine::createInstance`. Not thread-safe; called on the GUI thread inside the engine's budget-armed
/// protected call.
class FilterInstance {
 public:
  /// One per-sample result: `suppress` emits nothing (e.g. a derivative's first sample); otherwise `value` is emitted
  /// at the input time, or at `out_time` when the script returned an explicit timestamp.
  struct Result {
    bool suppress = false;
    double value = 0.0;
    std::optional<double> out_time;  ///< present iff the script returned (t, v)
  };

  virtual ~FilterInstance() = default;

  /// Per-sample step. `t` is seconds since the first sample this instance's node processed (the host subtracts that
  /// origin on the int64 spine before the double cast), `v` the sample value. Throws nothing: a script error is
  /// reported through `failed()` / `error()`.
  [[nodiscard]] virtual Result calculate(double t, double v) = 0;

  /// One MIMO step result: `suppress`, or `values`, one per declared output topic in order. There is no explicit
  /// out_time form: the outputs share the joined input timestamp.
  struct MimoResult {
    bool suppress = false;
    std::vector<double> values;
  };

  /// MIMO step (N inputs -> M outputs). `inputs[0]` is the primary value, the rest the secondary inputs the engine
  /// joined to this sample; `t` as in `calculate`. A class implements either this or the SISO `calculate`; the default
  /// suppresses.
  [[nodiscard]] virtual MimoResult calculateMimo(double /*t*/, PJ::Span<const double> /*inputs*/) {
    return MimoResult{true, {}};
  }

  /// Clears per-instance state (optional in the script). The host rebuilds the instance instead of calling it.
  virtual void reset() = 0;

  /// True once a script error put the instance into a sticky failed state; `calculate` suppresses from then on.
  [[nodiscard]] virtual bool failed() const = 0;
  [[nodiscard]] virtual const std::string& error() const = 0;
};

/// Language seam of the filters: Luau (`makeLuauEngine`) and embedded Python implement it. One engine is shared by many
/// catalogues and instances and holds no per-instance state.
class ScriptEngine {
 public:
  virtual ~ScriptEngine() = default;

  /// Every filter class a module declares (one class table, or a list of them), read without running
  /// `create()` / `calculate()`. `origin` labels provenance. Errors on a compile or shape failure.
  [[nodiscard]] virtual Expected<std::vector<FilterClass>> inspectModule(
      const std::string& source, const std::string& origin) = 0;

  /// A live instance of `klass`: compiles its `source`, finds the class by `id` and calls `create(params)` (flat JSON
  /// keyed by parameter name) in a VM of its own.
  [[nodiscard]] virtual Expected<std::unique_ptr<FilterInstance>> createInstance(
      const FilterClass& klass, const std::string& params_json) = 0;
};

/// The Luau backend. Every VM is sandboxed: a frozen stdlib, a memory cap and an instruction-budget watchdog
/// (`BudgetLimits`).
[[nodiscard]] std::shared_ptr<ScriptEngine> makeLuauEngine(BudgetLimits limits = {});

}  // namespace PJ::scripting
