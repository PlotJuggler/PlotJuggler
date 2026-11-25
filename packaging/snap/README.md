# PlotJuggler 4 snap

Classic snap based on **core26**, configured for native **amd64** and **arm64**
builds.
Arm64 still needs end-to-end validation.

## Build

```bash
sudo snap install snapcraft --classic --channel=latest/edge
packaging/snap/build.sh
```

Run the wrapper from the repository root.
Artifacts go to `packaging/snap/dist/`.

```bash
packaging/snap/build.sh --clean
packaging/snap/build.sh --context /path/outside/checkout
```

The default context is `~/.cache/plotjuggler-snap`.
The wrapper copies the working tree, including `.git` and local edits,
excluding `build/`, `packaging/snap/dist/` and `__pycache__/`.
Existing context files are retained, with a warning.
`--clean` cleans the Snapcraft part, not the context.
Use a new context for a fresh-source check,
and do not build concurrently in the same context.

## Features

- Native amd64 and arm64 packages.
- Bundled Qt shared libraries and complete Qt plugin tree, embedded Python
  runtime, and Conan runtime dependencies.
- Desktop integration, including the application launcher, icon and native
  GTK file chooser.
- Raster helper with shareware game data.
- 20 bundled marketplace extensions covering file loaders, streamers, message
  parsers and tools, including the ROS 2 subscriber.
  Extensions are deployed into the user's store on first launch while preserving
  compatible newer versions.
- Classic confinement for access to host ROS installations, custom-message
  workspaces and external plugins.
- X11 and native Wayland support.

### Limitations

Graphics libraries removed by GPU cleanup must be supplied by the host.
Bundled Qt Oracle, Mimer and MySQL plugins require compatible host client libraries
to make those backends usable.

The snap bundles all Qt shared libraries and plugins downloaded by aqt rather
than maintaining a selected runtime list.
Qt license and notice files are collected from the matching sources and
shipped alongside the upstream third-party notices.

## Install and use

```bash
sudo snap install --dangerous --classic packaging/snap/dist/plotjuggler_*.snap
plotjuggler /path/to/data
#or
snap run plotjuggler /path/to/data
```


## Testing

Run the smoke test to check the installed snap in headless X11 and Wayland
sessions using software rendering:

```bash
bash packaging/snap/tests/smoke.sh
```

The smoke test checks Python, X11 startup, first-launch extension directory
creation and native Wayland rendering with software graphics.
CI runs it on amd64 and arm64.
ROS integration, file dialogs and hardware graphics require separate testing.
