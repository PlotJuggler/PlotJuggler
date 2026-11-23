# Transforms, filters and Custom Topics

Derived series and objects the user makes in the UI. The script languages: [scripting.md](../scripting.md).

## Apply a built-in filter to a plotted curve

- **UI:** right-click the plot → "Apply Filter...", pick the curve and the filter, set its parameters in the generated
  form. The result is a new series listed under Custom Topics. Details of the panel:
  [plots.md](./plots.md), "Apply a filter".
- **MCP:** `add_filter` (`input`, `filter` id, `params`). Output name defaults to `<input>[<filter>]`.
- **Filters:** derivative, integral, absolute, scale, moving average, moving RMS, moving variance, outlier removal,
  binary filter, samples counter, time since previous ([scripting.md](../scripting.md), "Built-in filters").

## Write a custom series or object (Transform Editor)

- **Open:** Custom Topics panel (lower left) → "+" (`buttonAddCustom`, tooltip "Add custom topic"). The button shows
  only while the Transform Editor plugin is installed. The editor is not in the Toolbox menu.
- **Single Function tab:**
  1. Drag series or object topics from the Datasets panel (or Custom Topics) into the "Inputs" table
     (`tableSources`, columns Input / Var / Type). Var is the name the script reads the input by; double-click a row
     to rename it. With series only, the radio in the first column picks which input is `value`.
  2. Pick the language in the function header: "Lua" (`luaButton`, Luau) or "Python" (`pythonButton`, desktop only).
  3. "Global Variables" (`globalVarsText`): code run once before the first sample.
  4. The function body (`functionText`). The header shows its signature: `function( time, value, v1, ... )` for
     series inputs, `function( <Var1>, <Var2> )` for object inputs.
  5. The preview below runs the script and shows the result, or the error (`line N: ...`), and for object outputs an
     embedded 2D/3D view.
  6. "Advanced >" (object inputs only): "Params (JSON)" (`paramsLineEdit`) handed to the script as `params`, and
     "Pin at current time" (`pinCurrentTimeCheck`) to evaluate once at the cursor and keep that finding.
  7. "Create…" (`pushButtonCreate`) → "Create" dialog → "Name:" → OK. Disabled, with the reason in its tooltip, until
     the preview has run.
- **Outputs** are inferred by running the script: numbers become `name/output` rows in Custom Topics, one object
  becomes `name`, several objects `name/output`. A series function returning several values creates `name/a`,
  `name/b`.
- **Batch Functions tab:** one series function applied to each dropped series separately (`tableBatchSources`), the
  outputs named with a "Prefix" or "Suffix" (`suffixLineEdit`); "Create New Time Series" (`pushButtonCreateBatch`).
- **Function Library:** the box icon (`buttonLibraryBox`, "Browse and manage available functions.") lists built-in
  snippets: series (`backward_difference_derivative`, `central_difference_derivative`, `average_two_curves`,
  `integral`, `rad_to_deg`, `remove_offset`, `quat_to_roll`, `quat_to_pitch`, `quat_to_yaw`) and objects
  (`points_per_frame`, `lidar_crop`, `lidar_crop_map`, `witness_of_crop`, `cam_threshold`, `cam_annotations`,
  `depth_cloud`). "Use" loads the selected one. The save icon stores the current function in the library.
- **"Help"** in the editor's banner opens its own help pages.
- **MCP:** `create_derived_series` (Luau expression or body), `add_script_series` (Luau or Python filter class),
  `create_derived_object` (Luau object script). The editor's Python object scripts have no MCP route.
- **Show an object output:** open a 2D or 3D view ([scenes.md](./scenes.md)) and drag its row from Custom Topics into
  it. An image overlay draws only over its own image, so drag that image into the 2D view too.

## Edit or delete a custom topic

- **Edit:** select one row → pencil (`buttonEditCustom`, "Edit the selected custom topic") reopens the Transform
  Editor on it; the main button reads "Modify".
- **View:** on a row made by an agent or another plugin the pencil reads "View the formula (created by ...;
  read-only)", and on a row with no owner "View the formula (read-only)". A Transform Editor that declares
  `custom_topics_viewer` in its manifest opens on it read-only: the script (an agent's whole generated script), the
  inputs and params, "Read-only: created by ..." on the status line, no Create/Modify button and no preview. With an
  editor that does not declare it the pencil stays disabled and reads "Read-only" or "Created by ...; read-only".
  Marker rules have no view.
- **Delete:** select rows → "⋮" (`buttonCustomMenu`, "Custom topics options") → "Delete". Disabled when a selected row
  is read-only. When other derived topics read it, "Delete derived topic?" lists them; they stay and report a missing
  input. Ctrl+Z undoes a delete.
- **MCP:** an agent removes only its own: `remove_derived_series`, `remove_script_series`. An agent's rows cannot be
  edited or deleted by the user from the panel: ask the agent to remove them.
- **Row state** text: "Computing", "Missing input: ...", "Error: ...", "Pinned at N s". A series computed from objects
  shows a progress bar with a stop glyph.

## Quaternion to roll, pitch, yaw

- **UI:** Toolbox → "Quaternion to RPY". Drop one quaternion component series into the input frame; the other three
  (x/y/z/w siblings) fill in and "Output:" becomes `<prefix>rpy/`. Choose "Radians" / "Degrees" and "Unwrap Angles",
  then "Save". The result is a derived topic with `roll`, `pitch`, `yaw`.
- **MCP:** `create_derived_series` with the four components as inputs and `outputs: ["roll","pitch","yaw"]`
  ([scripting.md](../scripting.md) has the body).

## Frequency spectrum (FFT)

- **UI:** Toolbox → "Fast Fourier Transform". Drag one numeric series onto the input chart; choose "All Data" or "Only
  data in zoomed area", "Remove Average (DC, 0 Hz)", "Window:" ("Hann (recommended)" or "Rectangular"), then
  "Calculate Spectrum". "Export Spectrum Data" writes a dataset `fft_output` with `frequency_hz` and `amplitude`
  columns.
- **Gotchas:** needs evenly sampled data (resample irregular signals first) and at least 8 samples; one input at a
  time.
- **MCP:** none for the spectrum view. `export_series` (desktop) gives the samples for an agent's own FFT;
  `create_markers` `bandPower(s, f_lo, f_hi)` flags energy in a band.

## Export series to CSV or Parquet

- **UI:** Toolbox → "CSV/Parquet Exporter". Drag series into the table (or "Add all"), pick "CSV" or "Parquet",
  "Relative" or "Absolute" time, a time range on the slider, optionally "One file per group" with a "Prefix:", then
  "Export".
- **MCP:** `export_series` (desktop only): one time-aligned CSV or `.npy` file, sample-and-hold, time in display
  seconds.
