// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include "pj_base/expected.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"  // sdk::ScriptCheck
#include "pj_base/span.hpp"
#include "pj_scripting/object_script.h"

// The Python lane of the on-demand script runner (docs/derived_objects/ARCHITECTURE.md §14): the contract of
// object_script.h over a CPython module whose top level defines `def evaluate(inputs, params)`. Where
// `pythonAvailable()` is false (WASM without PJ_WASM_WITH_PYTHON) every entry point fails with "python is not available
// in this build".
//
// Hardened, NOT a sandbox (pj_scripting/CLAUDE.md): an AST check before compiling, gated builtins, read-only
// copies of the allowed modules, a process-wide audit hook shared with the filter engine, an allocation cap and a
// wall-clock watchdog. Scripts are trusted input.

namespace PJ::scripting {

struct PythonCode;

/// One Python script, checked and compiled at most once: by `compileOnDemandScriptPython`, or at the first run of one
/// `deferOnDemandScriptPython` gave. It carries no state, so make it once per recipe revision. Copies share the code
/// object; dropping the last copy posts its release to the Python helper thread and returns at once, on any thread.
struct CompiledPythonScript {
  std::shared_ptr<const PythonCode> code;
};

/// How long `checkOnDemandScriptPython` waits for its throwaway compile before answering unchecked: about
/// 100 times an uncontended compile of a short script (under 1 ms native, a few ms in the browser), and 20 GIL switch
/// intervals (5 ms) behind a running pure-Python script, yet within one frame budget a check button may freeze the
/// browser's main thread. The interpreter's boot (0.13-0.24 s in the browser) does not fit: the first validate of a
/// session boots it on the helper and is most likely unchecked.
inline constexpr std::uint64_t kPythonValidateWaitMillis = 100;

/// Whether this build runs Python scripts: true on desktop and on a WASM build with PJ_WASM_WITH_PYTHON, else false.
[[nodiscard]] bool pythonAvailable();

/// Starts the Python helper thread without booting the interpreter. In the browser a thread created while the main
/// thread waits may never start (Emscripten spawns its worker only once the main thread yields), so the app calls this
/// at startup, before `checkOnDemandScriptPython` first waits. Idempotent; a no-op without Python.
void prestartPythonHelper();

/// Test seam: the wait `checkOnDemandScriptPython` uses from now on in place of `kPythonValidateWaitMillis`; 0 restores
/// the constant. A test asserting a checked result sets a long one, so a loaded runner never turns it unchecked; a
/// test of the bound itself keeps the constant. Any thread.
void setPythonValidateWaitForTest(std::uint64_t millis);

/// A validate's check of `source`: a throwaway compile (nothing cached, no script handle touched) on the Python
/// helper thread, waited for at most `kPythonValidateWaitMillis`. Finished in time: valid, or invalid with
/// `compileOnDemandScriptPython`'s error text. Not finished (a script holds the GIL, the interpreter is still booting),
/// or the helper already busy with an item (then at once): unchecked, with why; the first run reports the error anyway,
/// and the helper skips a compile nobody waits for any more. An error only where `pythonAvailable()` is false. Any
/// thread but the helper, without the GIL.
[[nodiscard]] Expected<sdk::ScriptCheck> checkOnDemandScriptPython(std::string_view source);

/// `source` as a script its first `runOnDemandScriptPython` checks and compiles, on that run's thread: what the runtime
/// holds per recipe revision. Touches no interpreter, so it never waits for the GIL (in the browser a main-thread wait
/// freezes the page); a compile error is the first run's error, and every later run's, without compiling again. Fails
/// only where `pythonAvailable()` is false. Any thread.
[[nodiscard]] Expected<CompiledPythonScript> deferOnDemandScriptPython(std::string_view source);

/// Checks and compiles `source` now, on the calling thread, without running any of it: a syntax error (with its line
/// in `<string>`), a dunder attribute, a frame or generator internal (`gi_frame`, `f_globals`, ...), a name reaching
/// the interpreter's own namespaces (`__builtins__`, `globals`, `vars`, ...), a source over 1 MiB or no top-level
/// `def evaluate` is an error here. Boots the interpreter on first use and takes the GIL, waiting as long as a script
/// holds it: never call it where a wait is not allowed (the GUI thread; the runtime uses `deferOnDemandScriptPython`).
/// Without the GIL.
[[nodiscard]] Expected<CompiledPythonScript> compileOnDemandScriptPython(std::string_view source);

/// Runs `script` once as `evaluate(inputs, params)` in a fresh module namespace (D2: two runs never share
/// globals): `inputs` is a dict of the inputs by name, `params` the JSON object `params_json` (empty means `{}`,
/// at most `kMaxParamsDepth` levels). The returned value binds to `outputs` by the rules of `runOnDemandScript`
/// (a dict is keyed, a list or tuple positional; a `None` value in a dict is a missing output; `bool` is not a
/// number); `pj.unavailable(reason)` marks an output unavailable (D25). An unavailable input is an error (D6).
///
/// Script surface: `object_script.h`'s, in Python's idiom (python_object_binder.h binds it over the same native
/// operations). Objects are read-only: a field is an attribute (an int64 is a Python int,
/// a raw buffer is refused), a list a lazy sequence. Methods take keyword arguments named as Luau's option keys; a
/// vector is a tuple (`bounds()["min"]`), every index 0-based, not-equal `!=`. `pj.scene.new(frame_id=, id=)`,
/// `pj.annotations.new(image_topic=)`, `pj.cloud.new(preset=, frame_id=)` (`add(x, y, z, ...)` takes the values
/// positionally, `add_points(points)` a sequence of them) and `pj.voxels.new(frame_id=, origin=, cell_size=, dims=)`
/// (`set(i, j, k, value)`, int indices) build and chain; `pj.tf.now` is an int, `pj.tf.lookup(target, source,
/// when=None)` returns `(transform, None)` or `(None, reason)`, the transform a dict of `translation`, `rotation` and
/// `target_frame`, and `when` an int or float time, or an object (its own stamp, on the header clock). An image's
/// `decode()` returns `(pixels, None)` or `(None, reason)`, the shape of every fallible call, and a video's
/// `frame_at(t)` (an int, on the dataset clock) `(frame, None)`, the frame a dict of `image` and `presentation_ns`;
/// `to_point_cloud(camera_info, step=None, max_depth=None)` and `threshold`, whose result is an output, a value or
/// `pj.unavailable(reason)`. A call that needs the run's limits, media or observations raises outside its run (field
/// reads, `count`, `bounds`, `extreme_point` and `xyz_points` iteration work anywhere); an object a script leaves
/// behind holds its input's `keepalive`, and a builder works only in the run that made it. An output keeps its native
/// charge in its `keepalive`. `import` of `math`, `cmath`, `statistics` and `json` only, each a read-only copy (an
/// assignment to a module attribute is an error; state a script leaves on a class a copy shares with its module, such
/// as `statistics.NormalDist`, reaches the next run, the trust model's documented limit) without its private names or
/// the modules it imports outside that list
/// (`statistics.sys`); no `open`, `exec`, `eval`, `compile`, `getattr`, `setattr`, `type`, `input` or `help`.
///
/// Spends from `options`: `limits.mem_bytes` caps the run's Python allocations, the host's construction of its inputs
/// and params included, `limits.python_watchdog_millis` its wall clock (the timeout lands at the next bytecode, never
/// inside a native call), the native budget the strings it returns. Running out of either fails the run even when the
/// script caught the `MemoryError` or `TimeoutError`, or the watchdog never got to interrupt it. The namespace is
/// cleared inside the run on every exit, so the finalizers it releases run under its limits. `cancelled` is polled
/// before and after the run, and by the native operations; `cancel_flags`, polled by the watchdog, interrupt the
/// bytecode, and a cancelled run fails "script cancelled" whatever it raised. The native budget also pays for the
/// native outputs (crops, builders), which a native operation that runs out retries once after a garbage collection.
/// The result and the error carry what the run observed (`pj.tf.now`, `pj.tf.lookup`, `pj.tf.frames`). A script not
/// compiled yet is compiled first, after the cancellation check (a cancelled run compiles nothing), as
/// `compileOnDemandScriptPython`, and its compile error is the run's. Thread: the Python lane, without the GIL; takes
/// the GIL, which every native operation releases.
[[nodiscard]] Expected<ScriptBundle, ScriptError> runOnDemandScriptPython(
    const CompiledPythonScript& script, Span<const NamedValue> inputs, std::string_view params_json,
    Span<const OutputSpec> outputs, const RunOptions& options = {});

}  // namespace PJ::scripting
