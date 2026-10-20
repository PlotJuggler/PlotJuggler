// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <string_view>

#include "pj_base/expected.hpp"
#include "pj_base/span.hpp"
#include "pj_scripting/object_script.h"

// The Python lane of the on-demand script runner (docs/derived_objects/ARCHITECTURE.md §14): the contract of
// object_script.h over a CPython module whose top level defines `def evaluate(inputs, params)`. Desktop only:
// on WASM `pythonAvailable()` is false and every entry point fails with "python is not available in this build".
//
// Hardened, NOT a sandbox (pj_scripting/CLAUDE.md): an AST check before compiling, gated builtins, read-only
// copies of the allowed modules, a process-wide audit hook shared with the filter engine, an allocation cap and a
// wall-clock watchdog. Scripts are trusted input.

namespace PJ::scripting {

struct PythonCode;

/// One checked and compiled Python script; it carries no state, so compile it once per recipe revision. Copies
/// share the code object, which is released under the GIL whichever thread drops the last copy.
struct CompiledPythonScript {
  std::shared_ptr<const PythonCode> code;
};

/// Whether this build runs Python scripts: true on desktop, false on WASM.
[[nodiscard]] bool pythonAvailable();

/// Checks and compiles `source` without running any of it: a syntax error (with its line in `<string>`), a
/// dunder attribute, a frame or generator internal (`gi_frame`, `f_globals`, ...), a name reaching the
/// interpreter's own namespaces (`__builtins__`, `globals`, `vars`, ...), a source over 1 MiB or no top-level
/// `def evaluate` is an error here. Takes the GIL; callable from any thread.
[[nodiscard]] Expected<CompiledPythonScript> compileOnDemandScriptPython(std::string_view source);

/// Runs `script` once as `evaluate(inputs, params)` in a fresh module namespace (D2: two runs never share
/// globals): `inputs` is a dict of the inputs by name, `params` the JSON object `params_json` (empty means `{}`,
/// at most `kMaxParamsDepth` levels). The returned value binds to `outputs` by the rules of `runOnDemandScript`
/// (a dict is keyed, a list or tuple positional; a `None` value in a dict is a missing output; `bool` is not a
/// number); `pj.unavailable(reason)` marks an output unavailable (D25). An unavailable input is an error (D6).
///
/// Script surface: number and string inputs; `import` of `math`, `cmath`, `statistics` and `json` only, each a
/// read-only copy (an assignment to a module attribute is an error, so nothing leaks into the next run) without
/// its private names or the modules it imports outside that list (`statistics.sys`); no `open`, `exec`, `eval`,
/// `compile`, `getattr`, `setattr`, `type`, `input` or `help`.
///
/// Spends from `options`: `limits.mem_bytes` caps the run's Python allocations, the host's construction of its inputs
/// and params included, `limits.python_watchdog_millis` its wall clock (the timeout lands at the next bytecode, never
/// inside a native call), the native budget the strings it returns. Running out of either fails the run even when the
/// script caught the `MemoryError` or `TimeoutError`, or the watchdog never got to interrupt it. The namespace is
/// cleared inside the run on every exit, so the finalizers it releases run under its limits. `cancelled` is polled
/// before and after the run. Thread: the Python lane; takes the GIL, which any native call it makes may release.
[[nodiscard]] Expected<ScriptBundle, ScriptError> runOnDemandScriptPython(
    const CompiledPythonScript& script, Span<const NamedValue> inputs, std::string_view params_json,
    Span<const OutputSpec> outputs, const RunOptions& options = {});

}  // namespace PJ::scripting
