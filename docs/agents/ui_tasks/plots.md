# Plots

Labels as the app shows them; `objectName`s in backticks. Plots are drawn in docks inside tabs; an empty dock shows
the panel picker (Plot, 2D, 3D, Log, State Transitions, Custom Widget).

## Plot series over time

- **UI:**
  1. Find the fields in the Datasets panel; type in its "Filter..." box (`filterTimeseries`) to narrow the tree.
  2. Select one or more fields (Ctrl-click, Shift-click) and drag them with the left button onto a plot or an empty
     dock. A press on an already selected row drags the whole selection.
  3. An empty dock becomes a plot and zooms to the data; dropping on an existing plot adds curves to it.
- **Other routes:** the empty dock's "Plot" button (`buttonVizPlot`) makes an empty plot to drop into.
- **MCP:** `plot_tab` (`action: "create"`, then `"add"` with `curves`). It draws only in the agent's own tab.
- **Gotchas:**
  - Only the Datasets panel (and the Custom Topics panel below it) is a drag source: curves cannot be dragged from one
    plot to another. Copy and Paste in the plot's right-click menu copy a whole plot.
  - Dropping only discrete series (text, integer, boolean) on an empty dock makes a State Transitions strip, not a plot
    ([other_panels.md](./other_panels.md), "State Transitions").
  - Object topics (images, clouds, TF, logs) cannot go on a plot; dropped on an empty dock they create the matching
    panel.

## Make an XY plot

- **UI:** select exactly two scalar fields in the Datasets panel and drag them with the **right** mouse button onto an
  empty dock, an empty plot or an existing XY plot. The "New XY Curve" dialog shows "X axis" (`lineEditX`) and "Y
  axis" (`lineEditY`), "Swap" (`pushButtonSwap`) and "Name" (`lineEditName`); OK adds the curve.
- **MCP:** UI only; `plot_tab` adds time-series curves.
- **Gotchas:**
  - The two names are sorted alphabetically; the first becomes X. Use "Swap" to flip.
  - A plot is either time series or XY: time-series curves cannot be dropped on an XY plot and the reverse.
  - XY plots have no tracker line; a dot rides each curve at the playhead. "Keep aspect ratio of XY plots (1:1)"
    (`buttonRatio`, right toolbar) is on by default.

## Tabs and splits

- **New tab:** the "+" at the end of the tab bar (named `tab<N>`).
- **Rename:** double-click the tab; Enter commits, Escape reverts. Rename a dock by double-clicking its title.
- **Duplicate:** right-click the tab → "Duplicate tab" (the only item), giving `<name> (copy)`.
- **Split:** right-click a plot → "Split Horizontally" (new dock to the right) or "Split Vertically" (below), or the
  dock title-bar buttons "Split horizontally" (`buttonSplitHorizontal`) and "Split vertically"
  (`buttonSplitVertical`). Title-bar buttons appear on hover.
- **Maximize a dock:** "Fullscreen" (`buttonFullscreen`) on its title bar; click again to restore.
- **Close a dock:** "Close Plot Area" (`buttonClose`); the tab always keeps one dock.
- **Move a dock:** drag its title bar.
- **Close a tab:** its close button; closing the last plot tab opens a fresh one.
- **MCP:** `plot_tab` creates, zooms, focuses and closes the agent's own tabs only. `widget_state` reads and `set_widget_state` changes any dock's
  settings, the user's too: a plot's curves, colours, style and ranges.

## Zoom and pan

| Gesture | Does |
|---|---|
| Left-drag a box | zoom to the box |
| Ctrl + left-drag, or middle-drag | pan |
| Wheel | zoom both axes about the cursor |
| Hold X (or Y) while wheeling | zoom that axis only |
| Wheel over the bottom (left) axis | zoom X (Y) only |
| Right-click → "Zoom Out", "Zoom Out Horizontally", "Zoom Out Vertically" | fit the data |
| "Zoom Out All" (`buttonZoomOut`, right toolbar) | fit every plot of the tab |

- **Linked X:** "Link X axis" (`buttonLink`, right toolbar, on by default) applies an X zoom or pan to every
  time-series plot and State Transitions strip.
- **MCP:** `plot_tab` `action: "zoom"` with `start_s`/`end_s` (display seconds), on the agent's tab only.
- **Gotchas:** a box smaller than a small fraction of the plot in both directions does nothing.

## Edit curves

The curve editor is in the right panel: focus a plot, then the window title-bar button "Toggle right panel". From
top to bottom:

- **"Curve Width":** "Line width 1.0", "1.5", "2.0", "3.0" (`globalWidth1_0` ...), for the whole plot.
- **"Curve Style":** "Lines", "Dots", "Lines and Dots", "Sticks", "Steps (pre)", "Steps (post)" (`globalStyleLines`
  ...), for the whole plot.
- **"Curves"** list (`listWidget`, "Filter..." box `filterCurves`). Per row: a colour swatch (click to pick a colour),
  the name, "Toggle marker visibility" (`curveMarkersToggle`), "Toggle curve visibility" (`curveVisibilityToggle`),
  "Remove this curve from its plot" (`curveTrashToggle`). The menu button "Curves options" (`buttonCurvesMenu`) holds
  "Clear all curves".
- **Marker rows** at the foot (`markerScopesFooter`): "Dataset markers" and "Global markers", each with a show/hide
  toggle for this plot.
- **Legend:** clicking a legend entry hides or shows that curve; Ctrl + wheel over the legend changes its font size.
- **Remove every curve:** right-click the plot → "Remove ALL curves".
- **MCP:** `plot_tab` `"add"`/`"remove"` on the agent's tab; `set_widget_state` changes the curves, colours and style of
  any plot.
- **Gotchas:** curves cannot be renamed in the list. Width and style apply to the whole plot; only colour is per curve.

## Global plot options (right toolbar)

Always visible on the right edge; each applies to all plots.

| Tooltip | `objectName` | Does |
|---|---|---|
| "Link X axis" | `buttonLink` | link X zoom across plots (default on) |
| "Zoom Out All" | `buttonZoomOut` | fit all plots |
| "Show/Hide the grid" | `buttonActivateGrid` | grid (default off) |
| "Cycle TimeTracker display: ..." | `buttonTimeTracker` | tracker: line only → line + value → line + value + name |
| "Legend position ..." | `buttonLegendPosition` | left-click walks the corners, then hides; right-click shows/hides (default hidden) |
| "Show point in plot" | `buttonShowpoint` | hovering shows the nearest sample (also the 2D pixel inspector) |
| "Keep aspect ratio of XY plots (1:1)" | `buttonRatio` | XY aspect (default on) |
| "Show data point markers on curves" | `buttonDots` | toggle dots on lines |
| "Drop a blue reference line at the playback position; ..." | `buttonReferencePoint` | values shown as delta from that time |

- **MCP:** UI only.

## Read values at a time

- **UI:** move the playhead ([time.md](./time.md), "Move the playhead"); the tracker box lists `time : <t>` and each
  visible curve's value. Shift + left-click (or drag) on a time-series plot also moves the playhead. "Show point in
  plot" adds a hover readout of the nearest sample. The Datasets panel's options → "Show Values" adds a value column.
- **Differences:** "Drop a blue reference line ..." (`buttonReferencePoint`) fixes a reference time; the tracker then
  adds the delta from it.
- **MCP:** `read_series` (statistics, buckets, raw samples) or `evaluate`; `playback` `seek` to show the user.

## Apply a filter

- **UI:** right-click a plot → "Apply Filter..." (time-series plots with at least one curve). The Filter Editor takes
  over the chart area:
  1. "Source curve:" (`series_list`): pick one or more of the plot's curves.
  2. "Transform:" (`transform_list`): "-- No Transform --", "Absolute", "Scale", "Derivative", "Integral", "Moving
     Average", "Moving Root Mean Squared", "Moving Variance / Stdev", "Outlier Removal", "Samples Counter", "Binary
     Filter", "Time Since Previous", plus installed Luau filters.
  3. Set the parameters in the form; the "Preview" chart (`chart_preview`) shows the original and the filtered curve.
     "Copy parameters" / "Paste parameters" / "Copy these parameters into all series" share settings between sources.
  4. "Alias:" (`alias_edit`) names the output when one source is selected (default `<series>[<Filter>]`).
  5. "Apply" (`save_btn`) replaces each source curve in the plot with its filtered output; "Close" (`cancel_btn`)
     discards.
- **Remove a filter:** reopen "Apply Filter...", pick the filtered curve, choose "-- No Transform --", Apply: the plot
  shows the original again.
- **MCP:** `add_filter` (built-in filter id and parameters; ids and parameters in [scripting.md](../scripting.md),
  "Built-in filters"), `remove_derived_series` / `remove_script_series` to remove.
- **Gotchas:** the output is a new series in the Custom Topics panel; the original stays in the Datasets panel. Not on
  XY plots.

## Copy a plot

- **UI:** right-click → "Copy", then right-click another plot (or an empty dock) → "Paste". The plot's curves and
  styles travel as XML on the system clipboard.
- **MCP:** `widget_state` `get` on one plot, then `set_widget_state` of that element on another dock holding a plot.

## Export a plot

- **UI:** no image or CSV export on the plot. For data: Toolbox → "CSV/Parquet Exporter"
  ([transforms.md](./transforms.md), "Export series to CSV or Parquet").
- **MCP:** `export_series` (desktop) for aligned CSV or `.npy`; `screenshot` for an image.
