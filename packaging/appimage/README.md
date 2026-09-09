# PlotJuggler 4 — AppImage packaging

Builds a single relocatable `PlotJuggler-<version>-<arch>.AppImage` bundling the
`plotjuggler4` shell, its Qt 6 + Conan runtime, and (optionally) a set of plugins.
The default app version, Qt version, and AppImage arch come from repo-root
`versions.env`; release builds may override `PJ_VERSION` with the tag.

The `build/AppDir` this produces is also the payload of the Debian package — see
[`packaging/deb/README.md`](../deb/README.md). Anything added to the AppDir ships in both
artifacts, so check that side too when changing what gets bundled.

`smoke_test.sh` runs the finished AppImage on a machine that did not build it
(`--selftest-python` + a GUI startup under Xvfb) — release CI runs it in the
same clean containers as `packaging/deb/smoke_test.sh`, right after it, so the runtime
libraries come from the `.deb`'s `Depends`. See the header comment for
standalone use.

## Build

```bash
./scripts/install_qt6.sh        # once: Qt into ./.qt
./build.sh              # builds build/pj_app/plotjuggler4 + Conan runtime env

packaging/appimage/build_appimage.sh                       # app-only AppImage
packaging/appimage/build_appimage.sh --plugins-dir <path>  # bundle a local plugin folder
packaging/appimage/build_appimage.sh --plugins-registry    # bundle the official set (CI default)
packaging/appimage/build_appimage.sh --commit-hash <hash>  # append .<hash> to the filename
packaging/appimage/build_appimage.sh --retro-wad <path>    # bundle the retro payload (CI default)
```

Output lands at `packaging/appimage/PlotJuggler-<version>-<arch>.AppImage`, or
`packaging/appimage/PlotJuggler-<version>-<arch>.<hash>.AppImage` with `--commit-hash`
(release CI's workflow_dispatch/non-tag builds, so otherwise-identical-looking
dev artifacts stay distinguishable — matches the Windows installer's default
naming; tag builds omit it).

## Plugins

Plugins are **not** part of this repo. They are built and published separately by
[`pj-official-plugins`](https://github.com/PlotJuggler/pj-official-plugins) as
self-contained, per-extension marketplace zips (each = the plugin `.so` + its
`manifest.json`, with heavy dependencies static-linked), and indexed for download
by the [`pj-plugin-registry`](https://github.com/PlotJuggler/pj-plugin-registry).
Because the `.so`s are self-contained, bundling is just *copy/unpack* — no
dependency deployment.

There are two ways to bundle them (and a no-plugin default):

- **`--plugins-dir <path>`** — copy a folder you curated, verbatim, into
  `usr/lib/plotjuggler/plugins/`. Use this for local/offline builds; the folder
  must contain ready-to-run (self-contained) plugins, e.g. unpacked marketplace
  zips, each in its own subdirectory.
- **`--plugins-registry [url]`** — download the curated set (`BUNDLE_IDS` in
  `build_appimage.sh`) from the plugin registry, verify each `sha256` checksum,
  and unpack into `usr/lib/plotjuggler/plugins/<id>/`. Defaults to the registry's
  `development` branch (its default branch — the same ref the Windows installer
  uses); pass a URL to pin a specific ref. This is what the release CI uses.
- **(no flag)** — app-only AppImage. Users add plugins later via the in-app
  marketplace or `--plugin-dir`.

### Curated set (`--plugins-registry`)

The registry lists every official extension; the AppImage bundles this subset
(`BUNDLE_IDS` — same set as the Windows installer's `$PluginIds`, plus the
Linux-only `ros2-topic-subscriber`):

`csv-loader`, `mcap-loader`, `parquet-loader`, `ulog-loader`, `mp4-loader`,
`pointcloud-3d-loader`, `dummy-streamer`, `foxglove-bridge`,
`plotjuggler-bridge`, `webrtc-client`, `ros-parser`, `protobuf-parser`,
`json-parser`, `data-tamer-parser`, `toolbox-quaternion`,
`toolbox-transform-editor`, `toolbox-mosaico`, `ros2-topic-subscriber`.

Not bundled: `toolbox-colormap`, `toolbox-reactive-scripts-editor` (excluded by
request); `toolbox-fft`, `mqtt-subscriber`, `udp-server`, `zmq-subscriber`,
`lerobot-loader` (published, but not curated — installable via the in-app
marketplace).

### Where plugins live, and why

Bundled plugins go under `usr/lib/plotjuggler/plugins/<id>/` — the FHS-correct
bucket for arch-dependent `.so` code, and the exact path the installed
`plotjuggler4` resolves relative to itself (`usr/bin/plotjuggler4` →
`../lib/plotjuggler/plugins`). The app never loads plugins from there directly:
at startup it **seeds** that dir into the writable per-user extensions dir
(copying new ids, refreshing ids whose bundled version is newer) and loads
everything from the extensions dir — so marketplace install/uninstall works
normally and the read-only bundle stays a seed source. Kept out of
`usr/plugins/` so the seed's recursive plugin scan does not collide with Qt's
own platform plugins that `linuxdeploy-plugin-qt` deploys there. `AppRun.sh`
does **not** pass `--plugin-dir`; that flag stays a user-facing option,
forwarded verbatim if the user supplies one.

## Retro payload (`--retro-wad <path>`)

`3rdparty/retro/` inside the bundle holds an **independent, separately
licensed** program that PlotJuggler launches as a child process and links none
of: the GPLv2 `pj-raster-helper` (built from the vendored `doomgeneric`), its
game data as `base.wad`, and the license texts that must travel with them
(`COPYING`, `SOURCE-OFFER.txt`, `SHAREWARE-LICENSE.txt`, `README.md`). Only the
in-app trigger is hidden — the licenses are not.

It sits next to the app binary (`usr/bin/3rdparty/retro/`) because that is
where `MainWindow::openEmbeddedConsole` looks (`applicationDirPath()`), which
also means `packaging/deb/build_deb.sh` — which copies `usr/bin` wholesale — inherits it.

Two independent opt-ins, and the WAD is not in this repo:

```bash
PJ_BUILD_RASTER_HELPER=ON ./build.sh              # compile the helper
packaging/appimage/build_appimage.sh --retro-wad /usr/share/games/doom/doom1.wad
```

Release CI installs Ubuntu multiverse's `doom-wad-shareware` for that file,
pins its `sha256`, and re-checks it inside the finished AppImage. Omit the flag
and no retro payload ships.

## Build & verify in Docker

`packaging/appimage/build_in_docker.sh` builds the AppImage in the fully-baked builder
image (`Dockerfile.build`). It defaults to `PJ_BUILD_TESTS=OFF PJ_BUILD_DEMOS=OFF
PJ_BUILD_WIDGET_DEMOS=OFF` and `PJ_BUILD_TARGET=pj_app` (`--target pj_app`), so only
the shipped app and its dependencies, including `pj-plugin-check`, are built.
A fresh packaging tree is **~0.95 GB** and contains only the app closure
(**5 executables**). Packaging defaults to `PJ_DEBUG_INFO=none`; the AppImage
ships without DWARF.

The image ships Conan static dependency archives with debug info stripped; use
`REBUILD_IMAGE=1` to refresh the baked cache.

Pass `--with-tests` for a dev/CI tree: it sets `PJ_BUILD_TESTS=ON PJ_BUILD_DEMOS=ON
PJ_BUILD_WIDGET_DEMOS=ON` and `PJ_BUILD_TARGET=all`, building the full test suite
and demos for a later `ctest` run inside the builder image. It compiles the tests
but does not run them. Per-module gtest runners use `gtest_discover_tests` to
register ctest names `<runner>.<Suite>.<Case>`, with one process per case;
`ctest -R '^pj_runtime_tests\.' --test-dir build` filters the runtime module.
For the tests-on tree size, see workspace findings
`2026-09-08-build-size-reduction-results.md`.

`--with-tests` defaults to `PJ_DEBUG_INFO=split` and `PJ_COMPRESS_DEBUG=ON`, matching
the Linux developer defaults in `build.sh`. Both environment knobs are overridable:
`PJ_DEBUG_INFO=none|lines|full|split` selects no debug info, minimal line/backtrace
info, full DWARF, or DWARF in `.dwo` sidecars kept in the build tree;
`PJ_COMPRESS_DEBUG=ON|OFF` controls zlib compression. The policy lives in
[`cmake/PjDebugInfo.cmake`](../../cmake/PjDebugInfo.cmake).

Linux builds with GNU/Clang compilers use thin archives to avoid copying object
files, and `resources.qrc` is compiled once as `pj_resources`.
[`scripts/build_tree_size.sh`](../../scripts/build_tree_size.sh) `<build-dir>`
reports bytes by artifact class in decimal GB. Use a fresh build tree when
measuring the smaller default; disabling targets does not remove artifacts from
an existing tree.

Plugins:

- `--plugins-registry` — bundle the official set from the plugin registry.
- `--plugins-dir <dir>` — `<dir>` may be a **pj-official-plugins source repo**
  (compiled *inside* the builder at the container's glibc 2.35, so the `.so`
  actually `dlopen` on the runtime image / older distros — host-built plugins
  link a newer glibc and silently fail to load), or a directory of prebuilt
  self-contained plugins (copied verbatim). A source build runs the single
  aggregate `./build.sh`, which on Linux compiles all plugins including
  `toolbox_mosaico` in one pass (Arrow built once with Flight + gRPC), so Mosaico
  is included without a second standalone build. Any host path works — it is
  bind-mounted for you, no manual staging.

`packaging/appimage/run_in_docker.sh [--help]` verifies the built AppImage on a clean
`ubuntu:22.04` runtime image (`Dockerfile.run`) with only base X/GL libraries;
the GUI is forwarded to the host display via `xhost`.

`packaging/appimage/build_appimage_in_docker.sh` is a lighter alternative for local
packaging: it builds a plain `ubuntu:22.04` image (`docker/Dockerfile`), then
runs `scripts/install_qt6.sh` (reusing the host `./.qt`), `build.sh`, and
`build_appimage.sh` inside it — matching the release glibc/libstdc++ baseline
regardless of the host distro. Being a packaging build, it configures with
`PJ_BUILD_TESTS=OFF` / `PJ_BUILD_DEMOS=OFF` (neither the test suite nor the
scene3D dev demos ship). Arguments are forwarded to `build_appimage.sh`:
`--plugins-registry`, or `--plugins-dir <dir>` where a relative `<dir>` is
resolved against your current directory and must live inside the repo tree (only
the repo is bind-mounted into the container). Conan/ccache caches persist across
runs in host-side `*-appimage-docker/` directories, so rebuilds are incremental.

## CI

`.github/workflows/linux-appimage-release.yml` (the release build) compiles the
app inside the `pj4-appimage-builder` container (glibc 2.35 floor), bundles the
curated plugin set with `--plugins-registry` and the retro payload with
`--retro-wad`, audits the result's glibc needs,
and — on a version tag (e.g. `3.9.1`, no leading `v`) — attaches the AppImage to the GitHub Release
(`workflow_dispatch` builds an artifact only).

## Multi-distro ROS 2

A **single AppImage supports multiple ROS 2 distros** (humble / iron / jazzy /
rolling) through the `ros2-topic-subscriber` extension. `pj-official-plugins`
builds it as one `linux-x86_64` zip: a distro-agnostic **proxy** (links no ROS,
self-describing so plugin discovery works on machines with no ROS installed)
plus per-distro inner libraries under `dist/<distro>/`. At runtime the proxy
detects the user's distro (`$ROS_DISTRO` → `/opt/ros/<distro>` →
`$CONDA_PREFIX/share/<distro>`) and `dlopen`s the matching inner, which binds to
the user's **sourced system ROS** (the inners are intentionally *not*
self-contained — they must speak to the live ROS graph).

The AppImage needs no special handling for this: registry-mode unpacks the zip
into `usr/lib/plotjuggler/plugins/<id>/` verbatim, preserving the
`dist/<distro>/` layout the proxy resolves relative to itself. The release CI's
glibc audit exempts the per-distro inners (they are built in each distro's own
container and only ever `dlopen`-ed on machines running that distro).

## How it differs from PlotJuggler 3

PJ3 linked the system Qt 5, so `linuxdeploy-plugin-qt` found Qt on system paths.
PJ4 links Qt from `./.qt` and most external dependencies from Conan, so the build
script points `linuxdeploy` at both (`QMAKE`, `PATH`, and sourcing
`build/conanrun.sh` for the Conan library closure).

## Known limitations

1. **Qt-module coverage for plugins is not separately analyzed.** The Qt plugin
   deploys modules based on `plotjuggler4`; a plugin needing a Qt module that the
   app does not pull would be missed. None observed yet; revisit if a plugin
   fails to load with a missing-`libQt6*` error.
