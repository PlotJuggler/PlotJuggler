// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
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
/// share the code object; dropping the last copy posts its release to the Python helper thread and returns at once,
/// on any thread.
struct CompiledPythonScript {
  std::shared_ptr<const PythonCode> code;
};

/// How long `compileOnDemandScriptPython` waits for the Python helper thread before failing with `kPythonBusyError`.
inline constexpr std::uint64_t kPythonCompileWaitMillis = 2000;
/// Why a compile gave up waiting: the GIL stayed held past `kPythonCompileWaitMillis`.
inline constexpr std::string_view kPythonBusyError =
    "Python is busy (a running script holds the interpreter); try again";

/// Whether this build runs Python scripts: true on desktop, false on WASM.
[[nodiscard]] bool pythonAvailable();

/// Checks and compiles `source` without running any of it: a syntax error (with its line in `<string>`), a
/// dunder attribute, a frame or generator internal (`gi_frame`, `f_globals`, ...), a name reaching the
/// interpreter's own namespaces (`__builtins__`, `globals`, `vars`, ...), a source over 1 MiB or no top-level
/// `def evaluate` is an error here. Runs on the Python helper thread (the interpreter boots there on first use, and the
/// first compile registers the binder's types), which may wait for the GIL as long as a script holds it; the caller
/// waits at most `kPythonCompileWaitMillis`, then fails with `kPythonBusyError` and the helper skips the compile. Once
/// one caller gave up behind the helper's current item, a compile fails at once until that item finishes. The first
/// compile, which boots the interpreter, is waited for unbounded: no script can hold the GIL before it. Callable from
/// any thread but the helper, without the GIL.
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
/// The result and the error carry what the run observed (`pj.tf.now`, `pj.tf.lookup`, `pj.tf.frames`). Thread: the
/// Python lane; takes the GIL, which every native operation releases.
[[nodiscard]] Expected<ScriptBundle, ScriptError> runOnDemandScriptPython(
    const CompiledPythonScript& script, Span<const NamedValue> inputs, std::string_view params_json,
    Span<const OutputSpec> outputs, const RunOptions& options = {});

}  // namespace PJ::scripting
