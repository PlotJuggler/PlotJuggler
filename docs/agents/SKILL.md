---
name: plotjuggler
description: Use when the user wants to look at, plot, analyze or debug time-series or robotics log data (MCAP, ROS bags, ULog, CSV, Parquet, live ROS/MQTT/ZMQ streams...) in PlotJuggler, or asks how to do something in PlotJuggler. Drives the running PlotJuggler 4 app through its MCP tools.
---

# PlotJuggler 4 through MCP

PlotJuggler runs on the user's desktop, or in their web browser; its MCP tools act on that window or page. The user
sees everything you create, so work in their app instead of decoding files or plotting with your own scripts. This
guide ships with the PlotJuggler build that serves it: where the tools' own descriptions say more, they are current
too.

## Start

1. Several windows or pages: `list_instances`, ask the user which one, then `select_instance`. None: `launch`.
2. `get_status`: loaded datasets, time range, your tabs, and whether the user has a dialog open.
3. Data not loaded yet: `open_file` with the path. Loader defaults load everything with no dialog; pass `config` to
   choose topics, or `ask_user: true` when the user should pick in the loader dialog (the call returns when they
   press OK or cancel).
4. Live data: `start_stream` with a name from `get_status` `stream_sources` (ROS 2, Foxglove, MQTT...). No options
   uses what the user last set in that source's dialog; pass `config`, or `ask_user: true` to let the user set it up.
   Topics appear while it runs: poll `list_topics`. `stop_stream` keeps the data. Ask before starting a stream the
   user did not mention.

In the browser, `open_file` still takes a path on the agent's computer (the relay hands the file to the page),
`export_series` and `about` do not exist, and the data lives in that tab: closing it loses it.

## Long calls are jobs

- A call that takes long (a large `open_file`, a heavy `evaluate`, an `ask_user` dialog the user has not answered)
  returns after `wait_s` (20 s unless you pass it) with a `job_id` in place of its result, and the work goes on.
  `job_wait` returns the result once it is there, `jobs_list` shows what is running with its progress, `job_kill`
  stops it. A stopped import keeps the rows loaded so far unless you pass `keep_partial: false`; a stopped reload
  restores the data it was replacing.
- `start_stream` returns the `job_id` of a job that lives as long as the stream: `job_kill` or `stop_stream` stops
  it, and the data stays.
- `wait_for` waits for a live series to cross a threshold. Only samples that arrive after the call count: use
  `read_series` for what is already loaded.
- The user sees your running jobs from PlotJuggler's status bar and can stop any of them. A job that reads `killed`
  was stopped, by you or by them: say so, and do not start it again without asking.

## Find and read data

- `list_topics` (with `filter`) and `describe_topic` find paths. A unique suffix of a path resolves; an ambiguous one
  comes back with the candidates. When several datasets hold the same path, name one as `dataset:topic/field`. Do
  not re-verify paths you were just given.
- `describe_topic` also returns the `metadata` a plugin gave a topic or a field: a `description`, a `unit`, and any
  other key the plugin chose. Take units and meanings from it instead of guessing them from a name.
- `read_series`: `stats` for a summary, `buckets` (min/max per bucket) for the shape of a long signal, `raw` for
  samples inside a window you set. Compare channels in ONE call.
- `evaluate`: compute something (scalar series or objects: point clouds, images, TF) without leaving anything
  behind. Objects come back as summaries, never raw data.
- `export_series`: aligned CSV or `.npy` of several series for your own numpy/pandas work, when a computation does
  not fit a script.
- Never invent values: read them.

## Create things the user keeps

All of these appear under **Custom Topics** (bottom-left), are saved with the layout, are untouched by the user's
undo, and are yours to remove (`list_created`, `remove_derived_series`, `remove_markers`, `remove_script_series`). A
removal is refused while the user (or a plugin) has built a series on yours, since it would go too: tell the user
instead of working around it.

- `create_derived_series`: a live Luau transform over one or more inputs. Several inputs join on equal timestamps,
  so inputs that share none (different messages at different rates) are refused; fields of one message always share
  timestamps. `output_type: "string"` makes a text series (each sample returns a label such as "walking", nil skips
  it): shown as coloured states in a State Transitions panel, not plotted, and refused by
  `read_series`/`export_series`. Never feed a text series into a formula: it reads as 0.
- `add_script_series`: the same in Luau **or Python**; a class with `output = "string"` returns text.
- `add_filter`: one of the built-in filters (derivative, integral, moving average, ...).
- `create_markers`: threshold or rule markers on the timeline.
- `create_derived_object`: live object (recomputes with the playhead), or `pin_at_s` to freeze one instant as a
  finding.

The languages these run, with their APIs and examples: [scripting.md](scripting.md). A Python script for
`add_script_series` (first line is mandatory; only `math`, `cmath`, `statistics` import):

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

- `plot_tab` (create/add/remove/zoom/focus/close/list; zoom frames the one tab you name) and `scene_view` (3D/2D)
  open tabs that are YOURS, badge-marked; you cannot draw in or close the user's tabs.
- `widget_state` reads the settings of any dock, the user's included: a point cloud's size and colours, a camera, a
  plot's style, curve colours and ranges, a Log panel's filters. `list` finds the dock, `get` returns its XML and the
  values each attribute accepts. To change them, send the whole element back with your edits through
  `set_widget_state`. A refused change leaves the dock as it was and names the attribute; an accepted one is one
  undo step for the user.
- `save_layout` and `load_layout` read and replace the whole workspace as layout XML; `load_layout` asks the user
  before it replaces a workspace that holds anything.
- `playback` moves the one time cursor every plot shares; move it only when it helps.
- Times are display-axis seconds (the numbers on the plot X axes and in `get_status`).
- `screenshot` to check what the user actually sees before saying it looks right. To show one of your tabs, bring it
  forward first with `plot_tab` / `scene_view` `focus`.

## Etiquette

- A tool saying the user has a dialog open: only read-only tools run. Wait, poll `get_status`.
- `unload_file` may ask the user to confirm; `reload_file` re-reads a file in place.
- Product questions ("how do I show an image?"): call `about`, answer from it and from the pages below, and point to
  the entry point it names. Your training describes PlotJuggler 3; do not answer from it.
- End with a short, plain summary of what you created and where the user finds it.

## Reference

Read a page when the task needs it:

- [concepts.md](concepts.md): datasets, topics, fields and series paths; display versus recording time; the Source
  Timeline and offsets; layouts over several datasets; derived series, objects and markers; streaming; Record and
  the cache.
- [formats.md](formats.md): file and stream types, the plugin each needs, browser availability.
- [scripting.md](scripting.md): built-in filters, the Luau filter class, Python filters, on-demand object scripts
  (`pj.scene`, `pj.cloud`, `pj.annotations`, `pj.voxels`, `pj.tf`, image ops), marker rules.
- How to do each task in the UI, with its exact labels and whether a tool does it:
  [plots](ui_tasks/plots.md), [time and comparing runs](ui_tasks/time.md),
  [layouts, files, undo](ui_tasks/layouts.md), [transforms and Custom Topics](ui_tasks/transforms.md),
  [markers](ui_tasks/markers.md), [live data](ui_tasks/live_data.md), [2D and 3D scenes](ui_tasks/scenes.md),
  [other panels](ui_tasks/other_panels.md), [plugins and Marketplace](ui_tasks/plugins.md),
  [Mosaico](ui_tasks/mosaico.md).
