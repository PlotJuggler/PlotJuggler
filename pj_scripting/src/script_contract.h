// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

// The language-neutral half of an on-demand run, shared by the Luau and Python runners: the checks of a run's params
// and native budget, the memory-limit wording, and the binding of the returned values to outputs (object_script.h).

#include <cstddef>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_base/span.hpp"
#include "pj_scripting/memory_budget.h"
#include "pj_scripting/object_script.h"

namespace PJ::scripting::detail {

/// The longest `pj.unavailable(reason)` text a run keeps; the rest is cut.
inline constexpr std::size_t kMaxReasonBytes = 1024;

/// "script memory limit (N MB) exceeded".
[[nodiscard]] std::string vmLimitMessage(std::size_t limit_bytes);

/// A memory limit ended the run: its own cap (`vmLimitMessage`) or the native budget.
[[nodiscard]] bool isOverBudget(std::string_view message);

/// Object types an output can declare: those with a field table, except the ones whose publication no slice has
/// designed yet (a video frame; frame transforms, which the TF ingest tap would read).
[[nodiscard]] bool producible(sdk::BuiltinObjectType type);

/// `params_json` (empty means `{}`) as a JSON object at most `kMaxParamsDepth` levels deep, or why the run is refused.
[[nodiscard]] Expected<nlohmann::json> parseParams(std::string_view params_json);

/// The ledger a run charges for what it returns: `options.native_budget`, else a fresh one of `native_budget_bytes`.
[[nodiscard]] std::shared_ptr<native::MemoryBudget> nativeBudgetOf(const RunOptions& options);

/// One returned value: its key in the returned table, or its position (1-based), or neither for the returned value
/// itself; nullopt `value` for a type no output holds, named by `type_name`.
struct Returned {
  std::string key;
  int position = 0;
  std::optional<ScriptValue> value;
  std::string type_name;
};

/// Binds the returned values to outputs. No `declared` outputs: every value is an output, named by the inference rule
/// (`runOnDemandScript`). Otherwise a non-table binds to the single declaration, a position to the declaration at that
/// position, a key to the declaration of that name; every value must have its declared type or be unavailable.
[[nodiscard]] Expected<ScriptBundle> bindReturned(std::vector<Returned>& returned, Span<const OutputSpec> declared);

}  // namespace PJ::scripting::detail
