---
name: plotjuggler
description: Use when the user wants to look at, plot, analyze or debug time-series or robotics log data (MCAP, ROS bags, ULog, CSV, Parquet, live ROS/MQTT/ZMQ streams...) in PlotJuggler, or asks how to do something in PlotJuggler. Drives the running PlotJuggler 4 app through its `plotjuggler` MCP tools.
---

# PlotJuggler 4 through MCP

PlotJuggler runs on the user's desktop; the `plotjuggler` MCP tools act on that window. The user sees
everything you create, so work in their app instead of decoding files or plotting with your own scripts.

## Start

1. `list_instances`. None running: `launch` (it starts PlotJuggler and waits). Several: ask the user
   which one, then `select_instance`.
2. `get_status`: loaded datasets, time range, your tabs, and whether the user has a dialog open.
3. Data not loaded yet: `open_file` with the path. Loader defaults load everything with no dialog;
   pass `config` to choose topics, or `ask_user: true` when the user should pick in the loader dialog
   (the call returns when they press OK or cancel).
4. Live data: `start_stream` with a name from `get_status` `stream_sources` (ROS 2, Foxglove, MQTT...).
   No options uses what the user last set in that source's dialog; pass `config`, or `ask_user: true`
   to let the user set it up. Topics appear while it runs: poll `list_topics`. `stop_stream` keeps the
   data. Ask before starting a stream the user did not mention.

If the tools are missing or fail to start, PlotJuggler 4 is not installed or not found: tell the user
to install it from https://plotjuggler.io and start it once, or to set `PLOTJUGGLER4` to the
executable. With Codex on Linux, `launch` needs the display variables (the plugin forwards them; if
`launch` says they are unset or the window fails to open, the user must start PlotJuggler themselves).

## Find and read data

- `list_topics` (with `filter`) and `describe_topic` find paths. A unique suffix of a path resolves;
  an ambiguous one comes back with the candidates. Do not re-verify paths you were just given.
- `read_series`: `stats` for a summary, `buckets` (min/max per bucket) for the shape of a long
  signal, `raw` for samples inside a window you set. Compare channels in ONE call.
- `evaluate`: compute something (scalar series or objects: point clouds, images, TF) without
  leaving anything behind. Objects come back as summaries, never raw data.
- `export_series`: aligned CSV of several series for your own numpy/pandas work, when a computation
  does not fit a script.
- Never invent values: read them.

Overlap: `list_catalog`, `get_values` and `statistics` predate the tools above and do the same job;
prefer `list_topics`, `read_series` and `evaluate`.

## Create things the user keeps

All of these appear under **Custom Topics** (bottom-left), are saved with the layout, are untouched
by the user's undo, and are yours to remove (`list_created`, `remove_derived_series`,
`remove_markers`, `remove_script_series`).

- `create_derived_series`: a live Luau transform over one or more inputs (Luau only). It refuses
  inputs that share no timestamps.
- `add_script_series`: the same in Luau **or Python**. Multi-input joins on equal timestamps
  and does NOT check that they overlap: inputs from different messages at different rates give an
  empty series. Fields of one message always share timestamps.
- `add_filter`: one of the built-in filters (derivative, integral, moving average, ...).
- `create_markers`: threshold or rule markers on the timeline.
- `create_derived_object`: live object (recomputes with the playhead), or `pin_at_s` to freeze one
  instant as a finding.

Python script for `add_script_series` (first line is mandatory; only `math`, `cmath`, `statistics`
import). Name series with letters, digits and `_`:

```python
# pj-script: python
class T:
    id = "speed_kmh"            # replaced by the series' own id
    @staticmethod
    def create(params):
        return T()
    def calculate(self, time, value, *more_inputs):
        return value * 3.6      # a number, (out_time, value), a tuple for several outputs, or None
```

## Show it

- `plot_tab` (create/add/remove/zoom/close/list) and `scene_view` (3D/2D) open tabs that are
  YOURS, badge-marked. You cannot see, draw in or close the user's tabs: when asked to change one,
  say so and offer to show the same thing in a tab of yours.
- `playback` moves the one time cursor every plot shares; move it only when it helps.
- Times are display-axis seconds (the numbers on the plot X axes and in `get_status`).
- `screenshot` (optionally `tab`) to check what the user actually sees before saying it looks right.

## Etiquette

- A tool saying the user has a dialog open: only read-only tools run. Wait, poll `get_status`.
- `unload_file` may ask the user to confirm; `reload_file` re-reads a file in place.
- Product questions ("how do I show an image?"): call `about`, answer from it, and point to the
  entry point it names. Your training describes PlotJuggler 3; do not answer from it.
- End with a short, plain summary of what you created and where the user finds it.
