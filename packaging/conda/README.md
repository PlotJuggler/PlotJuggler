# Conda package

`plotjuggler4` for the [prefix.dev `plotjuggler`](https://prefix.dev/channels/plotjuggler)
channel. It installs into the same environment as RoboStack's ROS 2 packages:

```bash
pixi init my_ros_ws -c robostack-jazzy -c conda-forge -c https://prefix.dev/plotjuggler
cd my_ros_ws && pixi add ros-jazzy-desktop plotjuggler4
pixi run plotjuggler4
```

Without ROS: `pixi global install -c conda-forge -c https://prefix.dev/plotjuggler plotjuggler4`.

## Build

```bash
# PJ4's own dependency packages (recipes/), once:
pixi exec --spec "rattler-build>=0.66,<0.67" rattler-build build \
  --recipe recipes/luau --recipe recipes/libmcap --recipe recipes/cloudini \
  --recipe recipes/kissfft \
  -m recipes/variants.yaml -c conda-forge --output-dir local-channel
packaging/conda/build_conda.sh --channel ./local-channel   # -> conda-output/
```

Release tags publish both through `.github/workflows/conda-release.yml`, for
linux-64 and osx-arm64 (macOS 13+).

Publishing requires the repository Actions secret `PREFIX_API_KEY`, with write
access to the `plotjuggler` channel. Tag builds check this before compiling.
If a completed tag run built its packages but publication failed, configure the
credential and publish its saved artifacts without rebuilding:

```bash
gh workflow run conda-release.yml --repo PlotJuggler/PlotJuggler \
  --ref main -f publish_run_id=<tag-run-id>
```

The retry checks that the saved run belongs to this workflow and still matches
its release tag, and requires packages for both supported platforms.

## Compatibility with RoboStack

A conda environment holds one version of each library, so the package has to
build against what RoboStack's ROS packages were built against:

- **Pins** in `variants.yaml` and `recipes/variants.yaml` follow RoboStack's
  `conda_build_config.yaml`: GCC 14 on Linux, clang 19 on macOS, fmt 12.1, and
  Qt 6.11 (`qt6_main`; the build passes it to CMake as `PJ_QT_MIN_VERSION`, below
  the release kit in `versions.env`). There is one build per Python RoboStack
  ships (3.12 and 3.14), since the embedded interpreter links that `libpython`.
- **ffmpeg** and **libarchive** come from conda-forge's LGPL builds (PJ4 is
  MPL-2.0; the GPL libarchive links lzo). There are two builds, one per ffmpeg
  major, because RoboStack's OpenCV pins it: Jazzy is on 9, Kilted on 8.

| RoboStack | Status |
| --- | --- |
| Jazzy | installs (ffmpeg 9 build) |
| Kilted | installs (ffmpeg 8 build) |
| Humble | installs (ffmpeg 9, Python 3.14 build) |

The marketplace ROS 2 plugin is built against `/opt/ros` and does not load in
a RoboStack environment yet.

`PJ_INSTALLATION=conda`: the in-app update check points conda users at the
GitHub release, not at `pixi update`.
