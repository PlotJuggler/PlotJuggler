# Nix build (x86_64 Linux)

The root flake provides a native, sandboxed package, a runnable app, and a matching
CMake development shell. It does not use Conan, pixi, or the downloaded `.qt/` kit.

Use **Nix 2.27 or newer** with `nix-command` and `flakes` enabled. Initialize the
Git submodules before using a local checkout:

```bash
git submodule update --init --recursive
nix build                         # result/bin/plotjuggler4
nix run                           # launch the installed, Qt-wrapped app
nix run . -- --selftest-python     # headless embedded-Python check
nix flake check                   # build + installed-package smoke checks
```

`packages.x86_64-linux.plotjuggler` and `apps.x86_64-linux.plotjuggler` alias the
default outputs, preserving the old flake's names. The executable is `plotjuggler4`.
The package includes the desktop entry, icon, and sibling `pj-plugin-check` helper.
Tests, demos, and the optional GPL raster helper are excluded from the app build.

## Development

```bash
nix develop
pj-configure
cmake --build build/nix --target pj_app --parallel
./build/nix/pj_app/plotjuggler4
```

`pj-configure` is a Bash function supplied by the shell. It selects Ninja, the
same dependencies and offline SDK sources as the package, and `build/nix` as the
build directory. Additional CMake arguments override its defaults:

```bash
pj-configure -DPJ_BUILD_TESTS=ON
cmake --build build/nix --parallel
QT_QPA_PLATFORM=offscreen ctest --test-dir build/nix --output-on-failure --timeout 120
```

Do not use `build.sh` or `run.sh` in place of these commands: those scripts select
the separate Conan/`.qt/` toolchain. Keep the Nix and Conan build directories
separate. New source files must be added to Git before `nix build` can see them;
ordinary CMake builds in the development shell see the working tree directly.

## Dependency policy

- `versions.env` supplies the application version and required Qt release. The
  locked Nixpkgs revision matches that Qt release; evaluation rejects a mismatch.
  When upgrading Qt, select a matching Nixpkgs revision in `flake.nix` and regenerate
  `flake.lock`. Do not relax the check to accept an arbitrary Qt build: PJ4 also
  uses Qt private headers.
- The SDK version comes from `conanfile.txt`, and its source hash comes from the
  root `CMakeLists.txt`. Nix fetches and unpacks those verified bytes, then supplies
  them through `FETCHCONTENT_SOURCE_DIR_PLOTJUGGLER_SDK`.
- Compatible Nixpkgs packages supply the other dependencies where possible.
  Luau's embedding libraries, nanoarrow with IPC/flatcc, Cloudini, MCAP C++ headers,
  and decode-only LGPL FFmpeg reuse the pins in `recipes/`. Their existing CMake
  adapters and FFmpeg build/license/consumer checks are reused rather than copied.
- `inputs.self.submodules` includes the Git-pinned vendored sources. Configure and
  compilation run without network access; only Nix's fixed-output fetches download
  dependency sources. The optional raster helper is not compiled or installed.
- `wrapQtAppsHook` supplies the Qt runtime/plugin paths, including X11 and Wayland.
  OpenGL still needs a working host graphics driver. NixOS provides that integration;
  non-NixOS hosts may need a separate solution such as nixGL.

The packaging checks exercise the installed app's help/version and Python backend.
They do **not** run the full CTest suite or prove hardware-accelerated rendering;
use the development shell for those tests.

## Plugins

Official plugins are maintained in a separate repository and are **not bundled**.
The app retains its Marketplace and plugin-folder controls. Upstream downloadable
plugin binaries are not validated against this Nix closure; on NixOS they may
need their own Nix packaging/runtime dependencies. A working app package does not
imply that every Marketplace binary can be loaded unmodified.

## Packaging references

- [Nix flake source/submodule semantics](https://nix.dev/manual/nix/2.28/command-ref/new-cli/nix3-flake.html)
- [Nixpkgs Qt wrapping](https://github.com/NixOS/nixpkgs/blob/991eb3e01305e9549d0fc5504d034359ae0897a3/doc/languages-frameworks/qt.section.md)
- [CMake offline FetchContent source overrides](https://cmake.org/cmake/help/latest/module/FetchContent.html#variable:FETCHCONTENT_SOURCE_DIR_%3CuppercaseName%3E)
