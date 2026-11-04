# Packaging

Run packaging commands from the repository root after building the application.
The Nix flake is the exception: it builds and packages the app in one step.

| Format | Guide | Command |
| --- | --- | --- |
| Nix (x86_64 Linux) | [Native Nix build](nix/README.md) | `nix build` |
| Linux AppImage | [AppImage and Docker builds](appimage/README.md) | `packaging/appimage/build_appimage.sh` |
| Debian / Ubuntu | [Debian package](deb/README.md) | `packaging/deb/build_deb.sh` |
| Windows installer | [Qt Installer Framework](installer/README.md) | `.\packaging\installer\build_windows_installer.ps1` |
| Windows portable ZIP | [Portable Windows package](installer/README.md#portable-zip) | `.\packaging\installer\build_windows_portable.ps1 -StageDir <stage> -OutFile <zip>` |
| Conda (RoboStack) | [Conda package](conda/README.md) | `packaging/conda/build_conda.sh` |
| Browser app (app.plotjuggler.io) | [Hosted browser app](web/README.md) | `node packaging/web/deploy.mjs --package <deployment>` |

AppImages and `.deb` files are written into their respective packaging folders.
The Debian builder consumes `build/AppDir`, produced by the AppImage builder.
The Windows installer writes to `-OutDir` (the current directory by default).

`packaging/deb/publish_apt.sh` pushes a built `.deb` to the apt repository that
users add to `sources.list`; see [Distribution](deb/README.md#distribution).

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
| `deb/smoke_test.sh` | the installed `.deb`, Xvfb + llvmpipe | the `.deb` Release attach |
| `appimage/smoke_test.sh` | the AppImage, same container | none: the AppImage is attached before the smoke job runs |
| `windows-release.yml` | the silently installed IFW package, `PATH` = System32, D3D11 on WARP | both Windows package uploads |
| `windows-release.yml` | the extracted portable ZIP before installation, `PATH` = System32, D3D11 on WARP; also `--selftest-python` | both Windows package uploads |

The AppImage smoke running after its upload is deliberate: a smoke failure
must not cost the release its AppImage. A change that affects rendering is
therefore gated by a `workflow_dispatch` of both release workflows on its
branch before it merges (the QRhi cut-over is proven that way).

To regenerate the fixture, open it in a QRhi build with `--test-data`, change
it, and save it with File > Save Layout... (not "with Sources").
