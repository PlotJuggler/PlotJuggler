# Building PlotJuggler 4

[← Back to the README](../README.md)

PJ4 uses **C++20, CMake 3.22+ and Conan 2**, with Qt pinned in
[`versions.env`](../versions.env). The commands below follow the repository's
Linux build scripts; the Windows entry points follow them, and packaging
references are at the end.

For a native **x86_64 Linux Nix** build, use `nix build`, `nix run`, or
`nix develop` with the root flake. See the [Nix guide](../packaging/nix/README.md)
for prerequisites, the development/test commands, and plugin limitations. This
path does not require Conan or a separate Qt installation.

## Linux setup

On Ubuntu 22.04 or newer, install the compiler, build tools and system libraries:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake ninja-build git pkg-config ccache \
  python3 python3-pip python3-venv \
  libgl1-mesa-dev libegl1-mesa-dev libxkbcommon-dev \
  libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 \
  libxcb-randr0 libxcb-render-util0 libxcb-shape0 libxcb-xinerama0 \
  libxcb-xkb1 libxkbcommon-x11-0 libfontconfig1 libdbus-1-3 \
  libva-dev libdrm-dev xvfb zstd
```

Clone the public PJ4 branch with its submodules. `main` contains PJ3:

```bash
git clone --branch main-4.x --recurse-submodules https://github.com/PlotJuggler/PlotJuggler.git
cd PlotJuggler
```

For an existing checkout, run `git submodule update --init --recursive`.

Install the Python build tools in a virtual environment:

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install 'conan>=2,<3' 'aqtinstall>=3.3'
```

If you do not already have a Conan default profile, create one:

```bash
conan profile detect
```

## Build and run

Run these commands from the repository root, with the virtual environment active:

```bash
./scripts/install_qt6.sh
./build.sh
./run.sh
```

`scripts/install_qt6.sh` downloads the pinned Qt release into `.qt/` and reuses it on
subsequent runs. `build.sh` configures the public PlotJuggler Conan remote,
resolves dependencies using `conan.lock`, and builds into `build/`.
The first build may also compile missing dependencies.

Rerun `./build.sh` for incremental builds. To omit the test suite, use
`./build.sh --skip-test`; other options are listed by `./build.sh --help`.

Scene3D, the plots and the media viewer render through QRhi: OpenGL on Linux,
D3D11 on Windows, Metal on macOS; `PJ_GRAPHICS_API=opengl|vulkan|d3d11|d3d12|metal`
overrides the API at run time. There is no other GPU renderer to select at configure
time.

Launch through `./run.sh`: it clears `QT_IM_MODULE`, because otherwise the IBus
platform input context can be loaded from a system (older) Qt install and crash under
the pinned Qt runtime.

File loaders, streamers and toolboxes are distributed separately. Open
**File → Marketplace** to install them after launching your source build.
See [pj-official-plugins](https://github.com/PlotJuggler/pj-official-plugins)
to build plugins yourself.

## Direct CMake builds and offline preparation

`build.sh` is optional. After the Linux setup above, resolve dependencies and
configure CMake directly while network access is available:

```bash
./scripts/install_qt6.sh
./scripts/configure_conan_remote.sh
conan install . --output-folder=build --build=missing \
  --lockfile=conan.lock --lockfile-partial \
  -s build_type=RelWithDebInfo -s compiler.cppstd=20 \
  -r plotjuggler-conan -r conancenter
source versions.env
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH="$PWD/.qt/$PJ_QT_VERSION/gcc_64" \
  -DPJ_BUILD_TESTS=OFF
```

The Conan recipe and lockfile list the library dependencies; Qt is installed
separately. The configure step also populates `build/_deps` with Qwt source.
With tests enabled, it additionally fetches the pinned SDK source for test
fixtures. Conan may compile missing dependency binaries during preparation;
the application has not been built yet.

Retain the checkout and initialized submodules, `.qt`, the Conan cache
(`conan config home`), and `build`, including its generated toolchain and
`_deps`. On the same machine, build without network access:

```bash
cmake -S . -B build -DFETCHCONTENT_FULLY_DISCONNECTED=ON
cmake --build build --target pj_app --parallel
```

To regenerate the Conan toolchain from the prepared cache, repeat `conan install`
with `--no-remote --build=never` in place of the two `-r` options and
`--build=missing`. Cache preparation must use the same source revision, profiles,
compiler, architecture and build options as the offline build. Generated CMake
and Conan files contain absolute paths; a transferred build needs regeneration
and local source overrides such as `FETCHCONTENT_SOURCE_DIR_QWT_UPSTREAM` and,
when needed, `FETCHCONTENT_SOURCE_DIR_PLOTJUGGLER_SDK`.

## Notes for PJ3 package maintainers

- PJ4's executable is `plotjuggler4`; its application target is `pj_app`.
  The core application uses plain CMake, independently of ROS. ROS integration
  is distributed with the separate official plugins.
- PJ3's Qt5 plugin binaries cannot be loaded by PJ4. Port plugins to the
  [plugin SDK](https://github.com/PlotJuggler/plotjuggler_sdk) and follow the
  [official-plugin porting guide](https://github.com/PlotJuggler/pj-official-plugins/blob/main/porting_guide.md).
  Install compatible extensions through **File → Marketplace**.
- PJ4 filters use a different contract from PJ3 custom Lua functions; see
  [Filter Class Contract](../pj_scripting/docs/FILTER_CLASS.md#5-differences-from-pj3-lua_custom_function-intentional).
  Keep original PJ3 layouts and scripts when validating a migrated analysis.
- Packaging and runtime-library deployment are separate from compiling the
  application. Follow the [packaging guides](../packaging/README.md), including
  the [Debian installation layout](../packaging/deb/README.md#layout), rather than
  expecting `cmake --install` to install the desktop application and its runtime.

## Run tests

After a default build:

```bash
./test.sh
```

The wrapper runs CTest without opening windows. For GUI and GPU coverage under a
virtual display, use the same command as Linux CI:

```bash
xvfb-run -a ctest --test-dir build --output-on-failure --timeout 120
```

GPU-backed tests self-skip when there is no usable graphics backend (the QRhi
display runners under `offscreen`, or a backend without a driver) — expected on a
plain headless run, but Linux CI's xvfb + Mesa llvmpipe/lavapipe lane is supposed
to have both. Set `PJ_REQUIRE_GPU_TESTS=1` (as that CI lane
does) to turn those skips into hard failures instead, so a broken stack can't go
quietly green there. The QRhi display runners register one pass per backend
(`<runner>.opengl.*`, `.vulkan.*`, labels `qrhi-<backend>`); the Vulkan passes
need `VK_ICD_FILENAMES` pointing at a Vulkan driver such as lavapipe.

Before a push, `scripts/perf/gate_par.sh <out> [--wasm] [--baseline
<previous out>/dump]` runs the whole local gate in parallel: one compile under
the build slot (`scripts/perf/build_slot.sh`, an `flock` shared by every build
and benchmark on the machine), then the offscreen suite, the OpenGL and Vulkan
display runners (each on its own private headless GNOME Shell + Xwayland) and
the Playwright browser suite at the same time, and a pixel
dump compared image by image (`scripts/perf/pxcmp.py`) with the previous run's.
The script exits non-zero when any step failed (`<out>/status_*`). `--check` prints the resolved inputs and exits; the script header lists the
environment variables (emsdk and Qt WebAssembly locations, build jobs, lock
file).

## Build internals

`build.sh` (options: `./build.sh --help`):

1. Checks for `.qt/<PJ_QT_VERSION>/gcc_64/` and tells you to run
   `./scripts/install_qt6.sh` if it is missing.
2. Registers the PlotJuggler Conan remote (`scripts/configure_conan_remote.sh`, also
   called by CI) and runs `conan install ... --output-folder=build --build=missing`
   with `compiler.cppstd=20` from `conanfile.txt`.
3. Configures CMake with `CMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake` and
   `CMAKE_PREFIX_PATH=./.qt/<PJ_QT_VERSION>/gcc_64`.
4. Builds with `cmake --build build -j<jobs>`: one job per core, capped at one per
   1.5 GiB of available memory so a full rebuild cannot exhaust RAM (`PJ_BUILD_JOBS`
   overrides).

`ccache` is used automatically when installed, and so is the `mold` linker
(`-DCMAKE_LINKER_TYPE=MOLD`, CMake 3.29+), which links the many test executables several
times faster than GNU ld.

**System libraries.** The Conan FFmpeg build needs `libva-dev` and `libdrm-dev` on the
build host: `conanfile.txt` enables `ffmpeg/*:with_vaapi=True` / `with_libdrm=True` for
VAAPI hardware video decode (the `FfmpegDecoder` GPU path, which falls back to software
without a usable GPU driver), and the recipe's `vaapi/system` and `libdrm` wrappers
resolve `libva` / `libva-drm` through `pkg-config`. End users only need the `libva2`
runtime, which packaging pulls in; CI installs the `-dev` packages in
[`linux-ci.yml`](../.github/workflows/linux-ci.yml).

**The SDK.** `plotjuggler_sdk` recipes and prebuilt binaries live on PlotJuggler's JFrog
Conan remote (anonymous read). Build paths without Conan (WebAssembly, pixi) fetch the
pinned release tag from GitHub and build the SDK in-tree (see the root
`CMakeLists.txt`). `pj_datastore` is added right after the SDK is resolved, so the
`plotjuggler_sdk::base` and `pj_internal_fmt` targets it links already exist; its Conan
dependencies (`nanoarrow`, `tsl-robin-map`, `benchmark`) are in the root
`conanfile.txt`.

**Local-SDK mode.** `./build.sh --sdk-local[=PATH]` builds `plotjuggler_sdk` from a
local working tree (default: the `../plotjuggler_sdk` sibling checkout) instead of the
pinned Conan package, so an unreleased SDK change needs no release: editing an SDK header
and re-running `./build.sh` is one incremental rebuild. Dev-only: not reproducible,
refused in CI, and never a way to merge; merged code always builds from a released pin.

**Tests.** `enable_testing()` is wired at the top level, so `ctest` covers every module.
Per-module gtest runners are registered with `gtest_discover_tests` in
[`cmake/PjTests.cmake`](../cmake/PjTests.cmake), one process per case, under ctest names
`<runner>.<Suite>.<Case>`. `test.sh` forces `QT_QPA_PLATFORM=offscreen` because many
widget tests create a `QApplication` without choosing a platform and would otherwise
open windows.

**Build tree size and debug info.** `./build.sh --minimal` builds what the packaging
pipeline compiles (app closure only, no tests, demos or debug info); `--no-tests`,
`--no-demos`, `--target NAME`, `--debug-info LEVEL` and `--no-compress-debug` select the
pieces individually. For Linux RelWithDebInfo builds, `PJ_DEBUG_INFO=none|lines|full|split`
selects no debug info, line/backtrace info, full DWARF, or DWARF in `.dwo` sidecars, and
`PJ_COMPRESS_DEBUG=ON|OFF` controls zlib compression
([`cmake/PjDebugInfo.cmake`](../cmake/PjDebugInfo.cmake)). `build.sh` defaults to `split`
and `ON` on Linux; its options, or the same names as environment variables (how CI and
the container wrapper drive it), override either knob.
[`packaging/appimage/build_in_docker.sh`](../packaging/appimage/build_in_docker.sh)
defaults to `PJ_DEBUG_INFO=none`, so the AppImage ships without DWARF, and the Conan
static archives in the builder image are stripped of DWARF; pass `--with-tests` to it to
build the full suite and demos, then filter a module inside the builder image with
`ctest -R '^pj_runtime_tests\.' --test-dir build`. Linux GNU/Clang builds use thin
archives, and `resources.qrc` is compiled once as `pj_resources`.
[`scripts/build_tree_size.sh`](../scripts/build_tree_size.sh) `<build-dir>` reports bytes
by artifact class in decimal GB.

**Dock icon.** A dev build shows a generic gear in the GNOME/Wayland dock because no
`plotjuggler4.desktop` is installed (Wayland resolves the icon through app_id → desktop
file → icon theme, never the window icon). `scripts/install_desktop_entry.sh` registers a
user-local entry pointing at this checkout's `run.sh` (`--uninstall` removes it); the
`.deb` and integrated AppImages ship their own entry.

## Versions

[`versions.env`](../versions.env) is the single source of truth for `PJ_APP_VERSION`,
`PJ_QT_VERSION` and `PJ_APPIMAGE_ARCH`; never write a version or architecture literal
elsewhere, and change one by editing `versions.env` and nothing else. Consumers:

- bash scripts `source "<anchor>/versions.env"` by absolute path;
- the Windows PowerShell scripts parse its `KEY=VALUE` lines themselves
  (`scripts/windows/PjWindowsCommon.ps1`);
- CMake reads it with `pj_read_versions()` in
  [`cmake/PjVersions.cmake`](../cmake/PjVersions.cmake), which stamps
  `cmake/pj_version.h.in` for `main.cpp` and feeds `find_package(Qt6 …)`;
- CI uses a `Read versions` step (`source ./versions.env` → `$GITHUB_ENV`);
- Docker gets `--build-arg`s from `build_in_docker.sh`.

Only the app version is overridable per build (`-DPJ_VERSION=<v>` or the `PJ_VERSION`
environment variable; release CI stamps the git tag); `PJ_QT_VERSION` and
`PJ_APPIMAGE_ARCH` are hard pins. The Qt platform dir tokens (`gcc_64`, `msvc2022_64`)
are a separate namespace: leave them literal, never derive them from
`PJ_APPIMAGE_ARCH`. Windows CI installs Qt inline because `scripts/install_qt6.sh`
installs the `gcc_64` build.

## Portability

Linux and Windows are shipped targets. macOS is not yet, but
[`macos-ci.yml`](../.github/workflows/macos-ci.yml) builds and tests on macOS arm64
(AppleClang, Scene3D on Metal) nightly when the default branch moved, and on demand
(`gh workflow run "macOS CI" --ref <branch>`). AppleClang's `-Wunused-const-variable` and
`-Wunused-lambda-capture` catch platform-conditional code GCC accepts, and macOS temp
dirs sit behind the `/var -> /private/var` symlink, so tests must compare canonical paths.

Windows packaging: conda-forge ships no LGPL FFmpeg for win-64, so pixi-based Windows
artifacts must use the local `recipes/ffmpeg` package (the Conan path builds its own
LGPL-trimmed FFmpeg on all platforms). Windows runtime packaging (windeployqt plus the
FFmpeg/Qt DLLs `pj_app` needs) is in scope: `windows-ci.yml` resolves DLLs through `PATH`
at test time, which is not a shippable layout.

## Third-party dependencies

Vendored dependencies live in `3rdparty/<name>/` (the PJ3 convention) and are added from
the top-level `CMakeLists.txt`, mostly with `add_subdirectory`. They are explicitly
vendored: do not take them from Conan or system packages.

- **Qt-Advanced-Docking-System**: the docking framework used by `pj_app`.
- **nanocdr**: added with `add_subdirectory`.
- **doomgeneric**: a C engine whose sources are globbed directly into the
  `pj-raster-helper` target. With `raster/helper` it forms an optional, GPL-isolated
  standalone executable that PlotJuggler never links. It is off by default: the target is
  built only with `-DPJ_BUILD_RASTER_HELPER=ON` (and only when the vendored source is
  checked out). The Linux release (`linux-appimage-release.yml`) turns it on
  (`PJ_BUILD_RASTER_HELPER=ON ./build.sh`) and stages the helper with
  `packaging/appimage/build_appimage.sh --retro-wad`, so the AppImage and the `.deb` ship
  it under `3rdparty/retro/` next to the app binary.

**Qwt is external, not vendored**: `3rdparty/qwt/` holds only CMake glue. The default
build fetches the official release tarball (SHA-256 pinned) and compiles it unmodified on
every path, pixi included (`-DPJ_SYSTEM_QWT=ON` can link conda-forge's prebuilt `qwt` once
it is built against the pinned `qt6-main`). Never patch Qwt sources: behavior PJ3 carried
as Qwt patches lives in the app, namely axis tick-label formatting in `pj_plotting`'s
`PlotScaleDraw` (twin in `pj_dialog_host`'s chart preview) and the "Lines and Dots" curve
style in `pj_plotting`'s `PlotCurve` (Lines plus one batched, pixel-weeded `drawDots`
pass; `PlotRhiCanvas` builds the same square dots as geometry). See
`3rdparty/qwt/README.PJ4.md`.

Other PJ3-style vendorables (`QCodeEditor`, `sol2`, `color_widgets`, `date`) are vendored
on the same pattern when a module needs them, decided case by case.

Everything else (GLM, assimp, FFmpeg, the Lua runtime, ...) comes from Conan, including
**backward-cpp** (`backward-cpp/1.6`): `pj_app` links `Backward::Backward` and installs a
`backward::SignalHandling` crash handler in `main.cpp` for symbolized stack traces. Its
default `dw` backend pulls `elfutils` from Conan, so rich traces need no system `-dev`
packages.

## Windows setup

Windows builds use the same Conan + CMake flow through three root entry points,
`build.bat`, `run.bat` and `test.bat`: thin shims over `scripts\windows\build.ps1`,
`run.ps1` and `test.ps1`, which run under the Windows PowerShell 5.1 that ships
with Windows and share `scripts\windows\PjWindowsCommon.ps1`. They mirror `build.sh` / `run.sh` / `test.sh` (same options, same
`PJ_*` variables, same `build\` tree) and reproduce the Windows CI job: VS 2022
MSVC 14.4x, Conan with every profile setting pinned (`compiler.version=194`),
Ninja Multi-Config, RelWithDebInfo with `/Z7` embedded debug info. A local
`build\` therefore accepts the exact commands from
[`windows-ci.yml`](../.github/workflows/windows-ci.yml) (`cmake -S . -B build
-G "Ninja Multi-Config" ...`, `cmake --build build --config RelWithDebInfo`,
`ctest --test-dir build --build-config RelWithDebInfo`) and vice versa.

Install:

- **Visual Studio 2022** with the *Desktop development with C++* workload
  (MSVC 14.4x). `build.bat` locates it with `vswhere` and imports `vcvars64.bat`
  itself, so a plain PowerShell or cmd prompt works. Visual Studio 2026 alone is
  refused: CI, releases and the official plugins are built with VS 2022 so that
  Conan package IDs and the plugin ABI line up (see the `runs-on` comment in
  [`windows-ci.yml`](../.github/workflows/windows-ci.yml)).
- **Python 3** from python.org, including the `py` launcher, then
  `pip install "conan>=2,<3" "ninja!=1.13.0" aqtinstall`. If `python` on `PATH`
  is the Microsoft Store stub, the scripts put the real interpreter first by
  themselves (the stub breaks the meson-built Conan dependencies such as dav1d).
  Ninja 1.13.0 writes linker response files that `link.exe` misreads once a
  line passes 16,383 characters (LNK1181 on a truncated object path,
  [ninja#2616](https://github.com/ninja-build/ninja/issues/2616)), so the
  scripts refuse it; 1.13.1 and newer are fixed.
- **CMake 3.25 or newer** (for `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT`) and **Git**.

Clone with submodules as in the Linux section (`git clone --recurse-submodules`,
or `git submodule update --init --recursive` in an existing checkout).

Install the pinned Qt kit (`PJ_QT_VERSION` in `versions.env`, `msvc2022_64`) into
`.qt\` with the Windows counterpart of `scripts/install_qt6.sh`:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\windows\install_qt6.ps1
```

It is a no-op when `.qt\<PJ_QT_VERSION>\msvc2022_64\bin\qmake.exe` already exists,
removes a partial kit before installing, retries transient mirror failures, and
installs the aqtinstall commit pinned in `windows-ci.yml` only if the aqt already
present cannot resolve the kit's metadata. `build.bat` never installs Qt; it
points here when the kit is missing.

## Build, run and test on Windows

From the repository root, in PowerShell or cmd:

```powershell
.\build.bat             # full dev tree: tests + demos
.\build.bat --minimal   # app closure only, what the packaging build compiles
.\run.bat
.\test.bat              # ctest, headless; extra arguments go to ctest, e.g. -R Toast
```

`build.bat` registers the public PlotJuggler Conan remote, creates a default
Conan profile only if none exists, seeds Conan's source download cache from
`.github\conan\source-backup` into `%LOCALAPPDATA%\PlotJuggler4\conan-source-cache`
(rewriting the `core.sources:download_cache=` and `core.sources:download_urls=`
lines of Conan's `global.conf` to point there and at ConanCenter's source
mirror, and printing the edit; `core.*` settings cannot be passed on the
command line), runs `conan install` with the CI settings, configures with
`-G "Ninja Multi-Config"` into `build\`, copies `build\compile_commands.json` to
the repository root, and builds the `RelWithDebInfo` configuration. The first
build compiles any dependency the remotes have no binary for (FFmpeg, CPython,
assimp, ...) from source and can take well over an hour; later builds are
incremental. `build.bat --help` lists the options; they are the ones `build.sh`
accepts, except that `--sanitize`, `--tsan`, `--debug-info` and
`--no-compress-debug` are Linux-only and rejected. `PJ_DRY_RUN=1` prints the
Conan and CMake command lines without building. `sccache` is used as the
compiler launcher when it is on `PATH`.

The application is `build\pj_app\RelWithDebInfo\plotjuggler4.exe` (Ninja
Multi-Config puts each configuration's binaries in a subdirectory). It needs the
Qt, FFmpeg and CPython DLLs at load time; `run.bat` prepends the pinned Qt `bin\`
and the Conan bin directories recorded at configure time in
`build\pj_ffmpeg_bin_dirs.txt` and `build\pj_python_bin_dirs.txt` to `PATH`, and
points `QT_PLUGIN_PATH` at the pinned kit, so launch through `run.bat` rather
than double-clicking the exe. Arguments are forwarded to the application
(`.\run.bat --help`); the Linux-only `--apitrace` / `--heaptrack` switches print
a notice and launch normally.

`test.bat` sets up the same `PATH`, forces `QT_QPA_PLATFORM=offscreen`, and runs
`ctest --test-dir build --build-config RelWithDebInfo --output-on-failure
--no-tests=error --timeout 120` followed by your arguments; `--test-dir DIR`
selects another tree, `--sanitize` accepts only `none`, and the exit code is
ctest's.

File loaders, streamers and toolboxes are distributed separately: open
**File → Marketplace** after launching your source build, or build them from
source as below.

### Plugins from source

`scripts\windows\build_plugins.ps1` builds a
[pj-official-plugins](https://github.com/PlotJuggler/pj-official-plugins) checkout
with that repository's own `build.sh`, run through Git for Windows' bash inside
the VS 2022 environment in the order its `ci-windows.yml` uses
(`scripts/ensure_core.sh`, then `build.sh`), and copies every `*_plugin.dll`
into one folder for `run.bat --plugin-dir`. The checkout defaults to
`pj-official-plugins\` inside this repository (gitignored; `--plugins-dir`
points elsewhere) and the folder to `pj-official-plugins\build\pj4_plugins`
(`--stage-dir`). From the repository root:

```powershell
git clone https://github.com/PlotJuggler/pj-official-plugins.git
powershell -ExecutionPolicy Bypass -File scripts\windows\build_plugins.ps1                # every plugin
powershell -ExecutionPolicy Bypass -File scripts\windows\build_plugins.ps1 data_load_csv  # one plugin
.\run.bat --plugin-dir pj-official-plugins\build\pj4_plugins
```

`build.sh` compiles with the default Conan profile, so the script refuses to run
unless that profile says `compiler=msvc` and `compiler.version=194`. It warns
when the checkout is behind its upstream but never pulls. A full build removes
stale `*_plugin.dll` files from the folder first; a single-plugin build only
replaces that plugin. Close any PlotJuggler started with that folder before
rebuilding: Windows cannot replace a loaded DLL, so the script stops without
changing the folder. The first build compiles Arrow/Flight, gRPC, protobuf and
more from source and can take hours. The ROS 2 subscriber is Linux-only.
Instead of `--plugin-dir`, the folder can be added under Preferences → plugin
folders.

## Windows CI, packaging and browser builds

| Target | Instructions |
| --- | --- |
| Windows CI (the runner invocation the scripts mirror) | [Windows CI workflow](../.github/workflows/windows-ci.yml) |
| Windows installer | [Installer guide](../packaging/installer/README.md) |
| Linux AppImage / Docker build | [AppImage guide](../packaging/appimage/README.md) |
| Debian / Ubuntu package | [Debian packaging guide](../packaging/deb/README.md) |
| WebAssembly | [Browser build and deployment guide](WASM_DEPLOYMENT.md) |

For Qt toolchain details, see [Qt notes](QT_NOTES.md).
