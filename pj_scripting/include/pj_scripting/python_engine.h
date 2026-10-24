// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>

#include "pj_scripting/sandbox.h"
#include "pj_scripting/script_engine.h"

namespace PJ::scripting {

/// Construct the embedded-CPython backend (MVP). Implements the same `ScriptEngine`
/// seam as `makeLuauEngine`, so a `FilterCatalogue` built with it produces the same
/// `LuaSisoTransform`/`LuaMimoTransform` nodes — no new streaming plumbing.
///
/// A Python "filter module" defines a top-level class named `T`:
///
///     # pj-script: python
///     class T:
///         id = "my_filter"
///         name = "My Filter"
///         output = "double"            # "double" | "int64" | "same"
///         @staticmethod
///         def create(params):          # params: dict from params_json
///             return T()
///         def calculate(self, time, value, *args):
///             return value * 2         # number, or (out_time, value), or None to suppress
///
/// Limits (`limits`): each `calculate()`/`reset()` runs under a wall-clock watchdog of `python_call_millis`, module
/// evaluation and `create()` under `python_watchdog_millis`, and an instance's allocations, across its whole life,
/// under `mem_bytes` (past it a block of 1 KiB or more fails; smaller ones are served until twice the cap, where a
/// MemoryError stops the call). Past either the instance fails for good, even if the script caught the exception, and a
/// call past its deadline fails even when the watchdog never interrupted it; a native call is never interrupted, the
/// timeout lands at the next bytecode. Script code (its teardown included) runs with the process-wide audit hook armed,
/// which refuses `open`, `os.*`, `subprocess.*` and the like (python_sandbox.h).
///
/// The compatible profile: filters keep the real builtins (`getattr`, `type`, `eval`, `str.format`, ...), no AST
/// check, and the real modules behind the `math`/`cmath`/`statistics` import allowlist, so existing filters run
/// unchanged; they are as escapable as before. On-demand scripts use the strict profile of the same sandbox.
/// One process-wide interpreter (one GIL), shared with the on-demand Python lane: a guardrail, not a sandbox.
[[nodiscard]] std::shared_ptr<ScriptEngine> makePythonEngine(BudgetLimits limits = {});

}  // namespace PJ::scripting
