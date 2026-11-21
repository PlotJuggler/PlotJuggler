# Markers (Marker Editor)

Plot markers: events (a point or vertical line), regions (shaded time spans, including a background coloured by a
series' value) and value bands. The rule language: [scripting.md](../scripting.md), "Marker rules".

## Write a marker rule

- **Open:** Toolbox → "Marker Editor" (plugin `toolbox-anomaly-detector`). It opens with no source selected.
- **Steps:**
  1. **Source** (left column): pick a series from `source_list` (filter "Filter series...") or drag one from the
     Datasets panel onto it. Only numeric and boolean series are listed; text series are not.
  2. **Function** (middle): pick a template from `function_list`, or "-- No function --" for a blank rule. Templates:
     "Showcase (all markers)", "Severity colors (lines)", "Color map (region)", "Threshold (line)", "Out of range
     (region)", "Spike (point)", "Incoherent point", "Flatline (region)", "Rate of change (line)", "Limit lines
     (horizontal)", "Limit band", "Spectral band power (vibration)", "Boolean flag (edges)". Each has its constants
     (`TH`, `LO`/`HI`, ...) at the top to tune.
  3. **Lua rule** (right): edit the code in `code_editor`. The preview chart above shows the detected markers live; a
     broken rule shows "Rule error: ..." in the status line.
  4. **Scope**, in the Lua rule band: "Timeseries" (`scope_timeseries`, markers on the selected source series only),
     "Dataset" (`scope_dataset`, every plot of the dataset), "Global" (`scope_global`, every plot of every loaded
     dataset).
  5. "Apply" (`apply_button`). The status reads "Done: ..." or "Error: ...". The rule then recomputes by itself when
     the data changes or reloads.
- **Save / load a rule file:** "Save rule as..." / "Load rule..." write and read a portable JSON rule (`version`,
  `name`, `description`, `rule` with `code`, `source`, `fail_on`). The headless `anomaly_runner` CLI runs the same
  file in CI.
- **MCP:** `create_markers`, raw form (`inputs`, `rule`, `output`) or template form (`series`, `comparison`,
  `threshold`). `output: "__global__"` matches the Global scope. One marker set per agent; a new call replaces it;
  `remove_markers` clears it.
- **Gotchas:**
  - Picking a new source re-targets an untouched template; after a hand edit or a load, the rule is kept and its
    `series("...")` / `when("...")` names must be changed by hand.
  - A Global rule draws on every loaded dataset at its display time: on a dataset not aligned in the Source Timeline
    its markers can fall outside that dataset's data.
  - Rules are saved with the layout. Each Apply is one undo step.

## Colour the background by a series' value

- **UI:** Marker Editor → Function "Color map (region)", set the source, adjust the value-to-colour table, choose
  scope "Dataset" or "Global" to shade every plot, Apply.
- **MCP:** `create_markers` with a rule such as
  `when("/robot/mode"):backgroundColor({[0]="gray", [1]="green", [2]="red"})` and `output: "__global__"`.

## Show, hide and refresh markers on a plot

- **Toggle:** a series-scoped rule's markers follow its curve's "Toggle marker visibility" button in the plot's curve
  editor; Dataset and Global markers have their own rows, "Dataset markers" and "Global markers", at the foot of that
  editor ([plots.md](./plots.md), "Edit curves").
- **Manual refresh:** with File → Preferences... → Plotting → "Derived markers:" set to "Manual", a changed rule only
  flags itself; right-click the dataset in the Datasets panel → "Refresh derived" re-runs it. "Automatic" re-runs at
  the "Reload rate:".
- **MCP:** none for visibility; `create_markers` again re-runs an agent's rule.
