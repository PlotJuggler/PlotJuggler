<!-- SPDX-License-Identifier: MPL-2.0 -->

# Sanitizer lanes

PJ4's four build lanes are `none`, `asan` (ASan + UBSan), `tsan`
(ThreadSanitizer) and `msan` (MemorySanitizer).
The build and test scripts accept `--sanitize none|asan|tsan|msan`, or
`PJ_SANITIZE` as the environment equivalent; the command-line option wins.
The default is `none`.

| Diagnostic lane | Selector | Build tree | Coverage |
| --- | --- | --- | --- |
| ASan + UBSan | `asan` | `build/asan` | Memory access, lifetimes, leaks, undefined behavior and vector bounds; full test suite. |
| ThreadSanitizer (TSan) | `tsan` | `build/tsan` | The app and the whole configured test suite; plugins and SDK via the container. |
| MemorySanitizer (MSan) | `msan` | `build/msan` | The Qt-free `pj_datastore` closure only. |

## One builder image, two compilers

All diagnostic lanes run in the existing `pj4-appimage-builder` container
(Ubuntu 22.04). There is no sanitizer-specific image: the local builder already
contains Qt and the RelWithDebInfo Conan dependency closure, and every lane
reuses them. The instrumented lanes rebuild PJ4 and the pinned SDK from source.

| Lanes | Compiler |
| --- | --- |
| Ordinary builds, packaging, release | **GCC 11.4**, the image default and the compiler PJ4 ships with |
| `asan`, `tsan`, `msan` | **Clang 22** from LLVM's apt repository, provisioned per run |

The instrumented lanes do not use GCC 11:

- **ThreadSanitizer.** GCC 11's TSan runtime has no `pthread_cond_clockwait`
  interceptor (GCC PR 101978), and GCC 11's libstdc++ uses exactly that call for
  every `condition_variable::wait_for` on `steady_clock`. TSan never sees the
  wait release the mutex and reports a *double lock* at the next lock of it.
  Clang's runtime intercepts the call.
- **MemorySanitizer** exists only in Clang.
- **One compiler for all instrumentation.** Code instrumented for one compiler's
  sanitizer runtime cannot be loaded into a process running another's, so the
  app, the SDK sources and the plugins are all built with the same Clang.

Clang comes from LLVM's apt repository (apt.llvm.org), not from Ubuntu 22.04's
archive, whose newest is clang-15. Clang 15 fails twice: it cannot compile C++20
lambda captures of structured bindings, which the plugins use, and its shared
ThreadSanitizer runtime crashes at startup. The instrumented lanes need that
shared runtime (see [`PjSanitizers.cmake`](../cmake/PjSanitizers.cmake)): `pj_app`
links `--exclude-libs,ALL` and the SDK links every plugin and parser module with
`-z defs`, and neither works with Clang's default static runtime. Only the major
version is pinned (`PJ_SANITIZER_CLANG_VERSION`): apt.llvm.org publishes point
releases of that branch, so the exact build can move within it.

Clang compiles against the image's **GCC 11 libstdc++ headers**, the ones the
shipped build and the prebuilt Qt and Conan packages use, so instrumented binaries
link against the unchanged dependency closure. The Conan profile stays GCC 11 for
the same reason: only PJ4, the SDK sources and the plugin targets are compiled by
Clang.

The commands below run from the checkout root in a prepared builder shell,
with Qt available under `.qt/<PJ_QT_VERSION>/gcc_64` and the Conan cache
available. Test-only shells use `--entrypoint bash`: the default container
entrypoint always builds and packages an AppImage. See the
[AppImage build guide](../packaging/appimage/README.md#build--verify-in-docker)
for the existing builder and packaging setup. Keep each tree in its build
environment; Conan paths embedded by a container build need that container at
test time too.

CI runs the three lanes as one matrix job in `.github/workflows/sanitizers.yml`,
off the pull-request path: nightly, on every push to `main`, and on manual
dispatch. It uses the same `depot-ubuntu-22.04-4` runner as `linux-ci.yml`,
restoring its Qt and Conan caches; each lane has its own ccache key so
instrumented objects cannot seed another lane.

Modern host kernels can give sanitizer runtimes an incompatible address-space
layout, causing startup aborts before any test runs. The sanitizer CI job
includes this host-side setting; apply it on the Docker host when needed too:

```bash
sudo sysctl -w vm.mmap_rnd_bits=28
```

Clang is installed per run, **not baked into the builder image**: baking it
would force every developer through an image rebuild that re-bakes Qt and the
whole Conan closure. `build_in_docker.sh` adds apt.llvm.org and installs
`clang-<v>` with `libclang-rt-<v>-dev` (the sanitizer runtimes) and `llvm-<v>`
(for `llvm-symbolizer`, linked onto `PATH`) in both the plugin and the app
container of every instrumented lane (`PJ_SANITIZER_CLANG_VERSION`, default
`22`); the CI sanitizer job does the same on the runner. The symbolizer is not
optional: GCC's runtimes symbolize internally, Clang's only through an external
`llvm-symbolizer`. Without it every frame is a bare address, and no `leak:` or
`race:` suppression can match one. The `msan` lane additionally builds its instrumented libc++ into a
persistent volume, once per LLVM release: the build is stamped with the
compiler's version and redone when that changes.

## Ordinary build (`none`)

```bash
./build.sh --sanitize none
./test.sh --sanitize none --timeout 120
```

This uses `build/` without compiler sanitizers. `test.sh` supplies its headless
Qt environment but adds no sanitizer runtime defaults.

## ASan + UBSan

```bash
./build.sh --sanitize asan
./test.sh --sanitize asan --timeout 120
```

Always run this suite through `test.sh`: it selects `build/asan` and installs
the runtime environment and suppression paths. Bare CTest neither selects
that tree nor supplies those defaults. Extra arguments pass through to CTest,
for example `./test.sh --sanitize asan -R '^pj_scripting\.'`.

[`PjSanitizers.cmake`](../cmake/PjSanitizers.cmake) enables
`-fsanitize=address`, `-fsanitize=undefined`, frame pointers, and
`-fno-sanitize-recover=all`. UBSan findings terminate the process. The default
libstdc++ annotations, `_GLIBCXX_SANITIZE_STD_ALLOCATOR=1` and
`_GLIBCXX_SANITIZE_VECTOR=1`, detect accesses beyond a vector's size even when
they remain within its allocated capacity. These definitions must agree
across translation units exchanging vectors; `PJ_SANITIZE_CONTAINERS=OFF` is
the CMake escape hatch for diagnosed dependency-boundary false positives.
The ASan lane defaults to `PJ_DEBUG_INFO=lines` for symbolized reports.

The datastore's `sanitizer_selftest` runner includes death tests for a heap
use-after-free, signed integer overflow, and vector container-overflow. They
pass only when their child process dies with the expected diagnostic. A clean
child exit is a failure: it can mean the required instrumentation was lost.

## ThreadSanitizer

```bash
./build.sh --sanitize tsan
```

This builds **and runs** the app plus the whole configured test suite — 3048
tests at the time of writing — instrumented with `-fsanitize=thread`, in a
separate `build/tsan` tree. `--tsan` remains a deprecated alias.

It used to build three named targets with `PJ4_BUILD_APP=OFF`. That list covered
two of the seven PJ4 modules that spawn threads, and nothing in the plugins or
the SDK; a hand-maintained set stops covering new threaded code silently, the
moment someone forgets to extend it. The app was excluded only because the
container entrypoint always packages, which is a packaging constraint rather
than a ThreadSanitizer one.

The lane also runs through the container, where it exercises the plugins and the
SDK as well:

```bash
packaging/appimage/build_in_docker.sh --sanitize tsan --plugins-dir <plugins>
```

Both stages run even if the first reports something: a finding is the lane's
product, so aborting on it would leave the app suite unexercised. The recorded
status is re-raised at the end, and no AppImage is produced — a TSan tree is a
test lane.

### Two things TSan needs to run at all

**Address-space randomisation.** The TSan runtime reserves fixed shadow ranges and
aborts with `FATAL: ThreadSanitizer: unexpected memory mapping` before running a
single test when the kernel randomises mmap more widely than it expects. Current
kernels default `vm.mmap_rnd_bits` to 32, so on an untuned host *every* TSan
binary dies instantly. `build.sh` and `test.sh` disable randomisation for the
test process (`setarch -R`), which needs no host sysctl change.

**A relaxed seccomp profile in Docker.** The syscall that needs (`personality`)
is denied by Docker's default profile, so `build_in_docker.sh` passes
`--security-opt seccomp=unconfined` for this lane only.

### Suppressions are not optional here

Qt, GLib and FFmpeg are consumed prebuilt. TSan sees happens-before edges from
pthread primitives even in uninstrumented libraries, but **not** ordering
established by their lock-free atomics, so it reports races that do not exist.
Measured on the full suite with Clang 22, `halt_on_error=0` so that every report
counts, not only the first per test process:

| Suppressions | Tests failing | Reports |
|---|---|---|
| `tsan.supp` without `called_from_lib:libQt6Core.so` | 391 | 390, 385 of them inside `libQt6Core` |
| `cmake/sanitizers/tsan.supp` | 1 | 1: the SDK's `JobControl::armWatchdog` race, a genuine bug fixed upstream |

GCC 11's runtime added double-lock reports on every timed condition-variable
wait, because it has no `pthread_cond_clockwait` interceptor; Clang 22 reports
none. Where the hand-off is a join on a worker PJ4 owns, the code tells TSan
about it instead of suppressing: `FileLoader` calls `__tsan_release` as its
ingest worker's last access to shared state and `__tsan_acquire` after each
`QThread::wait()`, compiled only under TSan.

Read `cmake/sanitizers/tsan.supp` before adding an entry. Each `called_from_lib`
must resolve to exactly ONE loaded library, and a pattern that matches several
is fatal — the binary exits 66, which breaks `gtest_discover_tests` and silently
takes the entire app stage down while the lane still looks like a normal run
with findings.

Two limits of those library entries matter:

- **They do not cover hand-offs through Qt.** A `QtConcurrent::run` task or a
  queued call is built on one thread and first touched on another, ordered by a
  `QMutex` inside `libQt6Core` that TSan cannot see, while the racing frames are
  Qt header templates and lambdas compiled into PJ4. Those are suppressed per
  site at the end of `tsan.supp`, each only after an audit of its report. A
  `race:` entry also hides a genuine race inside the function it names, so
  re-audit a site when its threading changes. A Qt built with
  `-fsanitize=thread` would make all of them unnecessary: its headers already
  carry the `__tsan_mutex_*` annotations, active only in an instrumented build.
- **`called_from_lib:libQt6Core.so` also bypasses TSan's deferred signal
  delivery.** TSan runs an asynchronous signal handler when the thread next
  passes through one of its interceptors, and calls from an ignored library skip
  them. `QProcess` learns that a child whose `exec` failed has exited through its
  `SIGCHLD` handler, so that path blocks forever; `plugin_check_runner_test`
  skips the case under TSan. Dropping the entry is not an alternative without an
  instrumented Qt: the suite then fails 391 tests, with 385 of its 390 reports
  inside `libQt6Core`.

The build script's test invocation sets `halt_on_error=1` (stop at the first
race), `history_size=4` (retain more access history for the report) and the
suppression file. `test.sh` applies the same defaults for the `tsan` lane, so
rerunning a subset needs only a filter:

```bash
./test.sh --sanitize tsan --timeout 120 -R '^pj_datastore_tests\.'
```

Setting `TSAN_OPTIONS` yourself replaces those defaults wholesale, suppression
file included — pass the full string when you do.

The separately registered `sanitizer_race_selftest` deliberately races. CTest
passes it only when the output carries a `ThreadSanitizer: data race` report
(`PASS_REGULAR_EXPRESSION`), with
`TSAN_OPTIONS=halt_on_error=1:exitcode=66:abort_on_error=0`. An uninstrumented
clean exit fails, and so does a runtime startup abort that never reached the
race — the case a plain `WILL_FAIL` would have accepted. It is registered only when TSan
is on, and is the lane's proof that instrumentation is live rather than merely
linked. To exercise it explicitly:

```bash
cmake --build build/tsan --target sanitizer_race_selftest
./test.sh --sanitize tsan -R '^sanitizer_race_selftest$'
```

## MemorySanitizer

```bash
./build.sh --sanitize msan
packaging/appimage/build_in_docker.sh --sanitize msan
```

Covers the **Qt-free `pj_datastore` closure only**, and that is a hard limit
rather than a starting point.

MSan reports reads of memory it never observed being **written**. Uninstrumented
code that writes a buffer leaves the shadow poisoned, so the report fires later
inside *instrumented* code reading a value that was initialised correctly. That
is the opposite of ASan, where uninstrumented code is merely unchecked, and worse
than TSan, whose noise is filterable: **MSan registers no runtime suppressions
option at all**. Its only filter is a compile-time ignorelist over code you
compile. So every writer in the process must be instrumented, which rules out
prebuilt Qt and the GL stack that `dlopen`s a GPU driver. The root
`CMakeLists.txt` returns after `pj_datastore` for this lane.

Beyond the Clang every instrumented lane uses, it needs two things no other
lane does:

- **An instrumented libc++.** The distro package is an ordinary build; linking it
  makes every `std::` read report as uninitialised. `scripts/build_msan_libcxx.sh`
  builds one and fails if the result carries no `__msan_` symbols.
- **A dedicated Conan cache.** Sanitizer flags do not enter `package_id`, and
  `compiler.libcxx` is dropped for C-only packages, so a shared cache cannot
  distinguish an instrumented package from an ordinary one — in either direction.

It also needs PIE and constrained address-space randomisation, for the same
reason TSan does; see above. No AppImage is produced: this is a test lane.

`sanitizer_msan_selftest` deliberately reads uninitialised heap memory. CTest
passes it only on a `MemorySanitizer: use-of-uninitialized-value` report, so a
clean exit and a startup abort are both failures. Its
first version passed vacuously because an inline-asm memory clobber unpoisoned
the buffer it was meant to read — which is exactly the failure it exists to
catch, so do not reintroduce an optimiser escape there.

## Runtime environment and suppressions

For either `asan` or `tsan`, `test.sh` supplies these defaults **only when the
variable is unset**. Caller values, including an explicitly empty value, are
preserved. These defaults are defined in `test.sh`; CI invokes the script
instead of duplicating them. The `none` lane adds no sanitizer defaults and
does not clear an inherited environment.

| Variable | Default | Reason |
| --- | --- | --- |
| `ASAN_OPTIONS` | `abort_on_error=1:print_stacktrace=1:detect_leaks=1` | Fail on memory errors, request traces and enable leak checking. No `suppressions=` here: ASan parses that file with its own grammar and aborts on `leak:` entries. |
| `LSAN_OPTIONS` | `suppressions=<absolute path to cmake/sanitizers/lsan.supp>` | Point LeakSanitizer, the only runtime that reads `leak:` entries, at the reviewed file. |
| `UBSAN_OPTIONS` | `print_stacktrace=1` | Show where the unrecoverable undefined behavior occurred. |
| `PYTHONMALLOC` | `malloc` | Bypass CPython's obmalloc pools so ASan can observe individual allocations instead of a whole pool as one block. |

The ASan/LSan/UBSan variables are also exported for `--sanitize tsan`, but
they do not enable those sanitizers in a TSan binary. TSan uses its own
`TSAN_OPTIONS` as described above.

For all selectors, `test.sh` sets `QT_QPA_PLATFORM=offscreen` to avoid visible
test windows, clears `QT_IM_MODULE` to avoid an incompatible system input
plugin, and points `QT_PLUGIN_PATH` at the checkout's pinned Qt plugins.
An explicit `--test-dir DIR` overrides the selected directory, with a warning
if the paths differ; the runtime defaults still follow `--sanitize`.

The authoritative policy is in
[`cmake/sanitizers/README.md`](../cmake/sanitizers/README.md), with the shared
file at [`lsan.supp`](../cmake/sanitizers/lsan.supp). Its two entries cover the
deliberately retained CPython interpreter and released-GIL guards (a documented
lifetime decision matched by function, not a blanket Python suppression), and
the `QOpenGLContext` Qt's `QOpenGLWidget` drops when a context cannot be created
on the offscreen platform (matched on the context's constructor, with no PJ4
frame in the stack).

Add a suppression only after an observed, symbolized leak report supports a
narrow match. Every pattern must have an immediately preceding comment with
its reason and exactly one category: `ours-by-design` for an intentional PJ4
lifetime, or `third-party` identifying the dependency and its lifetime issue.
Do not suppress fixable bugs, speculate about leaks, or add blanket Python/Qt
matches. Entries must be compatible with every runtime reading the shared
file and revisited when the relevant lifetime changes.

There are **no UBSan suppressions**: `-fno-sanitize-recover=all` makes the
handlers unrecoverable, so a runtime suppression file would promise an escape
hatch that does not work. Fix the undefined behavior.

## Instrumented AppImages

`packaging/appimage/build_in_docker.sh --sanitize asan --plugins-dir <plugins>`
produces `PlotJuggler-<version>-asan-<arch>.AppImage`: the app, the SDK built
from source and every bundled plugin instrumented, all compiled by the lane's
Clang, with Clang's shared sanitizer runtime (`libclang_rt.asan-<arch>.so`, which
also carries UBSan) and `libqoffscreen.so` bundled so it can run headless.

The executable links that runtime as a shared library (`-shared-libsan`) on
purpose. `pj_app` links `--exclude-libs,ALL`, and with Clang's default static
runtime that hides the sanitizer symbols every instrumented plugin needs: each
plugin then fails to load with `undefined symbol: __asan_report_load4`.

`AppRun.sh` sets `ASAN_OPTIONS=detect_leaks=0` for this artifact, keyed off a
payload marker so renaming the file keeps the default. That is deliberate: Qt and
plugin teardown leaks would bury a use-after-free report. Leak detection belongs
to the test lane, which runs through `test.sh`.

The `tsan` and `msan` lanes produce **no** AppImage. Both are test lanes, and an
instrumented binary from either needs its runtime environment tuned before it
will start, so shipping one would hand users an artifact that aborts on launch.


## Plugins in the ASan lane

`--sanitize asan --plugins-dir <source repo>` instruments the plugins too, and it is
not optional. AddressSanitizer's checks are compile-time instrumentation: an
instrumented app loading UNinstrumented plugins reports **nothing** for a
use-after-free inside a plugin, while the reverse aborts at startup with "ASan
runtime does not come first". A half-instrumented bundle looks healthy and is blind.

The wrapper therefore forwards `PJ_SANITIZE` into the plugin container, where
`build.sh --asan` instruments the plugin targets and `scripts/ensure_core.sh`
instruments `plotjuggler_sdk` through Conan's `tools.build:*` configuration. If the
plugins checkout does not understand `--asan`, the build **fails with exit 3**
rather than quietly producing uninstrumented plugins.

Each lane owns its Conan and ccache volumes (`pj4-appimage-plugin-conan-asan`,
`pj4-appimage-plugin-ccache-asan`) and its own staging directory
(`build/asan/plugins-built`). This is not tidiness: Conan's `tools.build:*` settings
do **not** participate in `package_id`, so an instrumented `plotjuggler_sdk` carries
the same id as the Release one and a shared cache would let one lane serve the
other's binaries with no error at all.

`--fresh` is lane-scoped for the same reason — `--sanitize asan --fresh` drops only
the instrumented volumes and leaves the Release cache intact. Use it the first time
you build the ASan plugin lane, or whenever a `--sdk-dir` run has left a custom SDK
in that cache (the build warns when it detects one).

## What is not covered

Qt and the prebuilt Conan dependency closure are **uninstrumented**, so errors
inside them are invisible to compiler sanitizer instrumentation. The SDK is
the deliberate exception: instrumented lanes build its pinned sources along
with PJ4. Passing these lanes does not establish that dependency internals
are clean.

The vendored **Qt-Advanced-Docking** library is compiled **without**
instrumentation in every sanitizer lane by default (`PJ_SANITIZE_QT_ADS=OFF`).
Its dock-manager teardown trips UBSan's vptr check inside its own destructors,
and it is third-party code we do not patch. Pass `-DPJ_SANITIZE_QT_ADS=ON` to
instrument it anyway.

TSan produces **no AppImage**. `build_in_docker.sh --sanitize tsan` runs the lane
across the plugins, the SDK and PJ4 and then stops: an instrumented binary needs
its runtime environment tuned before it will even start, so packaging one as a
release artifact would hand users something that aborts on launch. The app
itself IS built and its GUI tests do run, offscreen, under instrumentation.

**MemorySanitizer covers the Qt-free `pj_datastore` closure only** (see its
section above). The rest of the app would need an instrumented Qt, CPython and
dependency closure, which this single-image design does not build.
