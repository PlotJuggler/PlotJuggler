# pj_scripting technical notes

What was observed, measured or reasoned behind the current code, with the conditions where they were recorded and the
change that would make it worth looking again. How the code works is in [ARCHITECTURE.md](./ARCHITECTURE.md). The
on-demand scripts' per-run costs and limits (binder cost per element, native operation cost per point, per-run setup,
allocation-cap overhead) are in `docs/derived_objects/ARCHITECTURE.md` "Reference numbers".

## CPython is linked shared

- **Observed:** with a static libpython, every stdlib C extension (`lib-dynload/*.so`: `math`, `zlib`, `_socket`, ...)
  failed to import with `undefined symbol: PyFloat_Type`. The extensions carry no copy of the interpreter and resolve
  `Py*` against the process when they are `dlopen`ed. `pj_app` links `--exclude-libs,ALL`, which marks static-archive
  symbols hidden, and ELF visibility only narrows, so `-rdynamic` cannot export them again. A shared libpython is not a
  static archive, so the flag does not touch it and the anti-interposition guard stays in force.
- **What the code does:** `cpython/*:shared=True` in `conanfile.txt` (on Windows the Conan recipe forces it anyway).
  `pj_app`'s `ExportedSymbolsGuard` test and `--selftest-python`, run against the packaged builds, pin both ends.
- **Dev caveat:** a user `LD_LIBRARY_PATH` naming another `libpython3.12.so.1.0` shadows the Conan one; the AppImage
  is immune (`AppRun.sh` puts `$APPDIR/usr/lib` first).
- **Re-test:** when the link flags of `pj_app` or the CPython packaging change.

## Time rebasing

- **Reasoning, with the arithmetic:** an absolute epoch in nanoseconds is about 2^60, so a double (53-bit mantissa)
  steps by 2^8 = 256 ns there and two samples 100 ns apart collapse to the same value. A double of seconds since the
  first sample steps by about 0.45 ps after an hour and stays at or under 1 ns up to about 97 days (2^23 s).
  `lua_time_precision_test.cpp` pins the 100 ns case through the node and shows the naive subtraction vanishing.
- **Consequence for the builtins:** quantities derived from a double time stay exact only inside that range
  ([FILTER_CLASS.md](./FILTER_CLASS.md) "Double-only engine limitations").

## Validate wait

- **What the code does:** `checkOnDemandScriptPython` waits `kPythonValidateWaitMillis` (100 ms) for its throwaway
  compile, then answers "unchecked" (the first run reports the error anyway).
- **Why 100 ms (reasoning from the figures below, not tuned):** an uncontended compile of a short script takes under
  1 ms natively and a few ms in the browser, so 100 ms is about 100 times that; it also covers 20 GIL switch intervals
  (5 ms each) behind a running pure-Python script, yet stays within what a check button may freeze the browser's main
  thread for. The interpreter's boot (0.13-0.24 s in the browser; machine not recorded) does not fit, so the first
  validate of a session boots it on the helper thread and is most likely unchecked.
- **Re-test:** if validate answers "unchecked" often after the boot, or the browser freeze is measured.

## Python allocation cap

- **Block table hash.** The cap keeps one record per live block in a table keyed by address shifted right by 4 (blocks
  are 16-byte aligned). The shift keeps neighbouring blocks in neighbouring buckets, which a loop of small allocations
  fills and empties in order; a mixing hash scattered them across cache lines and measured 2 to 4 times slower with a
  million live blocks. Homogeneous blocks of a few hundred bytes alias onto every n-th bucket and probe a few steps
  further: 1.3 times the bare loop. Machine not recorded. Re-test (and mix the high bits) if that aliasing shows in a
  profile.
- **Small blocks are always served.** CPython 3.12.7's `MAKE_FUNCTION` releases the code object twice when the function
  cannot be allocated (`Py_DECREF(codeobj)` then `goto error`; fixed in 3.14), so a refused small allocation crashes
  the interpreter. The cap therefore refuses only blocks of `kSmallestRefusedBlock` (1 KiB) or more, and a run of small
  objects is stopped at twice the cap by an asynchronous `MemoryError`. Re-test when the bundled CPython moves past the
  fix.
- **Single GIL.** The table's bookkeeping takes no lock under the GIL (the MEM and OBJ domains), which assumes one
  GIL: a script that creates a subinterpreter with its own GIL could break it. Accepted because scripts are trusted
  input. A free-threaded CPython has no such GIL; the per-domain flag in the table must then be false everywhere.

## Watchdog and helper threads are never joined

- **Observed (Windows):** joining a thread at process exit deadlocks under the loader lock. The Python watchdog and the
  helper thread are started on first use and never joined, so their code must stay in a library the executable links,
  never an unloadable plugin.
- **Observed (browser):** a thread created while the main thread waits on it never starts, because Emscripten spawns its
  worker only once the main thread yields. `pj_app/src/main.cpp` calls `prestartPythonHelper()` at startup.

## `bandPower` memory

- **Observed:** pocketfft's Bluestein path (lengths with a large prime factor) needs about 76 bytes per sample in
  float32 (about 152 in double). Above a memory budget (700 MB desktop, 350 MB in the browser;
  `kBandPowerMemoryBudgetBytes`) the series is truncated to the largest even 11-smooth length, which needs about 12
  bytes per sample and drops well under 1% of the samples. The rule is `toolbox_fft`'s `fftLength` in
  pj-official-plugins, so a rule matches the plugin.
- **Precision:** the FFT runs in float32, about 1e-7 relative accuracy. Re-test if a rule needs more.

## Native point-cloud operations

- **Compaction visits set bits only.** A per-record bit test mispredicts on a real scan and cost twice the predicate
  pass (`record_walk.h`; scan and machine not recorded).
- **Struct-view recycling (Python binder).** A list walk frees and allocates one view block per element, and the
  allocator round trip was about a third of a list-element read (`python_object_binder.cpp`; conditions not recorded).
  Freed blocks are reused within a run and emptied at its end.
- **`downsample` sorts points by cube,** the main cost of the operation ("Reference numbers" in the derived-objects
  architecture); a radix sort over the key bits the cloud's extent uses is the upgrade if it shows in a profile.
