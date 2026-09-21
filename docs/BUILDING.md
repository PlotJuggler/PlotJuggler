# Building PlotJuggler 4

[← Back to the README](../README.md)

PJ4 uses **C++20, CMake and Conan 2**, with Qt pinned in
[`versions.env`](../versions.env). The commands below follow the repository's
Linux build scripts; the Windows entry points follow them, and packaging
references are at the end.

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

Clone the repository with its submodules. The PJ4 source repository is currently
private, so this requires a GitHub account with access:

```bash
git clone --recurse-submodules https://github.com/PlotJuggler/PJ4.git
cd PJ4
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

File loaders, streamers and toolboxes are distributed separately. Open
**File → Marketplace** to install them after launching your source build.
See [pj-official-plugins](https://github.com/PlotJuggler/pj-official-plugins)
to build plugins yourself.

## Run tests

After a default build:

```bash
./test.sh
```

The wrapper runs CTest without opening windows. For GUI and OpenGL coverage
under a virtual display, use the same command as Linux CI:

```bash
xvfb-run -a ctest --test-dir build --output-on-failure --timeout 120
```

## Windows setup

Windows builds use the same Conan + CMake flow through three root entry points,
`build.bat`, `run.bat` and `test.bat`: thin shims over `scripts\windows\build.ps1`,
`run.ps1` and `test.ps1`, which run under the Windows PowerShell 5.1 that ships
with Windows. They mirror `build.sh` / `run.sh` / `test.sh` (same options, same
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
