# Release AppImage — embedded (compiled) plugin flavor

The **release CI** (`.github/workflows/linux-appimage-release.yml`, see
[`README.md`](README.md)) bundles the curated plugin set by **downloading the
published marketplace zips** from `pj-plugin-registry`
(`build_appimage.sh --plugins-registry`) — the same artifacts the Windows
installer ships.

This document covers the **local alternative**: compiling the same curated set
from `pj-official-plugins` source and embedding it.
[`build_release_appimage.sh`](build_release_appimage.sh) orchestrates that
full build. Use it when the registry is not an option — offline builds, testing
unpublished plugin changes, or building against a not-yet-released SDK.

## The bundled set

The compiled set is `RELEASE_FLAT_SOS` in `build_release_appimage.sh` plus the ROS 2 bundle. It matches
`BUNDLE_IDS` in `build_appimage.sh` (keep the two in lockstep). `toolbox-assistant-agent` is Linux-only
(POSIX CLI backends).

## Two build flows, tied together

1. **ROS 2 (multi-distro)** — `data_stream_ros2/docker/run-local.sh --bundle` (in `pj-official-plugins`)
   builds a distro-agnostic proxy plus one binary per ROS 2 distro (`--ros2-distros`, default `humble iron
   jazzy rolling`), each compiled in its own Docker image. At load time the proxy dispatches to the binary
   matching the ROS 2 distro installed on the user's machine.
2. **App + non-ROS 2 plugins + package** — the default is `packaging/appimage/build_in_docker.sh --plugins-dir
   <pj-official-plugins checkout>`, which compiles both the app and the aggregate plugin set in the Ubuntu
   22.04 builder image (glibc 2.35), picks up the ROS 2 bundle from step 1, keeps only the curated entries
   (`PJ_INCLUDE_PLUGINS`) and packages the AppImage, so the result is portable. With `--host-build` the app
   (`./build.sh`) and the plugins (the plugin repo's `./build.sh`) are built on the host, the curated `.so`
   files are staged and `build_appimage.sh --plugins-dir` packages them: faster, but the AppImage is tied to
   the host's glibc.

```bash
packaging/appimage/build_release_appimage.sh                       # DEFAULT: docker (portable, glibc 2.35)
packaging/appimage/build_release_appimage.sh --host-build          # opt-in: host build (non-portable)
packaging/appimage/build_release_appimage.sh --skip-ros2           # reuse a previous ROS 2 bundle
packaging/appimage/build_release_appimage.sh --ros2-distros "jazzy"  # restrict the ROS 2 matrix
packaging/appimage/build_release_appimage.sh --fresh               # (docker) wipe the Conan+ccache volumes
```

## Runtime requirement for ROS 2

The `ros2-topic-subscriber` proxy links **no** ROS libraries; it `dlopen`s the per-distro
binary at load time, and that binary links `rclcpp` & friends. So the ROS 2 topic
subscriber only appears (and works) when a supported ROS 2 distribution is installed and
its environment is on the process — i.e. `source /opt/ros/<distro>/setup.bash` before
launching PlotJuggler. With no ROS 2 present the plugin stays silent (this is expected,
not an error); every other bundled plugin works regardless.

## glibc floor / portability

By default `build_release_appimage.sh` runs the app + non-ROS 2 plugin build inside
[`build_in_docker.sh`](build_in_docker.sh) (Ubuntu 22.04 / glibc 2.35), so the released
AppImage runs on any distro with glibc ≥ 2.35 (Ubuntu 22.04 and newer, and equivalents on
other families). ROS 2 inner binaries are always built per-distro in Docker; the proxy
that PlotJuggler loads is built in Ubuntu 22.04, matching the app's glibc floor. Pass
`--host-build` to compile on the host instead — faster for local iteration, but the
resulting AppImage inherits the host's glibc floor (e.g. Ubuntu 24.04 → glibc 2.38).

## Status

`build_release_appimage.sh` is a convenience wrapper over the two flows above, kept for
local/offline embedded builds. The release CI does not use it: it compiles only the app
(inside the thin `ci-base` target of [`Dockerfile.build`](Dockerfile.build), published by
`.github/workflows/appimage-builder-image.yml`) and takes every plugin from the
registry.
