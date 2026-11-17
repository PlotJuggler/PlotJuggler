# Building PJ4 with pixi instead of Conan — exploration findings

`pixi.toml`, `recipes/` and `.github/workflows/pixi-ci.yml` build PJ4 from a pixi / conda-forge environment; the
Conan path (`./build.sh`) is the canonical build. This file records what that build needs and what was observed.
Versions are in `pixi.toml`, `recipes/*/recipe.yaml`, `conanfile.txt` and `versions.env`, not here. The SDK's own
conda packages are published to prefix.dev from the `plotjuggler_sdk` repository.

## Summary

`pixi run build` builds the whole app with:

- every dependency but three taken prebuilt from conda-forge, Qt included (`qt6-main`, whose pin in `pixi.toml` must
  track `PJ_QT_VERSION`), which also replaces `scripts/install_qt6.sh`; one command bootstraps the whole toolchain,
  compiler included;
- the two conda-forge gaps (**luau**, the C++ **mcap** library) and an LGPL-only **ffmpeg** built as local
  `rattler-build` recipes under `recipes/` into `local-channel/`;
- small CMake portability shims, all no-ops under Conan (below);
- no change to module source code.

## Mapping to conda-forge

| Conan / repo | conda-forge | note |
|---|---|---|
| `scripts/install_qt6.sh` (`PJ_QT_VERSION`) | `qt6-main` at the same version | all needed components, `GuiPrivate` headers included |
| Qwt (FetchContent of the pinned release) | none | conda-forge's prebuilt `qwt` is pinned to an older `qt6-main` minor and cannot co-solve, so pixi FetchContent-builds Qwt too; `-DPJ_SYSTEM_QWT=ON` (Linux only) can return once a matching build exists (comment in `pixi.toml`) |
| `plotjuggler_sdk` (Conan package) | none | like every non-Conan path, CMake fetches the pinned SDK release and builds it in-tree |
| nanoarrow (fetched in-tree, `PjNanoarrow.cmake`) | `libnanoarrow` | conda-forge's IPC target links zstd and lz4 without looking them up; `PjNanoarrow.cmake` provides the missing targets |
| fmt | `fmt` | compiled library instead of header-only; same `fmt::fmt` target |
| tsl-robin-map | `tsl_robin_map` | underscore name; installs the standard `tsl-robin-map` config |
| cpython + pybind11 | `python` + `pybind11` | see the embedded-Python shim below |
| zstd, fast_float, libpng, libjpeg-turbo, lz4 | same names (lz4 is `lz4-c`) | libjpeg-turbo target naming differs, see the shims |
| libarchive (static) | `libarchive` (shared) | conda-forge has no static build: one more runtime `.so` |
| ffmpeg (trimmed LGPL build) | local `ffmpeg`, build string `pj_lgpl_lean_*` (`recipes/ffmpeg`) | same decode-only configure line as Conan |
| backward-cpp (+ elfutils) | `backward-cpp` + `elfutils` | `dw` backend resolves in-env |
| draco, assimp | `draco`, `assimp` | Conan pins draco to what its source-built assimp needs; conda-forge's assimp ships every importer |
| system OpenGL headers | `libgl-devel`, `libopengl-devel`, `libegl-devel` | replace the system Mesa dev packages |
| system libva / libdrm (VAAPI) | pulled in by the ffmpeg build | no `apt-get install libva-dev libdrm-dev` |

## Local recipes (`recipes/`)

| package | approach | provides |
|---|---|---|
| `luau` | upstream has no CMake install: static libs, hand-installed, hand-written `LuauConfig.cmake` | `Luau::VM`, `Luau::Compiler`, `Luau::Ast`, `Luau::CodeGen` with Conan's link graph |
| `libmcap` | header-only copy plus a hand-written `mcap-config.cmake` (conda-forge `mcap` is the Python wrapper) | `mcap::mcap` INTERFACE carrying lz4 + zstd |
| `ffmpeg` (linux-64 + win-64) | decode-only LGPL-only rebuild of `conanfile.txt`'s trimmed option block; distinctive build string; the package test asserts `avcodec_license()` says LGPL and the configure line has no `--enable-gpl` / `--enable-nonfree` | `libav*.pc` + `libswscale.pc` for `PjScene2DFfmpeg.cmake`'s pkg-config fallback |

Each gap recipe ships a platform-neutral `*Config.cmake` (using `find_library`, so one body resolves `libFoo.a` and
`Foo.lib`) and `if: unix` / `if: win` install branches. ffmpeg exists on conda-forge, but its `lgpl_*` builds are
feature-complete (every LGPL decoder, encoder and network protocol, about 12 MB plus dependencies against 3.9 MB for
the trimmed build), the default solve picks `gpl_*`, and the win-64 build is GPL-only.

Build the channel with:

```bash
pixi exec --spec "rattler-build>=0.66" \
  rattler-build build --recipe-dir recipes/ -m recipes/variants.yaml \
    -c conda-forge --output-dir local-channel
```

It must be `pixi exec` (an ephemeral environment), not `pixi run`: `pixi run` and `pixi install` solve the default
environment first, which needs the channel this step produces. `-m recipes/variants.yaml` is load-bearing on Linux
(the sysroot gotcha below).

## CMake portability shims (no-ops under Conan)

- **`pj_scene2D/core/CMakeLists.txt`:** `libjpeg-turbo::libjpeg-turbo` is Conan's umbrella name; upstream's config
  exports `::jpeg` / `::turbojpeg`. A bridge target covers it (the module uses only the turbojpeg API).
- **`pj_scripting/CMakeLists.txt`:** `cpython::embed` is Conan-only. Without it the module resolves
  `find_package(Python COMPONENTS Development.Embed)` and bridges `Python::Python` to that name.
- **`pj_datastore/cmake/PjNanoarrow.cmake`:** supplies the zstd / lz4 targets conda-forge's nanoarrow IPC target
  links without finding.

## Semantic deltas

- **libarchive** is shared. Its conda build string says `gpl_*` because of the bsdtar / bsdcpio tools; the library
  itself is BSD, so linking it from an MPL app is fine.
- **assimp** ships with every importer (bigger library, wider parse surface on untrusted mesh files), where Conan
  builds only the four PJ4 uses. A leaner build would be one more local recipe.
- **Embedded Python** is the environment's shared `libpython`, so `pj_app` needs the environment (or a bundled
  runtime) at run time; `PJ_PYTHON_HOME` bakes the pixi prefix, which suits development only.
- **Toolchain:** conda's `cxx-compiler` with the conda sysroot instead of the system compiler: hermetic, but ccache
  entries do not carry over, so the first pixi build is cold.

## How to build

```bash
# 1. Build the local channel (once; again only when a recipe changes):
pixi exec --spec "rattler-build>=0.66" \
  rattler-build build --recipe-dir recipes/ -m recipes/variants.yaml \
    -c conda-forge --output-dir local-channel
# 2. Build and test (pixi solves the environment, Qt included, against local-channel):
pixi run build                     # configures + builds into build-pixi/
pixi run test                      # ctest over build-pixi
```

## Observed results

- Environment solve and install: minutes on the first run, about 2.1 GB, cached in `.pixi/`.
- The whole app, `pj_app` included, builds on linux-64 and win-64.
- During the exploration the linux-64 suite matched the Conan build; its one failure was a host-environment failure
  that fails the same way under Conan.

## The glibc sysroot gotcha (rattler-build)

Building the gap packages naively produced archives referencing `__isoc23_strtol` / `__isoc23_strtoull`, glibc 2.38+
symbols absent from conda's target sysroot (glibc 2.28), so linking PJ4 failed with `undefined reference to
__isoc23_*`. Without an explicit stdlib pin, rattler-build's compiler used the build host's glibc headers, which on a
modern host emit those redirects. The fix is conda-forge's idiom: `${{ stdlib('c') }}` in each recipe's build
requirements and `c_stdlib: sysroot` / `c_stdlib_version` in `recipes/variants.yaml` (passed with `-m`), which puts
the right `--sysroot` into the flags. It is gated `if: linux`. conda-forge's own CI never sees it (its old build
hosts emit no `__isoc23_*`): it shows only on a recent host such as a dev box or `ubuntu-latest`.

## The `SUBDIR` gotcha (rattler-build and make)

rattler-build exports `SUBDIR=linux-64` into the build script's environment, and GNU make imports environment
variables as make variables. FFmpeg's `ffbuild/common.mak` guards its top-level block with `ifndef SUBDIR`; with the
conda value set, the block defining `LINK` is skipped, every shared-library link line expands to an empty recipe, and
the build "succeeds" with no `.so` files, failing only later at install on a dangling symlink.
`recipes/ffmpeg/build_ffmpeg.sh` unsets `SUBDIR` first.

## win-64 ffmpeg (MSVC)

The MSVC leg builds only on the win-64 CI runner, and carries three non-obvious fixes:

- **Dynamic CRT:** `-MD` passed explicitly in `--extra-cflags` (MSVC defaults to the static CRT and
  `--enable-shared` does not change that), with a `config.mak` assertion.
- **Import-library location:** upstream `make install` puts the MSVC import `.lib` files in `bin/` while the `.pc`
  files point at `lib/`, so pkg-config discovery succeeds and linking fails. The recipe applies the `config.mak` sed
  conda-forge's feedstock uses, plus a post-install layout assertion.
- **Path forms:** mixed (`C:/…`, `cygpath -m`) for cl / link and the baked `--prefix`; MSYS (`/c/…`) only for
  `PKG_CONFIG_LIBDIR`. `PKG_CONFIG_LIBDIR` (not `_PATH`) keeps MSYS and system packages from satisfying the explicit
  dependency checks.

Hardware decode uses `--enable-d3d11va --enable-dxva2` (header-only Windows SDK backends). `av_hwdevice_iterate_types()`
lists DXVA2 first, so `FfmpegDecoder` asks for D3D11VA explicitly. The package test asserts D3D11VA hwaccel configs
for h264 / hevc / av1, independent of the GPU.

What a shipped pixi-built Windows artifact would still need (open, not scheduled):

- a hosted channel (prefix.dev, beside `plotjuggler_sdk`) with the linux-64 and win-64 recipe outputs,
  `platforms = ["linux-64", "win-64"]` and a committed multi-platform `pixi.lock`;
- LGPL staging: windeployqt does not stage third-party DLLs, so the installer must ship the FFmpeg, zlib and dav1d
  DLLs with their licence texts, keep them replaceable, and publish the exact FFmpeg source and configure line;
- a release / legal review of H.264 / HEVC decode patents, which an LGPL-only build does not settle.

## CI (`.github/workflows/pixi-ci.yml`)

A matrix of `linux-64` (`ubuntu-22.04`) and `win-64` (`windows-2022`, shared sccache with the other Windows
workflows):

1. `setup-pixi` installs the binary only: there is no committed lockfile for its cache to key on. The pixi package
   downloads are cached on `pixi.toml` + `recipes/`, the built `local-channel/` on `recipes/`.
2. The channel is built with `pixi exec`, skipped on a cache hit.
3. Windows: MSVC is set up after the channel build (rattler-build runs its own `vcvars`; stacking a second overflows
   cmd.exe's line limit), then the manifest's single platform is rewritten to `win-64` (a Linux box cannot build the
   win-64 gap packages, so no win-64 lock can be committed).
4. `pixi run build`, then tests: on Linux the Qt-free core modules only (`pj_datastore`, `pj_runtime`,
   `pj_scripting`, `pj_tf`, `pj_scene3D/core`, `pj_scene2D/core`, `pj_scene_common`), on Windows the full
   `pixi run test`.

The Linux leg skips the widget and GL tests because, on the GPU-less runner, conda's Qt stalls creating a GL context
instead of falling back to llvmpipe as the aqt Qt with system Mesa does in the Conan CI; routing conda's libglvnd to
a software GLX vendor headlessly was not solved. Scoping by module (modules that never link `Qt6::Widgets` or OpenGL)
was chosen over a GL test denylist because the widget tests create contexts in ways hard to enumerate by name. The
GUI and GL tests run in the Conan Linux CI and the pixi win-64 leg; a local `pixi run test` runs the full suite.

## Trade-offs observed

pixi gives:

- a one-command bootstrap (compiler, CMake, Ninja, Qt and every library), with no `install_qt6.sh`, no system `-dev`
  packages and no Conan profile issues;
- minutes instead of about an hour of cold Conan source builds (ffmpeg, CPython and assimp dominate
  `--build=missing`);
- a lockfile that can pin the whole environment, Qt and the compiler included.

Conan keeps:

- per-package feature tuning (the ffmpeg and assimp option blocks); with conda that means maintaining local variants,
  as `recipes/ffmpeg` does for licensing;
- static, trimmed artifacts for release packaging (AppImage), where conda prefers shared, feature-complete builds.
