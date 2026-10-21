# pj_scripting — scripting engines for Data Processors

The scripting substrate for PJ4 **Data Processors**. Filters are **self-describing
classes**: each declares its `id/name/description/version/output_kind` and a typed
`parameters` schema, plus a `create()`/`calculate()` (and optional
`calculate_batch`) implementation. The host reads that schema to (a) populate a
filter **catalogue** and (b) **generate** the parameter-editor form at runtime — so
changing a filter is a data edit, not a panel-code recompile.

Two backends sit behind the **`ScriptEngine`** seam
(`include/pj_scripting/script_engine.h`), selected by free fn:

- **Luau** — `makeLuauEngine(BudgetLimits = {})`, `src/luau_engine.cpp`. The bundled
  filter set and the default language. The binding is **hand-written**: Luau's API
  is C++ linkage, not `extern "C"`, so sol2 does not apply.
- **Python** — `makePythonEngine(BudgetLimits = {})`, `src/python_engine.cpp`. Embedded CPython via
  pybind11, IN-PROCESS (not out-of-process). Reached from the Transform Editor's
  `Lua | Python` switch; `DataProcessorService` routes to it by sniffing the
  `# pj-script: python` header on the source. Hardened — see below.

The data-processor base (`PJ::proc::DataProcessor`, owned by `pj_datastore`) stays
Qt-free, Luau-free **and Python-free**: only `pj_scripting` links either runtime.
Depends on `pj_base` + `pj_datastore` + `pj_tf` (public) + Luau + CPython/pybind11 (private).
Licensed MPL-2.0.

## Key headers

- `script_engine.h` — the `ScriptEngine` seam + `FilterInstance` (one VM per
  instance) + `makeLuauEngine(BudgetLimits)`.
- `python_engine.h` — `makePythonEngine()` and the Python filter-module contract
  (a top-level class `T` with `id` and a static `create(params)`), and its limits.
- `python_object_script.h` — the on-demand Python lane: an opaque `CompiledPythonScript` per revision (holders need no
  CPython include), checked and compiled once, and `runOnDemandScriptPython` (`evaluate(inputs, params)` in a fresh
  namespace, the binding of `object_script.h`). On desktop, and in the browser when built with `PJ_WASM_WITH_PYTHON`
  (`pythonAvailable()`; see the browser gotcha below). **No GUI-thread compile** (slice P-c):
  `deferOnDemandScriptPython`, what `compileOnDemand` gives the runtime, touches no interpreter, and the script's first
  run compiles it on the Python lane (booting the interpreter on first use), keeping the code object or the compile
  error for every later run; `compileOnDemandScriptPython` compiles on the calling thread and may wait for the GIL, so
  only tests, probes and workers call it. A validate's check, `checkOnDemandScriptPython`, is a throwaway compile on one
  Python helper thread (a thread and a queue, `src/python_object_script.cpp`), waited for at most
  `kPythonValidateWaitMillis` (100 ms) and reported unchecked (success) past it; a compiled script's last release is
  posted there too, never waited for. Python filters (`python_engine.cpp`) do not
  use the lane yet: they take the GIL on their caller's thread.
- `on_demand_script.h` — where a recipe's `language` picks its on-demand compiler and runner: `CompiledOnDemandScript`
  (Luau bytecode or the opaque Python handle), `compileOnDemand` and `runOnDemand`, which `pj_runtime` holds and calls
  without knowing the language.
- `src/python_object_binder.h` (private) — the Python twin of `object_binder.h` over the same native operations:
  objects, lazy lists and the point iterator, the point-cloud and image methods with the decoded pixels, a video's
  `frame_at` (a fallible call such as `decode()` or `frame_at` returns `(value, None)` or `(None, reason)`), the
  builders and `pj.tf`, all plain CPython types
  (pybind11 dereferences an allocation the cap refuses, and a list element costs about 195 ns, not 1.5 µs). Every operation
  that walks the data runs without the GIL; a call that needs the run's limits, media or observations (the thread's
  `PythonRun`) raises outside its run, a builder outside the run that made it, and an object kept past its run keeps
  its input's keepalive. Every proxy exposes only its documented names, no
  attribute and no buffer.
- `src/python_sandbox.h` (private) — the one interpreter (`ensureInterpreter`) and what every Python script call runs
  under: the audit hook (`ScopedAuditGuard`), the allocation cap, the watchdog thread (which also polls a call's cancel
  flags and interrupts it), `ScriptCallScope`, which arms the three around one call, and `ClearedOnExit`, which clears a script namespace inside it; and the strict profile
  on-demand scripts add (`Profile::kStrict`): the AST check (`compileChecked`), gated builtins with read-only module
  copies (`strictBuiltins`; a copy's functions share the real module's globals, which a filter can patch) and a hook
  that also refuses `exec` and `compile`.
- `src/script_contract.h` (private) — the language-neutral half of an on-demand run, shared by the Luau and Python
  runners and binders: which objects enter a script and at what stamp (`inputProblem`, `withStampShifted`,
  `ownStamp`), the comparison, extreme and channel names, the TF lookup wording, binding the returned values to outputs
  (`bindReturned`), parsing the params, the native budget of a run and the memory-limit wording.
- `filter_class.h` — the `FilterClass` schema + `ParamSpec`
  (`id/name/description/version/output_kind/parameters/source/origin`; `ParamType` ∈
  {number, integer, boolean, enum, string, text}). Drives the generated
  ParameterForm and the catalogue. The full schema/form contract is in
  [`docs/FILTER_CLASS.md`](./docs/FILTER_CLASS.md).
- `filter_catalogue.h` — `FilterCatalogue`, a Qt-free registry. v1 loads the single
  bundled multi-class resource via `addBundledSource`; plus `find`, `entries`,
  `makeProcessor`, and `makeProcessorFromSource` (embedded-source restore). A
  user-override dir + marketplace install are DEFERRED.
- `lua_siso_transform.h` — `LuaSisoTransform`, a `PJ::proc::DataProcessor` whose
  `calculateNextPoint` runs a Luau filter class via the `ProcessorSisoAdapter` as an
  eager `pj_datastore::IncrementalEngine` node.
- `sandbox.h` — `BudgetLimits` (the per-VM sandbox/watchdog budget).
- `src/luau_sandbox_state.h` (private) — the sandbox state every Luau engine shares: `BudgetState` (memory cap, fuel,
  optional `cancelled` poll, sticky `trip` reason), the allocator and interrupt, `tripRun`, and `newSandboxedState`.
  `trip` is checked by the host after each protected call even if the script `pcall`-swallowed the error.
- `marker_engine.h` — `runMarkerScript`, the whole-series → markers path (Luau). Its interrupt callback also polls
  the caller's `cancelled` check, so a cancelled script stops inside the interpreter, and native marker output is
  capped by `max_output_bytes` (16 MiB by default) independently of the VM's memory cap.
- `object_script.h` — the Luau object-script engine the on-demand service calls on a worker: compile once per recipe
  revision, a fresh sandboxed VM per run, `pj.tf` over a TF snapshot, output inference or lenient binding, the
  observed-clock/TF flags on the result and the error (and `over_budget` when a memory limit ended the run), a native ledger runs may share (`makeNativeBudget`, `RunOptions::native_budget`), and `estimateBytes(value)` (a decoded value's size from its
  field table, never by serializing it). Contract: the header; design:
  [`docs/derived_objects/ARCHITECTURE.md`](../docs/derived_objects/ARCHITECTURE.md) §5-7, §12, §14.
- `image_decoder.h` — the headless codec seam of the object scripts: `DecodedPixels` (original samples as floats, the
  pairing an annotation made from them takes, a charge) and `IImageDecoder`, which the session injects
  (`RunOptions::image_decoder`), so scripting links no codec.
- `video_frame_source.h` — the video seam: `IVideoFrameSource::frameAt` (the frame presented at a time, rgb8, charged
  before it is allocated) and `retireDataset`, which the session injects (`RunOptions::video_source`; pj_app's
  `SceneVideoFrameSource`), so scripting links no codec. A video input's `frame_at` reads it.
- `memory_budget.h` — `MemoryBudget`, one byte limit per evaluation for native allocations (a ledger separate from the
  VM's memory cap; its error reads "script native memory limit (N MB) exceeded"), and `MemoryReservation`, bytes held
  until destroyed. Public because a decoder charges its samples to it. An output's reservation lives in its buffer
  anchor (clouds) or beside it (`Charged<Value>`, a builder's output; the binder shares it between the output's script
  box and the views read from it), so retained outputs keep counting. Allocating operations take `NativeLimits` (that
  budget plus the run's cancel check, polled every 65536 records, or once per image row).
- `src/script_media.h` (private) — `ScriptMedia`, one run's media state, shared by the Luau and Python binders:
  the injected decoder and video source, the image and video inputs the run read (held for the run, so an address
  names one input even after the script drops it), one decode per input and run, the
  frames `frame_at` made, and what a finished annotations builder pairs with (the image input it names, a video's
  frames when they agree, else the result's stamp), and the result's stamp a builder's cloud or grid takes
  (`stampUnset`).
- `object_summary.h` — `summarizeObject` (a small JSON description of an object output: counts, bounds, frame ids) and
  `isEmptyObject` (the report's "empty" status), what an evaluation report carries instead of the object's bytes.
- `src/object_binder.h` (private) — objects as locked, read-only views over their SDK field tables (lists read
  lazily through `list_at`), `pj.int64`, the `pj.unavailable` marker, the point-cloud methods over `src/native/`, and
  the image methods (`representation`, `decode`, `to_point_cloud`) with the decoded-pixel box, and a video's `frame_at`.
  Also the `pj.scene` / `pj.annotations` / `pj.cloud` / `pj.voxels` builders: chainable boxed builders over the
  native ones, `finish()` spends a builder, a finished object's box and views share its native charge, and a method
  reads its options table raw (no script code runs inside a builder call); scripts cannot reach their metatables.
- `src/native/` (private; consumed by the object-script binder) — the heavy
  half of an object script, Luau-free so Luau and Python share it:
  - `point_cloud_ops.h`: count, bounds, extreme point, `cropBox`, `filterByField`, `transformXyz`,
    `nextFinitePoint`, and `downsample` (one centroid per occupied cube, built with the xyz preset). Little-endian
    only, x/y/z float32/float64, `row_step` honoured (rows may not overlap), the declared extent checked against
    `data` without overflow (the SDK field table's check), whole records copied (D15), non-finite points skipped;
    every output keeps the input's stamp.
  - `record_walk.h`: the row walk (`forEachRecord`) and `compactWhere`, which every cloud operation shares;
    a header so the tests drive them directly.
  - `scene_entities_ops.h`: `SceneEntityBuilder`, the snapshot idiom (one `kAll` deletion, then the
    entity) with errors that name the bad field (D27). `finish()` consumes the builder.
  - `point_cloud_builder.h`: `PointCloudBuilder`, PCL's familiar xyz / xyzi / xyzrgb point types in 16-byte records
    (D14); its buffer's capacity is charged before it grows (the old buffer beside the new until freed), every value
    must fit a float32, and a cloud is capped so its byte size fits a uint32.
  - `voxel_grid_ops.h`: `VoxelGridBuilder`, a dense float32 VoxelGrid (D16), its dimensions capped at 2^24 cells
    before anything is charged or allocated. A VoxelGrid is producible but has no SDK field table yet, so a script
    cannot read its fields.
  - `image_annotations_ops.h`: `ImageAnnotationsBuilder`, the same shape for pixel-space annotations
    (`image_topic` required, no text background; the binder pairs a finished set through `ScriptMedia`).
    `field_checks.h`: the argument checks both builders share.
  - `image_ops.h`: over `DecodedPixels`: `imageRepresentation`, pixel, gray, crop (keeping source coordinates),
    threshold (one points annotation, all or nothing under the budget, a non-finite sample never matching) and stats;
    one templated kernel per comparison, polled once per row.
  - `depth_ops.h`: `depthAsImage` and `depthToPointCloud`, a rectified depth image (16UC1/mono16 millimetres, 32FC1
    metres) through a CameraInfo's K into an organized x/y/z float32 cloud, NaN where there is no depth.

## Contract / gotchas

- **Sandbox + watchdog (`sandbox.h` `BudgetLimits`).** Each filter VM runs
  IN-PROCESS on the commit thread, so it is hardened: `luaL_sandbox` freezes the
  shared stdlib read-only, then `luaL_sandboxthread` gives the VM its own writable
  global table layered on the frozen stdlib (read-only `__index`). Net effect: a
  filter can assign top-level globals for persistent state, but cannot mutate the
  stdlib or leak globals into a sibling filter's VM. Plus a custom `lua_Alloc`
  memory cap and an interrupt-based instruction-budget watchdog armed per protected
  region. `os` / `io` / `require` / `debug` / `setfenv` / `getfenv` are blocked
  (`setfenv` would let a script swap its environment back onto the frozen stdlib).
  Each VM loads exactly ONE module, so `safeenv` stays on and stdlib imports keep
  the fast path. A sticky `trip` reason is checked by the host **even if the script
  `pcall`-swallows the error**, so a runaway can never silently survive.
- **Python runs under cooperative limits, and it is a guardrail — NOT a sandbox.** Both Python engines share one
  interpreter, never finalized, and `src/python_sandbox.h`: every call into script code, a filter's teardown included,
  runs inside a `ScriptCallScope` (the watchdog; the allocation cap, which refuses only blocks of 1 KiB or more and
  stops a call at twice the cap with an asynchronous MemoryError, so CPython never sees a small allocation fail; both
  sticky even when the script catches the error; and the process-global audit hook armed on that thread). The cap's
  bookkeeping is one hash table per allocator domain keyed by block address, whose records point at their budget (a live
  block keeps it alive; the GIL serializes MEM and OBJ, only RAW locks; about 10 ns per block, 1.4-1.5x on an
  allocation-heavy loop, `PythonSandboxBench.AllocationCapOverhead`). Two profiles (decided 2026-10-20): filters keep
  their builtins, syntax and real modules behind the `math`/`cmath`/`statistics` allowlist, so existing filters run
  unchanged and stay as escapable as before; on-demand scripts add the strict profile. The trust model (decided
  2026-10-18) is in-process with cooperative limits: scripts are trusted input. Compile on the host and run the code
  object inside the scope (`runCode`). An armed run cannot import a module that is not loaded yet (the hook refuses
  its file open), and a C-level import from a strict run goes through the gate, so the boot also loads what the
  allowed modules import lazily on a reachable path (`kLazyDependencies`: `heapq` behind `statistics.Counter`) and
  formats one Decimal to cache decimal's `_pydecimal` fallback (`statistics.Decimal`); a new such path is added there
  and to the browser's `stdlib_zip.py` roots.
- **CPython is linked SHARED on every desktop platform** (`conanfile.txt`
  `cpython/*:shared=True`), and this is load-bearing, not a preference. The stdlib's
  C extensions (`lib-dynload/*.so` — `math`, `zlib`, …) carry no copy of the
  interpreter and resolve `Py*` at dlopen time against the process; a static
  libpython leaves those symbols out of `pj_app`'s `.dynsym` and every such import
  dies with `undefined symbol: PyFloat_Type`. `-rdynamic` cannot rescue it —
  `pj_app` links `--exclude-libs,ALL`, which marks static-archive symbols
  `STV_HIDDEN`, and ELF visibility only ever narrows. `pj_app`'s
  `ExportedSymbolsGuard` test pins that flag in place; `--selftest-python` (run
  against the packaged AppImage in CI) pins the runtime end.
- **Python in the browser (`PJ_WASM_WITH_PYTHON`, off by default; WASM CI's acceptance build turns it on).** Only the
  on-demand lane: Python filters stay unavailable (`python_engine_unavailable.cpp`), because they take the GIL on the
  commit thread without a bound. `cmake/PjWasmPython.cmake` builds a static CPython 3.14.8 for wasm32-emscripten with
  pthreads at configure time from the SHA-256-pinned tarball (`cmake/wasm_python/`: the build script, two local
  patches in one file — every thread uses the JS call trampoline, and of upstream's syscall overrides only
  `__syscall_ioctl` is kept, which replaces Emscripten's for the whole process, Qt included — the `Setup.local` that
  drops 26 unused builtins, and `stdlib_zip.py`); pybind11 is 3.0.4 there (2.13 predates 3.14), against 3.12 and
  2.13 on desktop. The stdlib is a pyc-only zip embedded at `/lib/python314.zip` holding only the import closure of
  a root list; `pj_app`'s acceptance probe (`tests/wasm_python_stdlib_probe.cpp`) walks the strict profile's reach
  and fails on any lazy import not loaded at boot (see the next bullet's preload). `main.cpp` calls `prestartPythonHelper()` at startup, because a
  thread created while the browser's main thread waits on it never starts, and the pthread pool is 7. The GUI never
  compiles Python for a recipe, and a validate waits for the helper at most 100 ms, so a script holding the GIL inside
  one C call no longer freezes the page. Guide: `docs/WASM_DEPLOYMENT.md`.
- **Time contract (precision-critical).** The script sees `t` = **seconds since
  session start**. The int64-ns session-start is subtracted on the absolute spine
  **before** the double cast (avoiding epoch-double quantization, ~256 ns at epoch
  magnitude); output timestamps are reconstructed onto the absolute int64 spine. Get
  this wrong and every `dt`-based filter quantizes.
- **`LuaSisoTransform` failure mode.** A script runtime error **fails the node**
  (sticky), never corrupting a stateful accumulator. `reset()` =
  construct-new-and-swap. Luau filters are numeric-scalar only: a string-valued
  input is silently coerced to `0.0` (`proc::detail::toDouble`), not rejected —
  in practice series fed to a filter are numeric, so this case does not arise.
- **Filters are DATA.** `resources/filters/builtin_filters.luau` is ONE `.luau`
  resource returning a list of 12 classes (none, absolute, scale, derivative,
  integral, moving_average, moving_rms, moving_variance, outlier_removal,
  samples_counter, binary_filter, time_since_previous). The app reads the QRC
  resource and hands the source string to `addBundledSource` — `pj_scripting` never
  touches Qt. v1 loads only this bundled resource.

## Read deeper

| For | Read |
|---|---|
| The filter-class + ParameterForm schema contract, sandbox, time rebasing | [`docs/FILTER_CLASS.md`](./docs/FILTER_CLASS.md) |
| The processor base this implements | `../pj_datastore/include/pj_datastore/data_processor.hpp` |
