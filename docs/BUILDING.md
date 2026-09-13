# Building PlotJuggler 4

[← Back to the README](../README.md)

PJ4 uses **C++20, CMake and Conan 2**, with Qt pinned in
[`versions.env`](../versions.env). The commands below follow the repository's
Linux build scripts; Windows and packaging references are at the end.

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

## Windows, packaging and browser builds

| Target | Instructions |
| --- | --- |
| Windows development | [Windows CI workflow](../.github/workflows/windows-ci.yml), including MSVC, Qt and Conan setup |
| Windows installer | [Installer guide](../packaging/installer/README.md) |
| Linux AppImage / Docker build | [AppImage guide](../packaging/appimage/README.md) |
| Debian / Ubuntu package | [Debian packaging guide](../packaging/deb/README.md) |
| WebAssembly | [Browser build and deployment guide](WASM_DEPLOYMENT.md) |

For Qt toolchain details, see [Qt notes](QT_NOTES.md).
