# SPDX-License-Identifier: MPL-2.0

# Sanitizer test runs and suppression policy

Run `./test.sh --sanitize asan` for `build/asan`, `--sanitize tsan` for
`build/tsan`, or `--sanitize none` for `build/`. `PJ_SANITIZE` supplies the default
lane when the flag is absent; otherwise the default is `none`. The flag wins over
the environment, and both `--sanitize LANE` and `--sanitize=LANE` are accepted.

Other arguments go to CTest. An explicit `--test-dir DIR` (or `--test-dir=DIR`)
wins over the lane's build tree: the lane then selects only the runtime
environment. The script warns with both paths when the directories differ after
path normalization. It passes `--test-dir` once; the last explicit directory wins
if the caller repeats it. For example:

```bash
./test.sh --sanitize asan -R python
PJ_SANITIZE=tsan ./test.sh -R engine_concurrency
./test.sh --sanitize asan --test-dir /path/to/instrumented-build
```

For either instrumented lane, `test.sh` exports the following defaults only when
the caller has not set the variable. Existing values, including empty ones, are
preserved. The `none` lane adds no sanitizer runtime defaults.

| Variable | Default |
| --- | --- |
| `ASAN_OPTIONS` | `abort_on_error=1:print_stacktrace=1:detect_leaks=1` |
| `LSAN_OPTIONS` | `suppressions=<absolute path to lsan.supp>` |
| `UBSAN_OPTIONS` | `print_stacktrace=1` |
| `PYTHONMALLOC` | `malloc` |

`PYTHONMALLOC=malloc` bypasses CPython's obmalloc pools. Otherwise ASan sees a pool
as one block and misses errors between individual allocations inside it, reducing
coverage of the embedded interpreter.

## Leak suppressions

`lsan.supp` suppresses two leaks:

- The deliberate interpreter lifetime in `pj_scripting/src/python_engine.cpp`.
  `ensureInterpreter()` leaks its `py::scoped_interpreter` and
  `py::gil_scoped_release` guards and never finalizes Python. Finalization during
  static destruction is fragile, and the released GIL lets each caller acquire it
  on its own thread. The entry was added after an observed, symbolized report
  named `ensureInterpreter` as the allocating frame.
- Qt's `QOpenGLWidget` context-failure path. On a platform without OpenGL (the
  offscreen platform the tests use), the widget drops the `QOpenGLContext` it
  allocated during its first resize; the context and its private reference each
  other, so only indirect leaks are reported, with no PJ4 frame. Matched on
  `QOpenGLContext::QOpenGLContext`, the private's allocating frame. A widget or
  context PJ4 itself leaks is still reported through its own direct-leak stack.

Stacks through prebuilt Qt are short under the default fast unwinder, because Qt
is built without frame pointers. To find the frame an entry should match, rerun
the test with `ASAN_OPTIONS=...:fast_unwind_on_malloc=0`; then match a frame that
the default unwinder also records, as the two entries above do.

Every suppression line must have a comment line **directly above it**, stating
the reason and exactly one of these categories:

- `ours-by-design`: an intentional allocation with a documented lifetime decision
  in our code. This is a legitimate category here, including the interpreter
  guards described above.
- `third-party`: an observed leak in a dependency, with the dependency and reason
  for suppression identified.

Use the narrowest match supported by the observed allocation stack. Do not add
speculative entries, blanket Python or Qt matches, or suppressions for bugs that
should be fixed. The launcher supplies this path through `LSAN_OPTIONS` only:
AddressSanitizer parses a `suppressions=` file with its own grammar, rejects
`leak:` entries and aborts every instrumented process, so the file must never be
added to `ASAN_OPTIONS`. Revisit an entry when its code or dependency lifetime
changes.

## No UBSan suppressions

UBSan suppressions are deliberately absent. The sanitizer build uses
`-fno-sanitize-recover=all`, which makes UBSan handlers unrecoverable. A runtime
suppression file would advertise an escape hatch that does not work.
`UBSAN_OPTIONS` therefore defaults only to `print_stacktrace=1`; undefined
behavior reports must be fixed.
