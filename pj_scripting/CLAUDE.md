# pj_scripting — scripting engines for Data Processors

The scripting substrate for PJ4 **Data Processors** (filters, transforms, marker rules) and **on-demand object
scripts** (derived objects). Filters are **self-describing classes**: each declares its
`id/name/description/version/output_kind` and a typed `parameters` schema, plus a `create()`/`calculate()` (and
optional `calculate_batch`) implementation. The host reads that schema to populate a filter **catalogue** and to
**generate** the parameter-editor form at runtime, so changing a filter is a data edit, not a panel-code recompile.

Two languages sit behind the **`ScriptEngine`** seam (`include/pj_scripting/script_engine.h`):

- **Luau**: `makeLuauEngine(BudgetLimits = {})`, `src/luau_engine.cpp`. The bundled filter set and the default
  language. The binding is **hand-written**: Luau's API is C++ linkage, not `extern "C"`, so sol2 does not apply.
- **Python**: `makePythonEngine(BudgetLimits = {})`, `src/python_engine.cpp`. Embedded CPython via pybind11,
  in-process. `pj_runtime`'s `DataProcessorService` routes to it by the recipe's language or the
  `# pj-script: python` first-line directive.

The data-processor base (`PJ::proc::DataProcessor`, owned by `pj_datastore`) stays Qt-free, Luau-free **and
Python-free**: only `pj_scripting` links either runtime. `pj_scripting` never touches Qt.

## Targets

- **`pj_scripting_core`** (`src/luau_engine.cpp`, `src/marker_engine.cpp`) is the Luau engine plus the whole-series
  marker analysis. It depends only on `pj_base`, `nlohmann_json` (public: `filter_class.h` exposes it), Luau and kissfft
  (private). It is also packaged on its own for the SDK-only headless runner, which links the same engine so a rule
  runs identically in the GUI and headless; `marker_engine_test` links only it (plus gtest) and guards that.
- **`pj_scripting`** adds everything that needs the host: the data-processor adapters (`lua_siso_transform.cpp`,
  `lua_mimo_transform.cpp`), the filter catalogue, the object-script engines (Luau and Python) and `src/native/`. It
  links `pj_scripting_core`, `pj_datastore` and `pj_tf` publicly; Luau, tsl-robin-map and (desktop, and the browser
  with `PJ_WASM_WITH_PYTHON`) CPython/pybind11 privately. Builds without Python compile the `*_unavailable.cpp` stubs.
- **Rule:** never include `pj_datastore`, `pj_tf` or Qt from a `pj_scripting_core` source, or the standalone package
  stops building. Only `pj_scripting` links `pj_datastore` and `pj_tf`.

## Key headers

Each header's doc-comment owns its contract; one line each here.

Filters and processors:
- `script_engine.h`: the `ScriptEngine` seam, `FilterInstance` (one VM per instance), `makeLuauEngine`.
- `python_engine.h`: `makePythonEngine` and the Python filter-module contract (a top-level class with `id` and a
  static `create(params)`).
- `filter_class.h`: the `FilterClass` schema + `ParamSpec`; drives the generated ParameterForm and the catalogue. Full
  contract: [`docs/FILTER_CLASS.md`](./docs/FILTER_CLASS.md).
- `filter_catalogue.h`: `FilterCatalogue`, the Qt-free registry; loads the one bundled multi-class resource via
  `addBundledSource`, and `makeProcessorFromSource` restores an embedded source.
- `lua_siso_transform.h`, `lua_mimo_transform.h`: a filter class run as a `PJ::proc::DataProcessor` node in the
  `pj_datastore::IncrementalEngine` (SISO via `ProcessorSisoAdapter`; MIMO returns one value per declared output).
- `marker_engine.h`: `runMarkerScript`, whole-series → markers (Luau); polls the caller's `cancelled` inside the
  interpreter and caps native output at `max_output_bytes` independently of the VM cap.
- `sandbox.h`: `BudgetLimits`, the per-VM sandbox/watchdog budget.

On-demand object scripts:
- `on_demand_script.h`: where a recipe's `language` picks its compiler and runner (`CompiledOnDemandScript`,
  `compileOnDemand`, `runOnDemand`, `checkOnDemand`); `pj_runtime` calls these without knowing the language.
- `object_script.h`: the Luau object-script engine the on-demand service calls on a worker (compile once per recipe
  revision, `pj.tf` over a TF snapshot, output binding, shared native ledger, `estimateBytes`).
- `python_object_script.h`: the on-demand Python lane (opaque `CompiledPythonScript`, so holders need no CPython
  include; `pythonAvailable()`, `prestartPythonHelper()`, the validate wait).
- `memory_budget.h`: `MemoryBudget`, the per-evaluation native-allocation ledger (separate from the VM's memory cap),
  and `MemoryReservation`. An output keeps its reservation alive, so retained outputs keep counting.
- `image_decoder.h`, `video_frame_source.h`: the codec seams the session injects (`RunOptions::image_decoder`,
  `RunOptions::video_source`), so scripting links no codec.
- `object_summary.h`: `summarizeObject` / `isEmptyObject`, what an evaluation report carries instead of object bytes.

Private (`src/`):
- `luau_sandbox_state.h`: the sandbox state every Luau engine shares (`BudgetState`, allocator, interrupt, `tripRun`,
  `newFrozenState`, `newSandboxedState`).
- `object_binder.h`, `python_object_binder.h`: the Luau and Python bindings of objects, media and the
  `pj.scene` / `pj.annotations` / `pj.cloud` / `pj.voxels` builders, over the same native operations.
- `python_sandbox.h`: the one interpreter (`ensureInterpreter`), `ScriptCallScope` (watchdog + allocation cap + audit
  hook around one call), and the strict profile (`compileChecked`, `strictBuiltins`).
- `script_contract.h`: the language-neutral half of an on-demand run shared by both runners and binders (input
  stamps, binding returned values to outputs, params, run budget, error wording).
- `script_media.h`: `ScriptMedia`, one run's media state (decoder, video source, inputs read, one decode per input
  and run, what a finished annotations builder pairs with).
- `native/`: the heavy, Luau-free half both binders share: `point_cloud_ops.h`, `record_walk.h`,
  `point_cloud_builder.h`, `voxel_grid_ops.h`, `scene_entities_ops.h`, `image_annotations_ops.h`, `field_checks.h`,
  `image_ops.h`, `depth_ops.h`. Each header states its layout and budget contract. Every allocating operation
  charges its budget **before** allocating and polls the run's cancel check; every output keeps the input's stamp.

## Contract / gotchas

- **Luau filter sandbox (`BudgetLimits`).** Each filter VM runs in-process on the commit thread, so it is hardened:
  `luaL_sandbox` freezes the shared stdlib read-only, then `luaL_sandboxthread` gives the VM its own writable global
  table over it. A filter can keep state in top-level globals but cannot mutate the stdlib or leak into a sibling
  filter's VM. A custom `lua_Alloc` memory cap and an interrupt-based instruction budget are armed per protected
  region. `os` / `io` / `require` / `debug` / `setfenv` / `getfenv` are blocked (`setfenv` would let a script swap its
  environment back onto the frozen stdlib). Each VM loads exactly ONE module, so `safeenv` stays on.
- **The sticky `trip` is checked by the host after every protected call, even if the script `pcall`-swallowed the
  error**, so a runaway never silently survives. Keep that check on any new call path.
- **On-demand Luau shares one frozen state per worker thread** (`ThreadState` in `src/object_script.cpp`); each run
  gets a fresh thread with its own globals and `pj` table, re-armed limits, and a full collection at its end.
  **Anything reachable from the shared state must stay frozen or unreachable**: a new writable table or metatable a
  script can reach leaks between recipes. The GAP-38 tests in `tests/object_script_test.cpp` guard this; extend them
  when you add something to the shared state. Design: derived_objects ARCHITECTURE §14.
- **Python runs under cooperative limits: a guardrail, NOT a sandbox.** Scripts are trusted input. Both Python
  engines share one interpreter, never finalized. Every call into script code, a filter's teardown included, runs
  inside a `ScriptCallScope` (watchdog, allocation cap, audit hook); failures are sticky even when the script catches
  them. Compile on the host and run the code object inside the scope (`runCode`): the strict hook refuses compiles.
  Filters use the compatible profile (real builtins and modules behind the `math`/`cmath`/`statistics` allowlist, so
  existing filters run unchanged); on-demand scripts add the strict profile. Details: ARCHITECTURE §14.
- **Lazy stdlib imports must be preloaded.** An armed run cannot import a module not loaded yet (the hook refuses its
  file open), so the boot loads what the allowed modules import lazily on a reachable path (`kLazyDependencies` in
  `src/python_sandbox.cpp`, plus the warmed decimal fallback). A new such path is added there **and** to the browser's
  `cmake/wasm_python/stdlib_zip.py` roots; `pj_app`'s `tests/wasm_python_stdlib_probe.cpp` catches a miss.
- **No GUI-thread Python compile or GIL wait.** `compileOnDemand` defers (`deferOnDemandScriptPython`, touches no
  interpreter); the first run compiles on the Python lane. `compileOnDemandScriptPython` compiles on the calling
  thread and may wait for the GIL, so only tests, probes and workers call it. A validate
  (`checkOnDemandScriptPython`) waits for the Python helper thread at most `kPythonValidateWaitMillis`, then answers
  unchecked. Python filters (`python_engine.cpp`) do not use the lane: they take the GIL on their caller's thread.
- **CPython is linked SHARED on every desktop platform** (`cpython/*:shared=True` in `conanfile.txt`); never make it
  static. The stdlib's C extensions (`lib-dynload/*.so`) resolve `Py*` against the process at dlopen time; a static
  libpython leaves those symbols out of `pj_app`'s `.dynsym` (`pj_app` links `--exclude-libs,ALL`, which hides
  static-archive symbols, and `-rdynamic` cannot undo that), so every such import dies with
  `undefined symbol: PyFloat_Type`. `pj_app`'s `ExportedSymbolsGuard` test and `--selftest-python` (run against the
  packaged AppImage/deb in CI) pin both ends.
- **Python in the browser (`PJ_WASM_WITH_PYTHON`, on by default).** Only the on-demand lane: Python filters stay
  unavailable (`python_engine_unavailable.cpp`) because they take the GIL on the commit thread without a bound.
  `pj_app/src/main.cpp` calls `prestartPythonHelper()` at startup, because a thread created while the browser's main
  thread waits on it never starts. CPython build, stdlib zip and audits: [`docs/WASM_DEPLOYMENT.md`](../docs/WASM_DEPLOYMENT.md).
- **Time contract (precision-critical).** The script sees `t` = **seconds since session start**. The int64-ns
  session start is subtracted on the absolute spine **before** the double cast (epoch-magnitude doubles quantize to
  about 256 ns); output timestamps are reconstructed onto the absolute int64 spine. Get this wrong and every
  `dt`-based filter quantizes.
- **`LuaSisoTransform` failure mode.** A script runtime error **fails the node** (sticky), never corrupting a stateful
  accumulator. `reset()` = construct-new-and-swap. Luau filters are numeric-scalar only: a string-valued input is
  coerced to `0.0` (`proc::detail::toDouble`), not rejected.
- **Filters are DATA.** `resources/filters/builtin_filters.luau` is ONE `.luau` resource returning a list of 12
  classes (none, absolute, scale, derivative, integral, moving_average, moving_rms, moving_variance, outlier_removal,
  samples_counter, binary_filter, time_since_previous). The app reads the QRC resource and hands the source string to
  `addBundledSource`. No user-override directory is scanned.

## Tests

Per-module runners are registered by `pj_add_test_runner` (`cmake/PjTests.cmake`), one ctest entry per case:

- `pj_scripting_tests` (ctest prefix `pj_scripting.`): the Luau and Python filter engines, `LuaSisoTransform`, time
  precision, the Luau sandbox and watchdog.
- `pj_scripting_object_tests`, `pj_scripting_native_tests`: the object-script engine and its summaries; the native
  operations (their headers are private to `src/`).
- `pj_scripting_python_tests` (desktop only): the on-demand Python lane and its sandbox.
- `marker_engine_test` (links `pj_scripting_core` only), `builtin_golden_test` (ctest `BuiltinGoldenTest`: the bundled
  filters' golden behaviour) and `filter_catalogue_test`: standalone executables.

Whole module: `./test.sh -R '^(pj_scripting|marker_engine_test|BuiltinGoldenTest|filter_catalogue_test)'`.

## Read deeper

| For | Read |
|---|---|
| The filter-class + ParameterForm schema contract, sandbox, time rebasing, Python filters | [`docs/FILTER_CLASS.md`](./docs/FILTER_CLASS.md) |
| Derived objects: where to start, document map | [`../docs/derived_objects/README.md`](../docs/derived_objects/README.md) |
| Scripts and their outputs: recipe and bundle, types and bindings, time and alignment (§5-7); TF (§12); the scripting languages, Luau VM reuse and the Python sandbox (§14); per-run costs and limits (§19) | [`../docs/derived_objects/ARCHITECTURE.md`](../docs/derived_objects/ARCHITECTURE.md) |
| The processor base this implements | `../pj_datastore/include/pj_datastore/data_processor.hpp` |
