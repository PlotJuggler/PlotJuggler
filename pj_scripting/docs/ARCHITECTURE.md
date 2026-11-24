# pj_scripting architecture

How the module works today. Per-header contracts live in the headers; measurements and platform findings are in
[TECHNICAL_NOTES.md](./TECHNICAL_NOTES.md); the filter-author contract is [FILTER_CLASS.md](./FILTER_CLASS.md); the
on-demand script design (recipes, caching, scheduling) is in
[../../docs/derived_objects/ARCHITECTURE.md](../../docs/derived_objects/ARCHITECTURE.md).

The module runs user scripts in four roles: **data filters** (per-sample Luau or Python classes inside the
`IncrementalEngine`), **marker rules** (a Luau script over whole series), **on-demand object scripts** (a stateless
Luau or Python evaluation over named inputs, for derived objects and series), and the native operations behind the last.
Two languages sit behind the `ScriptEngine` seam (`script_engine.h`): Luau (`makeLuauEngine`, the default and the
bundled filters) and embedded CPython (`makePythonEngine`, in-process through pybind11). `pj_datastore`'s
`PJ::proc::DataProcessor` stays free of Qt, Luau and Python; only this module links the language runtimes, and it never
touches Qt.

## Targets and the dependency rule

- `pj_scripting_core` (`luau_engine.cpp`, `marker_engine.cpp`): the Luau engine and the whole-series marker analysis.
  Depends on `pj_base`, `nlohmann_json` (public: `filter_class.h` exposes it), Luau and header-only pocketfft. It is
  also packaged alone for the SDK-only headless runner, so a rule runs identically in the GUI and headless;
  `marker_engine_test` links only it and guards that.
- `pj_scripting`: everything that needs the host: the data-processor adapters (`lua_siso_transform.cpp`,
  `lua_mimo_transform.cpp`), the filter catalogue, the on-demand engines (Luau and Python) and `src/native/`. Links
  `pj_scripting_core`, `pj_datastore` and `pj_tf` publicly, and Luau, tsl-robin-map and CPython/pybind11 privately.
  Builds without Python (the browser without `PJ_WASM_WITH_PYTHON`) compile the `*_unavailable.cpp` stubs.
- A `pj_scripting_core` source never includes `pj_datastore`, `pj_tf` or Qt, or the standalone package stops building.

## Data filters

A filter is a self-describing class: `id`, `name`, `version`, `output`, a typed `parameters` schema, `create()` and
`calculate()` ([FILTER_CLASS.md](./FILTER_CLASS.md)). The host reads the schema without instantiating the class, to fill
the `FilterCatalogue` and to generate the parameter form, so changing a filter is a data edit. The bundled set is one
multi-class Luau resource (`resources/filters/builtin_filters.luau`) handed to `FilterCatalogue::addBundledSource`; no
user directory is scanned. `makeProcessorFromSource` restores a layout's embedded source, and
`standaloneClassScript(id)` gives one class as a self-contained module with its parameter defaults filled in (the MCP
`add_filter` tool).

`LuaSisoTransform` (a `proc::DataProcessor`, wrapped by `ProcessorSisoAdapter`) and `LuaMimoTransform` (an
`IMIMOTransform`: N inputs joined by the engine node, M outputs) run an instance inside the `IncrementalEngine`.
`LuaSisoTransform` declares itself causal and stateful and not stream-safe, so the engine runs it eagerly and
recomputes from the start.

- **Time.** The script sees `t` in seconds since the first sample the node processed. The node subtracts that origin
  on the absolute int64 spine before the double cast (an epoch-magnitude double quantizes to about 256 ns, which
  wrecks every dt-based filter) and rebuilds output timestamps on the absolute spine. The origin is captured again
  after `reset()`.
- **Failure is sticky.** A script runtime error fails the node (`failed()`) and suppresses its output; it never
  corrupts a stateful accumulator. `reset()` rebuilds the instance (a fresh VM is the only guaranteed-clean reset).
- **Output kind.** `output` is `"double"`, `"int64"`, `"same"` (mirror: the input column's kind; float64 for text) or
  `"string"` (a text column). The engine instance knows its class's kind: a text class fills
  `FilterInstance::Result::text` / `MimoResult::texts` and refuses a number, a numeric class refuses a string that is
  not a number, both naming `output = "string"` in the error. int64 output clamps and a non-finite result becomes 0.
- **Input kind.** The seam passes each input as a `FilterInstance::InputValue`: a number (int64 and uint64 widen to a
  double) or a view of the text (`proc::detail::numberOrText`). Luau gets a string, Python a `str`. In Luau, text that
  is not a number fails arithmetic (Luau reads number-like text as a number), the engine's own runtime error reported
  through the sticky failure; a Python `str` keeps its own semantics. `DataProcessorService` adds the input's name
  to the error when the failing node reads a text column.
- **Luau sandbox** (`luau_sandbox_state.h`, limits in `BudgetLimits`). Filters run in-process on the GUI thread, so
  each VM is hardened: `luaL_sandbox` freezes the shared stdlib read-only after `os`, `coroutine`, `debug`, `setfenv`
  and `getfenv` are dropped (`setfenv` would let a script swap its environment back onto the frozen stdlib), then
  `luaL_sandboxthread` gives the VM a writable global table of its own over it: a script can keep state in globals but
  cannot mutate the stdlib or reach a sibling VM. A custom allocator caps the VM's bytes and an interrupt spends one
  fuel unit per safepoint. Each VM loads exactly one module, so `safeenv` stays on. The sticky `trip` (first reason a
  run must fail) is checked by the host after every protected call, because a script's `pcall` can swallow the error;
  keep that check on every new call path.
- **Method binding** reads `calculate` / `reset` by raw field, then raw lookup through a table-valued `__index`; a
  `__index` function is never invoked, since it would run script code outside any guard.
- **Python filters** (`python_engine.cpp`) take the GIL on the caller's thread and are unavailable in the browser
  (the on-demand lane is the only Python there): a call would hold the GIL on the main thread without a bound.

## Marker rules

`runMarkerScript` runs a Luau rule once over a caller-supplied `SeriesProvider` (random access, look back and forward;
`bandPower` for spectra) and returns `PlotMarker`s. Timestamps stay in nanoseconds as doubles with no rebasing. Native
output is capped by `max_output_bytes` apart from the VM cap, and `cancelled` is polled from the interpreter's
interrupt, so a loop that calls nothing native is still stoppable.

The rule helpers (`when`, its conditions `above` ... `changes` and its actions `backgroundColor`, `event`,
`verticalLine`) are Luau (`kPrelude` in `marker_engine.cpp`), run after the natives are bound and before
`luaL_sandbox`, so they freeze with the other globals. The prelude chunk receives two natives as arguments, so rules
never see them: a one-call region emitter that leaves the rule's own `startMarker` region open, and a `t, v` sample
read without `s:at`'s per-sample table, which was most of a scan's time. Every Luau call and loop iteration costs the
rule's fuel (`BudgetLimits::setup_fuel`, plus `series_sample_fuel` per sample of each series the rule opens, once per
series), so a built-in condition is data (a kind and its limits) tested inline in the
one scan loop, and the action's callback runs only where the result changes (TECHNICAL_NOTES "Marker rule fuel"). Color names are a copy of Qt's table
(`src/marker_color_names.inc`), since `pj_scripting_core` cannot link Qt; `pj_plotting`'s `MarkerColorNames.MatchQt`
fails when the pinned Qt's table drifts from it. To regenerate it, print `QColor::colorNames()` with their RGBA,
sorted by name.

## On-demand object scripts

`on_demand_script.h` routes a recipe's `language` to its compiler and runner so `pj_runtime` holds and runs a script
without knowing the language. Both languages bind the same native operations (`src/native/`: point-cloud, image, depth,
scene, annotation and voxel builders) and share the language-neutral run contract (`script_contract.h`: which objects
enter a script and at what stamp, binding returned values to outputs, errors) and one run's media state
(`script_media.h`).

- **Memory is charged before it is allocated.** A native operation reserves its output in a per-evaluation
  `MemoryBudget` (separate from the VM's cap) and polls the run's cancel check; an output keeps its reservation, so a
  retained result keeps counting. When a native operation fails on the budget, a full collection runs and the operation
  retries once, because the collector cannot see native bytes held by dead outputs.
- **Luau shares one frozen state per worker thread** (`ThreadState` in `object_script.cpp`). Each run gets a fresh
  thread on it with its own globals, `pj` table, seed and re-armed limits, and a full collection when it ends.
  Anything reachable from the shared state must stay frozen or unreachable: a new writable table or metatable a script
  can reach leaks between recipes. `ObjectScript.GlobalsDoNotLeakBetweenRuns` and its neighbours in
  `object_script_test.cpp` guard this; extend them when adding to the shared state.
- **Python runs under cooperative limits: a guardrail, not a sandbox.** Scripts are trusted input; CPython cannot be
  confined from inside the process. Both Python engines share one interpreter, never finalized. Every call into script
  code, a filter's teardown included, runs inside a `ScriptCallScope` (audit hook armed, allocations charged to a
  budget, a watchdog timing it); failures are sticky even when the script catches them. The host compiles and runs the
  code object inside the scope (`runCode`) because the strict hook refuses compiles. Filters use the *compatible*
  profile (real builtins, real modules behind the `math` / `cmath` / `statistics` allowlist, so older filters run
  unchanged); on-demand scripts use the *strict* profile (AST check, gated builtins, read-only module copies, `exec` and
  `compile` refused).
- **Lazy stdlib imports are preloaded.** An armed run cannot import a module that is not loaded yet (the hook refuses
  the file open), so the boot loads what the allowed modules import lazily on a reachable path (`kLazyDependencies` in
  `python_sandbox.cpp`, plus the warmed decimal fallback). Add a new such path there and to the browser's
  `cmake/wasm_python/stdlib_zip.py` roots; `pj_app`'s `tests/wasm_python_stdlib_probe.cpp` catches a miss.
- **No GUI-thread Python compile or GIL wait.** `compileOnDemand` defers (`deferOnDemandScriptPython` touches no
  interpreter); the first run compiles on the Python lane. `compileOnDemandScriptPython` compiles on the calling
  thread and may wait for the GIL, so only tests, probes and workers call it. A validate (`checkOnDemandScriptPython`)
  waits for the Python helper thread at most `kPythonValidateWaitMillis`, then answers unchecked.
- **Python in the browser** (`PJ_WASM_WITH_PYTHON`) is the on-demand lane only. `pj_app/src/main.cpp` calls
  `prestartPythonHelper()` at startup: a thread created while the browser's main thread waits on it never starts. The
  stdlib zip downloads on first use ([../../docs/WASM_DEPLOYMENT.md](../../docs/WASM_DEPLOYMENT.md)).
- **CPython is linked shared on every desktop platform** (`cpython/*:shared=True` in `conanfile.txt`); making it static
  breaks every stdlib C-extension import ([TECHNICAL_NOTES.md](./TECHNICAL_NOTES.md) "CPython shared").

## Locale

Luau's parser and `tonumber` read numbers with `strtod`, so under a decimal-comma `LC_NUMERIC` every literal like `0.5`
is a "Malformed number" and no script compiles. A `Q(Core)Application` adopts the user's locale for the C library, so
`pj_app`'s `main()` resets `LC_NUMERIC` right after constructing it; any other executable that runs scripts beside a Qt
application must do the same.

## Recipes

- **New filter:** add a class to the bundled Luau resource ([FILTER_CLASS.md](./FILTER_CLASS.md)) and a case to
  `builtin_golden_test.cpp`; no C++ change.
- **New native operation:** add it under `src/native/` (Luau-free), reserve its output before allocating, poll the
  cancel check, keep the input's stamp, then bind it in both `object_binder.cpp` and `python_object_binder.cpp`.
- **New script language:** implement `ScriptEngine` and the on-demand pair in `on_demand_script.h`.

## Known gaps

- The Python sandbox is a guardrail: a script can escape it by walking the object graph, and state left on a class a
  read-only module copy shares with its module (for example `statistics.NormalDist`) reaches later runs.
- Python filters run on the caller's thread with the GIL held, once per row, bounded only by `python_call_millis`.
- `LuaSisoTransform` and `LuaMimoTransform` each carry their own copy of the double-to-integer conversions.
