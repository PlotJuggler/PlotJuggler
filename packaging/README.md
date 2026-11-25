# Packaging

Run packaging commands from the repository root after building the application.
The Nix flake and Snapcraft build and package the app in one step.

| Format | Guide | Command |
| --- | --- | --- |
| Nix (x86_64 Linux) | [Native Nix build](nix/README.md) | `nix build` |
| Classic snap (amd64 Linux) | [Snap packaging](snap/README.md) | `packaging/snap/build.sh` |
| Linux AppImage | [AppImage and Docker builds](appimage/README.md) | `packaging/appimage/build_appimage.sh` |
| Debian / Ubuntu | [Debian package](deb/README.md) | `packaging/deb/build_deb.sh` |
| Windows installer | [Qt Installer Framework](installer/README.md) | `.\packaging\installer\build_windows_installer.ps1` |
| Windows portable ZIP | [Portable Windows package](installer/README.md#portable-zip) | `.\packaging\installer\build_windows_portable.ps1 -StageDir <stage> -OutFile <zip>` |
| macOS disk image (arm64) | [macOS app bundle](macos/README.md) | `packaging/macos/build_macos_bundle.sh --qt-dir .qt/<PJ_QT_VERSION>/macos` |
| Conda (RoboStack) | [Conda package](conda/README.md) | `packaging/conda/build_conda.sh` |
| Browser app (app.plotjuggler.io) | [Hosted browser app](web/README.md) | `node packaging/web/deploy.mjs --package <deployment>` |

AppImages and `.deb` files are written into their respective packaging folders.
Snaps are written to `packaging/snap/dist/`; their isolated build context lives
outside the checkout in the user cache.
The Debian builder consumes `build/AppDir`, produced by the AppImage builder.
The Windows installer writes to `-OutDir` (the current directory by default).

`packaging/deb/publish_apt.sh` pushes a built `.deb` to the apt repository that
users add to `sources.list`; see [Distribution](deb/README.md#distribution).

## Licences

Every desktop package ships PlotJuggler's `LICENSE` and the `THIRD_PARTY_NOTICES.txt`
that `cmake/PjNotices.cmake` writes into `build/pj_app/` at configure time; the
About box shows the same file, embedded in the executable. It holds, for each
third-party component, its licence and its copyright and licence text:

- Qt: the SBOM packages of the modules and plugins the packages deploy
  (`_PJ_QT_DESKTOP_NOTICE_PACKAGES`), and the texts of the licences they name;
  ICU, which Linux kits ship without an SBOM licence, from `cmake/notices/icu4c-73.txt`;
- the vendored code under `3rdparty/` and `resources/`, and Qwt;
- every Conan package, with the `licenses/` folder the package carries plus
  `cmake/notices/conan/<name>.txt` when it exists. Packages CMakeDeps writes no
  file for (compiled into a shared Conan library, such as CPython's OpenSSL and
  FFmpeg's dav1d, or header-only inside assimp) are listed in
  `_PJ_CONAN_HIDDEN_NOTICES` with a committed text. On Linux the configure fails
  when a package of `conan.lock` has neither, so a new dependency needs its text.

Its header states how the LGPL components can be replaced or relinked and
carries the written source offer.

| Package | Where |
| --- | --- |
| AppImage | `usr/share/doc/plotjuggler4/`, with the distribution libraries' `share/doc/<package>/copyright` and their versions in `DISTRIBUTION-PACKAGES.txt` |
| Debian | `/opt/plotjuggler4/share/doc/` (the AppImage's), and `/usr/share/doc/plotjuggler4/copyright` |
| Windows installer and portable ZIP | `licenses/`, with the MPL-2.0 and LGPL-3.0 texts the installer pages show |
| macOS | `PlotJuggler.app/Contents/Resources/` |
| Browser bundle (`PlotJuggler4-web-bundle.zip`) | the root, with the browser build's own notices and its source offer in `sources/` |

The browser build writes its own notices (`cmake/PjWasmNotices.cmake`) and source offer
([docs/WASM_DEPLOYMENT.md](../docs/WASM_DEPLOYMENT.md#third-party-notices)).
Known gap: the bundled plugins' release ZIPs (pj-official-plugins) carry no licence or notice files yet, so
nothing ships for the libraries they link statically.

## Render check

`smoke/render_check.sh <app> [args...]` proves a QRhi build draws, not just
starts. It opens `smoke/render_fixture.pj4.xml` (two plots, a Scene3D view and
a media viewer, bound to the series and the image `--test-data` generates)
with `--screenshot`, and fails unless the app exits 0 (a missing or blank grab
exits non-zero), the Scene3D view and the media viewer both log the expected
QRhi backend, and every plot logs `PJ_WASM_PLOT_RHI_READY` on it. The backend
is `PJ_RENDER_BACKEND`, by default D3D11 on Windows, Metal on macOS and OpenGL
elsewhere. It needs a display and runs under POSIX `sh`, Git Bash included.

| Where | What it runs on | Gate |
| --- | --- | --- |
| `scene3d_rhi_app_smoke` ctest (`pj_app`, QRhi build, Linux, label `qrhi-opengl`) | the build tree's `plotjuggler4` | every QRhi display lane |
| `deb/smoke_test.sh` | the installed `.deb`, Xvfb and native Wayland/Weston + llvmpipe | the `.deb` Release attach |
| `appimage/smoke_test.sh` | the AppImage, Xvfb and native Wayland/Weston in the same container | none: the AppImage is attached before the smoke job runs |
| `windows-release.yml` | the silently installed IFW package, `PATH` = System32, D3D11 on WARP | both Windows package uploads |
| `macos-release.yml` | the app run from the mounted `.dmg`, Metal; also `--selftest-python` | the `.dmg` upload |
| `windows-release.yml` | the extracted portable ZIP before installation, `PATH` = System32, D3D11 on WARP; also `--selftest-python` | both Windows package uploads |

The AppImage smoke running after its upload is deliberate: a smoke failure
must not cost the release its AppImage. A change that affects rendering is
therefore gated by a `workflow_dispatch` of both release workflows on its
branch before it merges.

To regenerate the fixture, open it in a QRhi build with `--test-data`, change
it, and save it with File > Save Layout... (not "with Sources").
