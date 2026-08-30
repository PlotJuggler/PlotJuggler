# Building PJ4 with pixi instead of Conan — exploration findings

**Status: exploration** (branch `feat/pixi-build`). The Conan path (`./build.sh`)
remains the canonical build. This documents what a pixi/conda-forge build looks
like, what it buys us, and what it costs.

Context: this is Phase 2 territory of the Conan→pixi migration whose Phase 1
(SDK conda packaging, published to prefix.dev) already shipped in
`plotjuggler_sdk`.

## TL;DR

A full PJ4 build driven by `pixi run build` works, with:

- **all but 4 dependencies** taken prebuilt from conda-forge — including
  **Qt 6.11.1 exactly** (`qt6-main`), which also replaces the manual ~1GB
  `install_qt6.sh` step. One command bootstraps the entire toolchain
  (compiler included).
- the 4 gaps (**luau, mcap C++, cloudini, nanoarrow C**) packaged as local
  `rattler-build` recipes under `recipes/`, built into `local-channel/`. In a
  real migration these would be published once to the existing prefix.dev
  channel (same pipeline as `plotjuggler_sdk`).
- **3 small CMake portability fixes** (all no-ops under Conan — see below).
- **zero changes to module source code.**

## What maps 1:1 to conda-forge

| conanfile.txt | conda-forge | note |
|---|---|---|
| (install_qt6.sh 6.11.1) | `qt6-main 6.11.1` | exact version; all needed components (Svg, SvgWidgets, OpenGLWidgets, UiTools, Concurrent, Test, GuiPrivate headers) |
| fmt/12.2.0 | `fmt 12.2` | compiled lib instead of header-only — same `fmt::fmt` target |
| tsl-robin-map/1.4.0 | `tsl_robin_map 1.4.0` | underscore package name; installs the standard `tsl-robin-map` CMake config |
| gtest/1.18.0 | `gtest 1.17.0` | |
| glm/1.0.3 | `glm 1.0.1` | |
| benchmark/1.9.5 | `benchmark 1.9.5` | |
| nlohmann_json/3.12.0 | `nlohmann_json 3.12.0` | |
| cpython/3.12.7 + pybind11/2.13.6 | `python 3.12` + `pybind11 2.13.6` | see "cpython::embed" below |
| zstd, fast_float, libpng, libjpeg-turbo | same names (lz4 is `lz4-c`) | libjpeg-turbo target naming differs, see fixes |
| libarchive/3.8.7 (static) | `libarchive 3.8.8` (shared) | conda has no static build; fine at build time, adds a runtime .so |
| ffmpeg/9.0.1 (custom LGPL build) | local `ffmpeg 9.0.1` `pj_lgpl_lean_*` (`recipes/ffmpeg`) | same trimmed decode-only configure line as Conan. Interim pin was conda-forge `ffmpeg =*=lgpl_*` — license-clean but feature-complete, and the default solve picks `gpl_*` |
| backward-cpp/1.6 (+elfutils) | `backward-cpp 1.6` + `elfutils` | `dw` backend resolves in-env, same as Conan |
| draco/1.5.7 | `draco 1.5.7` | Conan pins the exact version its source-built assimp requires; prebuilt conda packages don't share that graph constraint |
| assimp/6.0.5 (4 importers) | `assimp 6.0.5` (all importers) | same major on both paths; conda ships the full importer set — see risks |
| OpenGL headers (system) | `libgl-devel`, `libopengl-devel`, `libegl-devel` | replaces the system mesa dev packages |
| (system libva/libdrm for VAAPI) | pulled in transitively by conda ffmpeg | no more `apt-get install libva-dev libdrm-dev` |

## The local recipes (`recipes/`): 4 gaps + the LGPL-lean ffmpeg

The first four don't exist on conda-forge; each recipe replicates the exact
imported targets the Conan packages provided (verified against the
Conan-generated configs in the main checkout's `build/`). `ffmpeg` is
different — it *does* exist upstream, but the local rebuild is a licensing/
surface-control decision (see the row note and "Semantic deltas" below):

| package | recipe approach | provides |
|---|---|---|
| `luau` 0.700 | upstream has no CMake install → build static libs, hand-install, hand-written `LuauConfig.cmake` | `Luau::VM`(→Common,m), `Luau::Compiler`(→Ast), `Luau::Ast`(→Common), `Luau::CodeGen`(→VM) |
| `libmcap` 2.1.1 | header-only copy + hand-written `mcap-config.cmake` (conda-forge `mcap` is the *Python* wrapper) | `mcap::mcap` INTERFACE carrying lz4+zstd |
| `cloudini` 1.2.2 | standalone static build (vendored-deps path, tools/benchmarks/tests off) + hand-written config | `cloudini::cloudini` carrying lz4, zstd, m, pthread |
| `libnanoarrow` 0.7.0 | upstream CMake with `-DNANOARROW_IPC=ON`; upstream installs its own config; ships `libflatccrt.a` alongside (pj_datastore links it by name) | `nanoarrow::nanoarrow_static`, `nanoarrow::nanoarrow_ipc_static` |
| `ffmpeg` 8.1 (linux-64 + win-64) | decode-only LGPL-only rebuild of conanfile.txt's trimmed option block (`--disable-autodetect`, VAAPI+libdrm, dav1d); distinctive `pj_lgpl_lean_*` build string; embedded package test compiles against it and asserts `avcodec_license()` says LGPL and the configure line has no `--enable-gpl`/`--enable-nonfree` | standard `libav{codec,format,util}.pc` + `libswscale.pc` for `PjScene2DFfmpeg.cmake`'s pkg-config fallback (no CMake config shim needed) |

Each gap recipe ships a committed, platform-neutral `*Config.cmake` (using
`find_library`, so one config body resolves both `libFoo.a` on Linux and
`Foo.lib` on Windows) plus `if: unix` / `if: win` install branches. Rebuild the
whole channel with:

```bash
pixi exec --spec "rattler-build>=0.66" \
  rattler-build build --recipe-dir recipes/ -m recipes/variants.yaml \
    -c conda-forge --output-dir local-channel
```

It must be `pixi exec` (an ephemeral env), **not** `pixi run`: the default env
depends on these packages, and any `pixi run`/`pixi install` solves the
default env first — which fails until the channel it needs already exists. The
`-m recipes/variants.yaml` is load-bearing on Linux — see the sysroot gotcha
below.

Long-term these belong on the PlotJuggler prefix.dev channel next to
`plotjuggler_sdk` — then `local-channel/` disappears and `pixi.toml` points at
the hosted channel.

## CMake changes required (all backward-compatible with Conan)

1. **`pj_datastore/CMakeLists.txt`** — the flatccrt link-injection used
   `target_link_libraries` on the found nanoarrow IPC target; upstream's config
   exposes that name as an ALIAS (rejected by CMake). Resolve
   `ALIASED_TARGET` first. No-op for Conan targets (not aliases).
2. **`pj_scene2D/core/CMakeLists.txt`** — `libjpeg-turbo::libjpeg-turbo` is
   Conan's umbrella name; upstream's config exports `::jpeg` / `::turbojpeg`.
   Added a bridge target (module only uses the turbojpeg API). No-op when the
   umbrella target already exists.
3. **`pj_scripting/CMakeLists.txt`** — `cpython::embed` is Conan-only. When
   absent, resolve `find_package(Python COMPONENTS Development.Embed)` and
   bridge `Python::Python` to the same name. Also fixed a latent bug in the
   Python-home fallback (include-dir needs two parent hops, not one — never
   exercised under Conan because `cpython_PACKAGE_FOLDER_*` always wins).

## Semantic deltas to be aware of

These are the places where "prebuilt conda binary" differs from "our tuned
Conan source build":

- **FFmpeg feature surface — resolved by `recipes/ffmpeg`.** Conan built a
  minimal LGPL decoder set (h264, hevc, mjpeg, av1/dav1d; file protocol only;
  no encoders/muxers). conda-forge `lgpl_*` ffmpeg is LGPL-license-clean but
  *feature-complete* (all LGPL decoders, encoders, network protocols, ~12 MB
  + deps) — that was the interim pin. The local channel now rebuilds FFmpeg
  from source with the same trimmed configure line as Conan (3.9 MB package),
  so the pixi env ships the identical decode-only surface and is
  release-grade on the licensing axis.
- **libarchive** is shared in conda (Conan build was static) — one more
  runtime `.so` to ship. Its conda build string says `gpl_*`: bsdtar/bsdcpio
  CLI tools are GPL, but **the library itself is BSD** — linking `libarchive.so`
  from an MPL app is fine.
- **assimp importer set**: both paths are on assimp 6.0.5, but conda-forge ships
  it with all importers enabled (bigger lib, wider parse surface on untrusted
  mesh files) where Conan builds only the four PJ4 uses. If that matters we'd
  pin/patch a leaner build on our own channel.
- **Embedded Python is shared** (`libpython3.12.so`) instead of Conan's static
  libpython: `pj_app` now needs the env (or bundled lib) at runtime.
  `PJ_PYTHON_HOME` bakes to the pixi env prefix, which is correct for dev;
  packaging would bundle a python runtime explicitly.
- **fmt** is a compiled lib, not header-only. Same API; negligible practical
  difference.
- **Toolchain**: the build uses conda's `cxx-compiler` (GCC 14.3 with conda
  sysroot) rather than the system compiler. Fully hermetic — but ccache
  entries don't transfer between the two toolchains, so the first pixi build
  is cold.

## How to build

```bash
cd .worktrees/pixi-build           # or wherever this branch is checked out
# 1. Build the local-channel packages (once; re-run only when a recipe changes):
pixi exec --spec "rattler-build>=0.66" \
  rattler-build build --recipe-dir recipes/ -m recipes/variants.yaml \
    -c conda-forge --output-dir local-channel
# 2. Build + test (pixi solves the env — incl. Qt — against local-channel):
pixi run build                     # configures + builds into build-pixi/
pixi run test                      # ctest over build-pixi
```

Step 1 uses `pixi exec` (not `pixi run`) so it runs before the default env can
be solved — the local channel it produces is exactly what lets that solve
succeed (the default env pulls luau / mcap / cloudini / nanoarrow from there).

## Results (linux-64, verified)

- Environment solve + install: minutes on first run (~2.1 GB env, cached in `.pixi/`).
- Local channel: all 4 gap packages build via `rattler-build` and their archives
  are verified free of `__isoc23_*` refs (see the sysroot gotcha below).
- Configure: clean; FFmpeg found via the existing pkg-config fallback path.
- Build: **1144/1144 targets, `pj_app` binary produced.**
- Tests: **245/246 pass.** The single failure — `extension_manager_test`
  (`ConstructionReflectsStagedUpdateInOnePass`) — is a **pre-existing
  environmental host failure** (a `/tmp` backup-rename step that fails
  deterministically on this machine regardless of the build system; it fails
  the same way under the Conan build). So this is **parity with Conan.**

**win-64 (verified in CI): the whole app compiles and 243/246 tests pass** —
further than the Conan Windows CI, whose Build step fails. See the CI section for
the 3 Windows-specific test failures and why the leg is a `continue-on-error`
tracker.

## The glibc sysroot gotcha (rattler-build)

The single hardest part. Building the gap packages naively produced archives
that referenced `__isoc23_strtol` / `__isoc23_strtoull` — glibc ≥2.38 symbols
that don't exist in conda's target sysroot (glibc 2.28), so linking PJ4 against
them failed with a wall of `undefined reference to __isoc23_*`.

Root cause: without an explicit stdlib pin, rattler-build's compiler fell back
to the **build host's** system glibc headers (which, on a modern dev box or
`ubuntu-latest`, emit the `__isoc23_*` redirects). The fix is the modern
conda-forge idiom: add `${{ stdlib('c') }}` to each recipe's build requirements
and declare `c_stdlib: sysroot` / `c_stdlib_version: "2.28"` in a variant config
(`recipes/variants.yaml`, passed via `-m`). That injects the correct `--sysroot`
into `$CXXFLAGS` so the archives target glibc 2.28 and stay portable. It is
gated `if: linux` in the recipes — Windows/MSVC has no glibc.

This bug is invisible on conda-forge's own CI (old CentOS/manylinux build hosts
have no `__isoc23_*`), so it only bites when you build the channel on a
bleeding-edge host — exactly what a dev box and `ubuntu-latest` are.

## The `SUBDIR` gotcha (rattler-build × autotools/make)

rattler-build exports `SUBDIR=linux-64` (the conda *platform* subdir) into the
build script's environment. GNU make imports environment variables as defined
make variables, so any build system that uses a variable named `SUBDIR`
internally silently inherits it. FFmpeg's `ffbuild/common.mak` guards its
top-level block with `ifndef SUBDIR` ("am I included from a library subdir?")
— with the conda value present, the block defining the `LINK` macro is
skipped, every shared-lib link line expands to an **empty recipe** (legal
make!), and the build "succeeds" while producing zero `.so` files; the
failure only surfaces later as `install: No such file or directory` on a
dangling symlink. Fix: `unset SUBDIR` at the top of the recipe's build script
(see `recipes/ffmpeg/recipe.yaml`).

## win-64 ffmpeg (shipped target) — MSVC notes and follow-ups

Windows became a **required shipped target** (2026-07-15), which makes the
GPL-only conda-forge win-64 ffmpeg unshippable — the local recipe now builds
both platforms. The MSVC leg (validated only on the pixi-ci win-64 runner; a
Linux box cannot build it) carries three non-obvious fixes, all flagged in a
pre-implementation Codex review:

- **Dynamic CRT**: `-MD` passed explicitly via `--extra-cflags` (MSVC defaults
  to the static CRT; `--enable-shared` does not select it), with a
  `config.mak` assertion so a toolchain change can't silently revert it.
- **Import-lib install location**: upstream `make install` puts the MSVC
  import `.lib` files in `bin/` while the generated `.pc` files point at
  `lib/` — pkg-config discovery would succeed and linking fail. Fixed with the
  same `config.mak` sed conda-forge's feedstock uses, plus a post-install
  layout assertion.
- **Path forms**: mixed (`C:/…`, `cygpath -m`) for everything cl/link and the
  baked `--prefix`; MSYS (`/c/…`) only for `PKG_CONFIG_LIBDIR`. Using
  `PKG_CONFIG_LIBDIR` (not `_PATH`) keeps MSYS/system packages from
  satisfying the explicit dependency checks.

Hardware decode swaps VAAPI/libdrm for `--enable-d3d11va --enable-dxva2`
(header-only Windows SDK backends, no host deps). `FfmpegDecoder` gained an
explicit D3D11VA preference: FFmpeg's device enum orders DXVA2 first, so the
plain `av_hwdevice_iterate_types()` walk would pick the legacy backend. The
package's consumer test asserts D3D11VA hwaccel configs exist for
h264/hevc/av1 (GPU-independent, catches Windows-SDK regressions).

### Follow-ups before a shipped Windows artifact (Codex review, 2026-07-15)

1. **Hosted immutable channel + multi-platform lock.** `local-channel/` +
   the CI platforms-sed is bootstrap-only: a shipped state needs the
   linux-64 + win-64 recipe outputs published (prefix.dev, next to
   `plotjuggler_sdk`), `platforms = ["linux-64", "win-64"]`, both ffmpeg pins
   pointed at the hosted channel, and a committed multi-platform `pixi.lock`.
   Archive the exact release `.conda` artifacts.
2. **Release staging / LGPL §6 compliance.** windeployqt does NOT stage
   third-party DLLs: the installer step must explicitly ship the four FFmpeg
   DLLs + zlib + dav1d, their license texts (`COPYING.LGPLv2.1`, notices),
   publish the exact FFmpeg source + configure line alongside each release,
   add the FFmpeg LGPL notice to the About dialog (currently MPL-only), keep
   the DLLs replaceable (no renaming/embedding), and re-run the license guard
   against the staged installer tree with conda off PATH.
3. **Patent posture** (H.264/HEVC decode) is jurisdiction-dependent and not
   resolved by LGPL-only builds — a release/legal review item, not a build
   item.
4. **Flip the win-64 leg to blocking** (`continue-on-error: false` + required
   branch-protection check) once green; separate any environment-sensitive
   tests from the blocking suite first if stragglers remain.

## CI (`.github/workflows/pixi-ci.yml`)

A matrix job (`ubuntu-22.04` + `windows-latest`):

1. `setup-pixi` (binary only — `run-install: false`, no cache: there is no
   committed lockfile to key on, and setup-pixi's cache requires an install).
2. Build the gap-package channel with **`pixi exec`** (not `pixi run` — `pixi
   run` would first try to solve the default env, which needs the channel this
   step produces).
3. Windows only: set up MSVC (`ilammy/msvc-dev-cmd`) **after** the channel build
   (rattler-build runs its own `vcvars` internally; stacking a second one
   overflows cmd.exe's line limit), then `sed` the manifest's single platform to
   `win-64` (the committed manifest is linux-64-only — a Linux box cannot build
   the win-64 gap packages, so no valid win-64 lock can be committed).
4. `pixi run build`; then tests — on Linux the Qt-free **core** suite (`ctest`
   scoped to the core module dirs, see below), on Windows the full `pixi run
   test`.

### CI results

- **linux-64: green** — full build + the **Qt-free core test suite** (~155
  tests, `ctest` scoped to the core module dirs, all passing in seconds).
  The widget/GL test families (`pj_scene3D/widgets`, `pj_scene2D/widgets`,
  `pj_widgets`, plotting-GL) are *not* run on this leg: conda's Qt cannot bring
  up a hardware-less GL context on the GPU-less runner — instead of falling back
  to llvmpipe (as the aqt-Qt + system-Mesa combo the Conan CI uses does), context
  creation *stalls*. Getting conda's libglvnd to dispatch to a software GLX
  vendor headlessly is a deep, GPU-less-only plumbing problem, orthogonal to
  whether the build works. Rather than a fragile GL denylist (the widget tests
  create contexts in ways that are hard to enumerate by name), the leg scopes
  ctest to the modules that *structurally* never link `Qt6::Widgets`/OpenGL and
  so cannot hang: `pj_datastore`, `pj_runtime`, `pj_scripting`,
  `plotjuggler_sdk` (base + plugins), `pj_scene3D/core`, `pj_scene2D/core`,
  `pj_scene_common` — the columnar store, runtime services, the Luau/Python
  scripting engine, the SDK, and the scene *core* logic (tf, pointcloud/occupancy
  /voxel codecs, media decode). The GUI/GL tests are verified locally (with a
  GPU), by the Conan Linux CI, and by the pixi **win-64** leg. The local
  `pixi run test` still runs the full suite.
- **win-64: the whole app compiles, and 243/246 tests pass.** This is notably
  *further than the canonical Conan `windows-ci.yml`, whose Build step fails* —
  the pixi/conda toolchain builds all of PJ4 on Windows. The 3 remaining test
  failures are Windows-specific, not pixi-build-system issues: `python_engine_test`
  and `DataProcessorTransformTest` (the embedded-Python `PYTHONHOME` layout under
  conda differs from Conan's, which the current `pj_scripting` bake targets), and
  `PlotCanvasContextRecreationGlTest` (GL-context recreation). The win-64 leg is
  marked **`continue-on-error`**, matching the Conan `windows-ci.yml` convention:
  PJ4 v1 is Linux-only (a CLAUDE.md non-goal), so Windows is a portability
  tracker, not a release gate. It proves the *build system* works on Windows.

## Assessment

What pixi buys:

- **One-command bootstrap** — `pixi run build` on a fresh machine installs
  compiler, CMake, Ninja, Qt 6.11.1, and every library. No `install_qt6.sh`,
  no `apt-get install libva-dev libdrm-dev`, no Conan profile quirks (the
  asensus-remote / gdbm C23 class of breakage disappears).
- **Minutes instead of ~an hour of cold Conan source builds** (ffmpeg+cpython
  +assimp dominate `--build=missing` time); no more poisoned-Conan-cache CI
  failures.
- **Lockfile** (`pixi.lock`) pinning the entire environment including Qt and
  the compiler — stronger reproducibility story than conanfile.txt + a
  side-channel Qt install.
- Aligns with where the SDK already is (conda packages on prefix.dev).

What Conan still does better here:

- **Per-package feature tuning** (the 200-line ffmpeg/assimp option blocks
  have no conda equivalent short of maintaining our own package variants —
  which `recipes/ffmpeg` now does for the case that matters most, licensing;
  assimp still ships feature-complete from conda-forge).
- **Static, trimmed artifacts** for release packaging (AppImage) — conda
  prefers shared, feature-complete builds.

Suggested next steps if we pursue this:

1. Publish `luau` / `libmcap` / `cloudini` / `libnanoarrow` to the PlotJuggler
   prefix.dev channel (recipes are ready; same release pipeline as the SDK).
2. Land the 3 CMake portability fixes on `main` regardless — they're
   package-manager-neutral and one fixes a latent bug.
3. Decide the split: pixi as the *developer/CI* environment (fast bootstrap,
   hermetic), Conan retained only for the *release packaging* build (tuned
   static deps) — or commit fully to pixi and accept shared feature-complete
   deps in the AppImage.
