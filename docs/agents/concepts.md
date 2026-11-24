# Concepts

The model behind the MCP tools and the UI. Exact labels and gestures are in [`ui_tasks/`](./ui_tasks/); the scripting
languages in [scripting.md](./scripting.md).

## Datasets, topics, fields, series

- **Dataset:** one loaded file or one live stream. Each file opened becomes its own dataset, so two runs of the same
  robot are two datasets side by side. `get_status` lists them with an id, a name and a path.
- **Topic:** a named stream inside a dataset (`/imu`, a CSV's table, an MCAP channel). A topic is either **scalar**
  (numeric or text fields stored as columns) or an **object topic** (images, point clouds, TF, scene entities, logs,
  markers...), which plots cannot draw but 2D/3D scenes and other panels can.
- **Field:** one column of a scalar topic (`linear_acceleration/x`). A nested message flattens into `/`-separated
  field paths.
- **Series path:** `topic/field`, for example `/imu/linear_acceleration/x`. When several datasets carry the same path,
  qualify it as `dataset:topic/field`, the dataset named as `get_status` shows it. A unique suffix of a path also
  resolves in the MCP tools.
- **Datasets panel:** the tree on the left listing every dataset with its topics and fields; what the selected plot
  shows is written in semi-bold italic.

Loaded data is read-only: values cannot be edited or deleted and the source file is never modified. A dataset can
only be reloaded, replaced, merged or removed.

## Time

- **Recording time (raw):** the timestamps stored in the data, int64 nanoseconds, usually epoch time.
- **Display time:** the seconds on the plot X axes, the time readout and every MCP tool that takes or returns a time
  (`playback`, `read_series` windows, `export_series`, `create_derived_object` `pin_at_s`). For each dataset:
  `display = raw - dataset offset - global reference`. The dataset offset is what the Source Timeline edits; the
  global reference is the earliest sample across all loaded datasets while the playback bar's "Use time offset"
  button is on, and 0 while it is off (then display time is absolute epoch seconds). The global reference is one
  origin for all datasets, not one per dataset.
- **Clock time display:** a playback-bar button shows times as HH:mm:ss instead of seconds; it changes the labels
  only.
- **Playhead / tracker:** the one time cursor shared by every plot, scene and panel. Playback moves it; so do clicking
  or dragging the time slider and the `playback` tool. Plots draw it as a vertical line with a value box.
- **Script time** differs again: a filter's `t` is seconds since that filter's first sample; marker rules and object
  scripts see nanoseconds on the dataset clock (see [scripting.md](./scripting.md)).

## Source Timeline and comparing runs

The Source Timeline (bottom panel) shows each dataset as a bar on a shared display axis. Dragging a bar changes that
dataset's offset; Align and Reset set the offsets of all datasets at once; Merge combines selected datasets into one.
Offsets move data on the display axis only, never the stored timestamps. Plots keep their X window when offsets
change, so after an align a plot can show an empty stretch until it is zoomed out. This is a known limitation.

## Layouts

A **layout** (`.pj4.xml`) saves the workspace: tabs, splits, plots and their curves and styles, scenes and panels,
derived series and objects with their scripts, and marker rules. It can be saved **bound to its data sources**, so
loading it reopens the same files (with each dataset's Source Timeline offset) and restarts the same streams with
their saved settings, without their dialogs.

A layout made for one dataset, loaded while several loaded datasets carry its topics, asks which datasets it is for.
One choice pins it to that dataset; several duplicate each tab, one copy per dataset named `<tab> [<dataset>]`, with
each curve in the same colour in every copy and the chosen datasets aligned to start together. A layout holding a
transform or an object script can show one dataset at a time.

Undo and redo (keyboard only: Ctrl+Z, Ctrl+Shift+Z or Ctrl+Y) cover the user's workspace edits. Whatever an agent
creates (its tabs, derived series, objects, marker sets) is outside the user's undo history, so the user's undo never
removes it.

## Derived series, derived objects, markers

| What | Engine | Output | MCP | UI |
|---|---|---|---|---|
| Derived series (filter, transform) | per arriving sample, stateful | a new scalar topic | `add_filter`, `add_script_series`, `create_derived_series` | Apply Filter..., Transform Editor |
| Derived object (on-demand) | at the requested time, stateless | object topics, numbers, strings | `create_derived_object`, `evaluate` (no output kept) | Transform Editor |
| Finding | a derived object evaluated once at one time and kept | as above, pinned | `create_derived_object` with `pin_at_s` | none |
| Markers | once over whole series | events, regions, value bands drawn on plots | `create_markers` | Marker Editor |

- **Custom Topics panel:** the tree below the Datasets panel listing every derived series and object, with its state;
  rows made by an agent or a plugin carry its badge. The Transform Editor (the "+" there) adds and edits the user's own.
- All of these are saved with the layout, script included, and recompute when their inputs change, reload or stream.
- A derived object computes only when something wants it: a scene layer showing it, a plotted number output, an
  explicit evaluation. Moving the playhead reruns it only when a resolved input changes.
- Removing a series that someone else's series reads is refused: the removal would cascade.

## Streaming

- A **streaming source** (ROS 2, MQTT, ZeroMQ, UDP, Foxglove bridge, PlotJuggler Bridge, ...) is a plugin; starting
  one creates a live dataset that grows while it runs. `stop_stream` stops it and keeps the data; in the UI, removing
  the live dataset stops the stream and deletes its data.
- **Retention:** the buffer length (seconds) on the Stream page keeps the newest seconds of each stream and drops
  older samples. A stateful filter sees every sample before it is dropped.
- **Pause** keeps receiving into a side buffer while the display is frozen; resuming merges it.
- **Demand:** with some sources a topic is subscribed only once it is shown in a view; the topic's right-click menu can
  force streaming of a whole topic first.
- While streaming, playback follows the live edge of the stream launched last.

## Record and the source cache

- **Record** writes what the active streams receive into standard MCAP files, one per stream, in a timestamped folder
  under the recordings folder (Preferences → Recording). Only subscribed topics are recorded, and only sources that
  pass raw messages to PlotJuggler; a stream started after Record is pressed is not recorded. The live session keeps
  running; the recording is opened later like any MCAP.
- **Source cache:** downloads from cloud connectors are captured as MCAP files, so a layout bound to a download
  restores it offline from the cache instead of downloading again. Its folder, budget and the "Capture downloads for
  offline restore" switch are on Preferences → Recording. Recordings are never evicted; the cache is.
- Neither exists in the browser build.

## Plugins

Data loaders, streaming sources, message parsers, toolboxes and Custom Widgets are plugins (extensions), installed
from the Extensions Marketplace. A file format or a stream needs its loader or streamer **and** a parser for its
message encoding when the payloads are encoded messages (ROS CDR, Protobuf, JSON...). Which plugin a format needs:
[formats.md](./formats.md). Enabling, disabling or adding a plugin folder takes effect after a restart.

## Desktop and browser

The same app runs as a desktop program and in the browser (`app.plotjuggler.io`), with a smaller built-in plugin set,
no recording, no source cache, no Python filters and no `export_series` or `about` tools. Data opened in the browser
lives in that tab. Details: [formats.md](./formats.md).

## Agents' own views

An agent draws only in tabs and scene views it creates (`plot_tab`, `scene_view`), marked with its badge. It changes
the user's docks only through `set_widget_state` (one dock's settings, one undo step) and `load_layout`, which
replaces the whole workspace after the user agrees to it. The user can duplicate an agent's tab to get a copy that is theirs. PlotJuggler can do only
receiving and displaying: it cannot publish messages or command a robot.
