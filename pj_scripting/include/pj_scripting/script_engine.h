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

/// A live, stateful filter instance — one script VM per instance (the engine
/// keeps no cross-instance state, so two filters never alias). Created from a
/// `FilterClass` via `ScriptEngine::createInstance`. NOT thread-safe; called on
/// the commit thread inside the engine's budget-armed protected call (M2).
class FilterInstance {
 public:
  /// One per-sample result. `suppress` == true means "emit nothing for this
  /// input" (e.g. the first sample of a derivative). When emitting, `value` is
  /// the output; `out_time` is set only when the script returned an explicit
  /// timestamp (two numbers) — otherwise the host emits at the input time.
  struct Result {
    bool suppress = false;
    double value = 0.0;
    std::optional<double> out_time;  ///< present iff the script returned (t, v)
  };

  virtual ~FilterInstance() = default;

  /// Per-sample step. `t` = seconds since session start, `v` = the sample value
  /// (both doubles — host rebases time off the int64 spine before the cast).
  /// Throws nothing: a script error is reported via `failed()`/`error()`.
  [[nodiscard]] virtual Result calculate(double t, double v) = 0;

  /// One MIMO step result: `suppress` (emit nothing) or `values`, one per declared
  /// output topic (positional). Distinct from the SISO `Result` (no explicit
  /// out_time form in MIMO v1 — outputs share the joined input timestamp).
  struct MimoResult {
    bool suppress = false;
    std::vector<double> values;
  };

  /// MIMO step (N inputs -> M outputs). `inputs[0]` is the primary value; the
  /// rest are the secondary inputs the engine joined to this sample, all in
  /// seconds since session start. A class implements EITHER this or the SISO
  /// `calculate` above; the default suppresses so a SISO-only backend is unaffected.
  [[nodiscard]] virtual MimoResult calculateMimo(double /*t*/, PJ::Span<const double> /*inputs*/) {
    return MimoResult{true, {}};
  }

  /// Clear per-instance state (optional in the script). The host generally
  /// prefers re-creating the instance (construct-new-and-swap) over reset.
  virtual void reset() = 0;

  /// True once a script error has put the instance into a sticky failed state;
  /// thereafter `calculate` suppresses. `error()` carries the message.
  [[nodiscard]] virtual bool failed() const = 0;
  [[nodiscard]] virtual const std::string& error() const = 0;
};

/// Backend-agnostic seam for a scripting language: Luau (`makeLuauEngine`) and
/// embedded Python implement it. One engine is shared by many catalogues/instances;
/// it holds no per-instance state.
class ScriptEngine {
 public:
  virtual ~ScriptEngine() = default;

  /// Parse a module's source and return EVERY filter class it declares — without
  /// running `create()`/`calculate()`. A module may `return` a single class
  /// table or a list of them (the bundled multi-class resource). `origin` labels
  /// provenance. Returns an error string on a compile/shape failure.
  [[nodiscard]] virtual Expected<std::vector<FilterClass>> inspectModule(
      const std::string& source, const std::string& origin) = 0;

  /// Build a live instance of `klass` with the given params (flat JSON keyed by
  /// parameter name) by compiling its `source`, locating the class by `id`, and
  /// calling its `create(params)`. Each instance gets its own VM.
  [[nodiscard]] virtual Expected<std::unique_ptr<FilterInstance>> createInstance(
      const FilterClass& klass, const std::string& params_json) = 0;
};

/// Construct the Luau backend. Every VM it creates is sandboxed: read-only
/// globals (`luaL_sandbox`), a curated stdlib (no io/os/loadstring/package), a
/// memory cap + an instruction-budget watchdog (see `BudgetLimits`).
[[nodiscard]] std::shared_ptr<ScriptEngine> makeLuauEngine(BudgetLimits limits = {});

}  // namespace PJ::scripting
