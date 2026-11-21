# Log, State Transitions and Custom Widget panels

Labels as the app shows them (tooltips for icon buttons); `objectName`s in backticks. Each panel is created from an
empty dock's picker or by dropping a topic on an empty dock. Right-click any of them for "Open Settings" / "Close
Settings", "Copy", "Paste", "Split Horizontally", "Split Vertically" and "Clear" (back to the empty dock).

## Log

Log topics (entries with a time, a level and a message, as a loader or a parser produces them) merged by time into one
list.

- **Open:** "Log" (`buttonVizLog`) in an empty dock, then drop a log topic on it ("Drop a log topic here"); or drop the
  log topic on an empty dock directly. Drop more log topics to merge them.
- **Filter:**
  - Level toggles "Debug" (`levelDebug`), "Info" (`levelInfo`), "Warn" (`levelWarn`), "Error" (`levelError`), "Fatal"
    (`levelFatal`), all on by default.
  - Text fields "Filter sources..." (`sourceEdit`), "Filter categories..." (`categoryEdit`), "Filter messages..."
    (`filterEdit`). The source and category fields and columns appear only when some entry has one.
  - "Topics" (`topicsButton`, "Choose which topics to show"), shown when the panel holds more than one topic.
  - "Hide the entries after the playhead" / "Show the entries after the playhead, greyed" (`futureButton`).
- **Columns:** "Time", "Level", "Source", "Category", "Topic" (several topics only), "Message".
- **Read an entry:** click a row; the pane below (`detailText`) shows its time, stamp, level, source, topic, category
  and the full message. "Copy the selected entry to the clipboard" (`copyButton`).
- **Jump to an entry:** double-click its row to move the playhead there (ignored while playing).
- **Follow:** while playing, the list scrolls with the playhead and ignores the wheel and the scroll keys; paused, it
  scrolls to the entry at the playhead. There is no separate follow button.
- **MCP:** UI only for the panel. `list_topics` shows log topics; `screenshot` shows what the user sees.
- **Gotchas:** the Log panel has no settings page ("Open Settings" shows an empty right panel). Times follow the clock
  toggle (`buttonUseDateTime`).

## State Transitions

A strip per discrete series (text, integer, boolean, or a float holding whole numbers), drawn as coloured segments,
one per value held.

- **Open:** "State Transitions" (`buttonVizStateTransitions`) in an empty dock, or drop only discrete series on an empty
  dock. Drop more discrete series on it to add rows.
- **Navigate:** wheel zooms the time axis about the cursor; Shift + wheel scrolls the rows; left-drag pans. With "Link X
  axis" (`buttonLink`) on it follows the plots' X zoom and drives it.
- **Read a segment:** hover it: the value, its start and its duration in seconds.
- **Rows:** the right panel ("Open Settings") lists them with "Toggle curve visibility" and "Remove this curve from its
  plot"; "Curves options" → "Clear all curves" removes all.
- **MCP:** UI only. `read_series` reads the same series as values.
- **Gotchas:** the playhead line is display-only: click a plot or the time slider to seek.

## Custom Widget

A dock that hosts a Qt widget from a plugin (gauges, dashboards...) fed by series the user wires to its inputs.

- **Add:** "Custom Widget" (`buttonVizCustomWidget`) in an empty dock → pick `<widget> (from <plugin>)` from the menu.
  The button is disabled when no Custom Widget plugin is installed ("No custom widgets installed. Get them from the
  Marketplace."; install one: [plugins.md](./plugins.md)). An entry is disabled when the plugin was built for an
  incompatible Qt; its tooltip says why.
- **Wire inputs:** the right panel opens on the widget's page:
  1. "Inputs" (`inputsBand`) lists each input with its unit and kind ("number", "text" or "object").
  2. "Choose series" (`buttonChooseSeries`, single input) or "Add series" (`buttonAddSeries`, an input taking
     several) opens a chooser listing only the series that fit; double-click one to wire it. "Cancel"
     (`buttonCancelChooser`) goes back.
  3. Each wired series has "Remove this series" (`buttonRemoveWire`). "pending" means its dataset is not loaded yet.
  4. "Settings" (`settingsBand`): the widget's own controls, or "This widget has no settings.".
- **Drop instead:** drag series from the Datasets panel onto the widget; each fills the first free input of its kind.
  When none fits, "Series not accepted" lists what the inputs take.
- **MCP:** UI only.
- **Gotchas:**
  - A topic only a Custom Widget can show, dropped on an empty dock, is refused with "This topic is shown by a Custom
    Widget. Create one in this panel first."
  - If the plugin is missing (a layout from another machine), the dock shows a placeholder and keeps its wiring for when
    the plugin is installed.
  - After a plugin error the widget is disabled with "This widget was disabled after an error: ...".
