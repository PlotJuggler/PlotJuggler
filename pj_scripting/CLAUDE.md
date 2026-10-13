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
- **Python** — `makePythonEngine()`, `src/python_engine.cpp`. Embedded CPython via
  pybind11, IN-PROCESS (not out-of-process). Reached from the Transform Editor's
  `Lua | Python` switch; `DataProcessorService` routes to it by sniffing the
  `# pj-script: python` header on the source. Imports are restricted — see below.

The data-processor base (`PJ::proc::DataProcessor`, owned by `pj_datastore`) stays
Qt-free, Luau-free **and Python-free**: only `pj_scripting` links either runtime.
Depends on `pj_base` + `pj_datastore` (public) + Luau + CPython/pybind11 (private).
Licensed MPL-2.0.

## Key headers

- `script_engine.h` — the `ScriptEngine` seam + `FilterInstance` (one VM per
  instance) + `makeLuauEngine(BudgetLimits)`.
- `python_engine.h` — `makePythonEngine()` and the Python filter-module contract
  (a top-level class `T` with `id` and a static `create(params)`).
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
  the fast path. A "tripped" flag is checked by the host **even if the script
  `pcall`-swallows the error**, so a runaway can never silently survive.
- **Python imports are an allowlist, and it is a guardrail — NOT a sandbox.**
  `python_engine.cpp` hands each filter module a namespace whose `__builtins__` has
  `__import__` swapped for a gate over `kAllowedImports` (`math`, `cmath`,
  `statistics`); anything else is rejected as *"module 'X' is not available in Data
  Processors"*, so the limit reads as policy rather than as breakage. Only the root
  package is matched, and the gate governs **only what the filter source imports** —
  an allowed module's own body runs under its own globals with unrestricted
  builtins, which is why `statistics` still reaches `_statistics`, `random` and the
  rest. What this does NOT do: CPython cannot be sandboxed from within (an
  object-graph walk reaches past any namespace), `open` lives in builtins rather
  than behind an import, and there is no memory cap or watchdog on the Python side.
  Filter sources are trusted input. The Luau backend above is the hardened one.
- **CPython is linked SHARED on every platform** (`conanfile.txt`
  `cpython/*:shared=True`), and this is load-bearing, not a preference. The stdlib's
  C extensions (`lib-dynload/*.so` — `math`, `zlib`, …) carry no copy of the
  interpreter and resolve `Py*` at dlopen time against the process; a static
  libpython leaves those symbols out of `pj_app`'s `.dynsym` and every such import
  dies with `undefined symbol: PyFloat_Type`. `-rdynamic` cannot rescue it —
  `pj_app` links `--exclude-libs,ALL`, which marks static-archive symbols
  `STV_HIDDEN`, and ELF visibility only ever narrows. `pj_app`'s
  `ExportedSymbolsGuard` test pins that flag in place; `--selftest-python` (run
  against the packaged AppImage in CI) pins the runtime end.
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
