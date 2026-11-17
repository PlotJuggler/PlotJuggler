# pj_scripting

The script engines of PJ4: **data filters** (self-describing Luau or Python classes run per sample inside the
`IncrementalEngine`), **marker rules** (Luau over whole series) and **on-demand object scripts** (stateless Luau or
Python evaluations for derived objects), behind the `ScriptEngine` seam (`include/pj_scripting/script_engine.h`).
`pj_datastore`'s `PJ::proc::DataProcessor` stays free of Qt, Luau and Python; only this module links the language
runtimes, and it never touches Qt. How it works: [docs/ARCHITECTURE.md](./docs/ARCHITECTURE.md).

## Invariants

- A `pj_scripting_core` source never includes `pj_datastore`, `pj_tf` or Qt: the standalone package (the SDK-only
  headless runner links it) stops building.
- The host checks the sticky `trip` after every protected Luau call, because a script's `pcall` can swallow the error;
  keep that check on any new call path or a runaway survives.
- Anything reachable from the on-demand Luau shared state (one frozen state per worker thread) stays frozen or
  unreachable: a writable table or metatable a script can reach leaks between recipes. Extend
  `ObjectScript.GlobalsDoNotLeakBetweenRuns` when adding to it.
- Python is a guardrail, not a sandbox: scripts are trusted input. Every call into script code runs inside a
  `ScriptCallScope`, and the host compiles outside it (the strict hook refuses compiles).
- A module an allowed Python module imports lazily must be preloaded at boot (`kLazyDependencies` in
  `python_sandbox.cpp`) and listed in the browser's `cmake/wasm_python/stdlib_zip.py` roots: an armed run cannot open
  the file, and the import fails.
- No GUI-thread Python compile or GIL wait: use `deferOnDemandScriptPython`. The browser needs
  `prestartPythonHelper()` at startup.
- CPython stays linked shared (`cpython/*:shared=True`); static breaks every stdlib C-extension import.
- The script's `t` is seconds since the filter node's first sample, rebased on the int64 spine before the double cast;
  an absolute epoch in a double quantizes to ~256 ns and breaks every dt-based filter.
- A native operation reserves its budget before allocating, polls the run's cancel check and keeps the input's stamp.
- Luau needs `LC_NUMERIC` "C": any executable that runs scripts beside a Qt application resets it (`pj_app`'s `main()`
  does), or every literal like `0.5` fails to compile.

## Key headers (`include/pj_scripting/`)

- `script_engine.h`, `python_engine.h`, `sandbox.h`: the engine seam and `FilterInstance`, the Python filter contract,
  `BudgetLimits`.
- `filter_class.h`, `filter_catalogue.h`: the filter schema (contract: [docs/FILTER_CLASS.md](./docs/FILTER_CLASS.md))
  and the Qt-free catalogue.
- `lua_siso_transform.h`, `lua_mimo_transform.h`: filters as `IncrementalEngine` nodes.
- `marker_engine.h`: `runMarkerScript`, `validateScript`.
- `on_demand_script.h`, `object_script.h`, `python_object_script.h`: the on-demand dispatch and the two engines, with
  the script-facing surface.
- `memory_budget.h`, `image_decoder.h`, `video_frame_source.h`, `object_summary.h`: the native budget ledger, the codec
  seams the session injects, the evaluation report summary.
- Private, in `src/`: `luau_sandbox_state.h`, `object_binder.h` / `python_object_binder.h`, `python_sandbox.h`,
  `script_contract.h`, `script_media.h`, and `native/` (the Luau-free operations both binders share).

## Tests

`./test.sh -R '^(pj_scripting|marker_engine_test|BuiltinGoldenTest|filter_catalogue_test)'`. Runners:
`pj_scripting_tests` (filters, sandbox, watchdog, time precision), `pj_scripting_object_tests`,
`pj_scripting_native_tests`, `pj_scripting_python_tests` (desktop only), and the standalone executables
`marker_engine_test` (links `pj_scripting_core` only), `builtin_golden_test` and `filter_catalogue_test`.

## Docs

- [docs/ARCHITECTURE.md](./docs/ARCHITECTURE.md): targets, filters, sandboxes, on-demand scripts, Python, locale,
  recipes, known gaps.
- [docs/TECHNICAL_NOTES.md](./docs/TECHNICAL_NOTES.md): measurements and platform findings behind the choices.
- [docs/FILTER_CLASS.md](./docs/FILTER_CLASS.md): the filter-author contract (module shape, parameter schema, time).
- [../docs/derived_objects/ARCHITECTURE.md](../docs/derived_objects/ARCHITECTURE.md): recipes, types and bindings,
  scripting languages, reference numbers.
