# Time, playback and comparing runs

Labels as the app shows them (tooltips for icon buttons); `objectName`s in backticks. The time model (display versus
recording time, offsets): [concepts.md](../concepts.md), "Time".

## Play, pause, loop, speed

The playback bar at the bottom of the window, left to right:

| Control | `objectName` | Does |
|---|---|---|
| "Use time offset: ..." | `buttonTimeOffset` | display time relative to the earliest loaded sample (axis starts at 0); off = absolute time |
| "Show time as clock time (HH:mm:ss) instead of seconds" | `buttonUseDateTime` | clock labels on plots, readout, Log time column, Source Timeline ruler |
| time readout | `displayTime` | current display time, read-only |
| "Loop" | `playbackLoop` | restart at the end |
| "Start/Stop player (Space)" | `buttonPlay` | play / pause |
| time slider | `timeSlider` | click or drag to seek |
| "Rate (speed multiplier)" | `playbackRate` | playback speed |
| "Step size to move the time tracker ..." | `playbackStep` | step of the slider's keyboard moves |

- **Shortcut:** Space plays and pauses.
- **Rate and Step size** are scrubbers: drag the field to change the value, or click it without dragging to type one
  (Enter commits, Escape reverts).
- **MCP:** `playback` with `action` `play`, `pause`, `rate` (`rate`), `state`. Loop and Step size are UI only.
- **Gotchas:** while a live stream plays, the slider is disabled and playback follows the live edge
  ([live_data.md](./live_data.md)).

## Move the playhead

- **UI:** click or drag the time slider; Shift + left-click (or drag) on a time-series plot; click or drag the ruler
  of the Source Timeline, or drag its pink needle; double-click a row in a Log panel (while paused).
- **MCP:** `playback` `action: "seek"` with `time_s` in display seconds; the reply says where the cursor landed after
  clamping to the data range.
- **Gotchas:** the State Transitions playhead is display-only. Every plot, scene and panel follows the one playhead.

## Show clock time or time since start

- **Clock:** `buttonUseDateTime` switches the time labels to `HH:mm:ss` (the readout adds the date); the time zone,
  local or UTC, is File → Preferences... → Plotting → "Clock time zone:" (`clockTimeZone`). Values too small to be
  epoch times show as an elapsed duration.
- **From zero:** `buttonTimeOffset` subtracts one global origin, the earliest raw sample across all loaded datasets,
  from every dataset. It is one origin for all datasets: two runs recorded hours apart stay hours apart. To start each
  run at zero, align them in the Source Timeline ("Align dataset starts" below).
- **MCP:** UI only. MCP times are always display seconds as currently shown; `get_status` and `read_series` report
  each dataset's range on that axis.
- **Gotchas:** toggling `buttonTimeOffset` keeps every plot showing the same data window; only the numbers change.

## The Source Timeline

The bottom panel ("Toggle bottom panel" on the window title bar shows or hides it) draws one bar per dataset on the
display axis, with the dataset names in a "Datasets" column on the left (`timelineDatasetsLabel`). A rail of buttons
sits to its left.

| Action | How |
|---|---|
| Shift one dataset in time | drag its bar (left button) |
| Shift several rigidly | select their names (Ctrl-click, Shift-click), then drag one of their bars |
| Snap to other bars' edges | "Snap to neighbouring dataset edges while dragging" (`buttonTimelineSnap`, on by default) |
| Zoom the timeline | wheel over it |
| Pan the timeline | drag the empty background |
| Fit all bars | "Zoom out to fit all sources" (`buttonTimelineFit`) |
| Reorder rows | drag a name in the "Datasets" column |
| Seek | click or drag the ruler, or drag the pink needle |

- **MCP:** UI only: no tool reads or sets a dataset's offset. `get_status` and `list_topics` report times after the
  offsets.
- **Gotchas:** the timeline has no context menu. While a live stream plays it is frozen and reads "Streaming: pause
  playback to interact with the timeline". Offsets change display time only, never the stored timestamps; a
  source-bound layout saves them.

## Align dataset starts or ends

- **UI:** rail buttons "Align sources: line every source's start up at the earliest start" (`buttonAlignStarts`) and
  "Align sources: line every source's end up at the latest end" (`buttonAlignEnds`). The earliest-starting (or
  latest-ending) dataset stays where it is; the others move to it.
- **Undo the alignment:** "Reset all timeline changes" (`buttonTimelineReset`) sets every offset back to zero and
  restores the row order. Ctrl+Z undoes a bar drag.
- **MCP:** UI only.
- **Gotchas (known limitation):** Align and Reset move the data, but the plots keep their X window, so a curve can
  slide out of view and a plot can look empty. Press "Zoom Out All" (`buttonZoomOut`, right toolbar) or the plot's
  "Zoom Out" afterwards. This is not a bug.

## Merge datasets

- **UI:** select two or more names in the Source Timeline, then "Merge the selected datasets" (`timelineMergeButton`);
  or right-click selected datasets in the Datasets panel → "Merge". Confirm "Merge datasets" ("Merging datasets is a
  destructive operation...") with "Merge". The merged dataset's bar is amber.
- **MCP:** UI only.
- **Gotchas:** refused for a live stream ("Stop streaming the live source before merging it with other datasets.") and
  when object topics share a name with incompatible types. "Reset all timeline changes" does not undo a merge.

## Compare two runs

Two recordings of the same robot, one plot per signal with both runs overlaid:

1. Open both files: each becomes its own dataset ([layouts.md](./layouts.md), "Open data files").
2. In the Datasets panel drag the same field from each dataset onto one plot.
3. "Align sources: line every source's start up at the earliest start" (`buttonAlignStarts`), or drag one bar until
   the events line up.
4. "Zoom Out All" (`buttonZoomOut`): plots do not follow the alignment by themselves.

- **One layout for both runs, one tab per run:** [layouts.md](./layouts.md), "Compare two runs with one layout".
- **MCP:** `open_file` twice, then `plot_tab` with both qualified paths (`run_a.mcap:/imu/linear_acceleration/x`,
  `run_b.mcap:/imu/linear_acceleration/x`); the alignment itself is UI only. `read_series` on each path compares the
  numbers without a plot.
- **Gotchas:** with "Link X axis" on, zooming one plot zooms all of them, which keeps the runs side by side.
