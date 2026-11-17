# pj_custom_widgets

The "Custom Widget" panel: a dock whose content is a Qt widget created by a Widget plugin (the SDK family `Widget`, ABI
in `pj_plugins/widget_protocol`). The module owns what the host does for such a panel: wiring series to the widget's
named inputs, resolving the value at the tracker time, pushing it to the plugin, persisting the panel in the layout,
and destroying the plugin's objects in a safe order. The plugin never sees topics, datasets or the data store. How it
works: [docs/ARCHITECTURE.md](./docs/ARCHITECTURE.md).

## Invariants

- Every plugin call is on the GUI thread and goes through `callPlugin` / `callOrDisable`. An `inputs_changed` arriving
  from inside a plugin call is deferred (`deferOnce`), or the input list changes under a running `on_update`; a
  `config_changed` from inside one is dropped, or an undo turns into a new step.
- Inputs are matched by name, never by position, and `WidgetBindings::setInputs` is the only reconciliation point. A
  failed `inputs()` is never reconciled against (`readInputs()` disables the panel): an empty list would drop every
  wire.
- `on_update` runs only when a wired sample changed, plus one deferred `requestUpdate` for playback start/stop and for
  entries landing on a wired object topic. Fetch object payloads and strings only after the identity hash changed, or
  every tick pays for a lazy fetch.
- Widgets receive canonical object bytes through the panel's `ObjectPayloadEncoder`, for pushes and `query_history`.
- `releasePlugin()` runs in both `aboutToBeDiscarded()` and the destructor, detaching the plugin's widget before
  destroying the handle. Drop either and the plugin's code can be reached by a deferred delete after the session is
  gone. Never `deleteLater` anything the plugin created.
- A widget that cannot be created, or was disabled after an error, keeps its layout element verbatim
  (`preserved_`): the layout round-trips without the plugin.
- The host stores the whole settings JSON and sends it whole through `load_config`, manifest defaults filled in: a new
  panel's first `load_config` is the full defaults object, never `{}`.
- The active-widgets record is added right before `createHandle` and removed after the handle died. A crash inside the
  plugin is not catchable in-process; this record is how the next start learns of it.
- Persistence is one `<CustomWidget>` tag; an `<input>` goes through `LayoutXml`'s three passes like a transform input.
  Wires that do not resolve stay pending and are written back unchanged.

## Layout

- `core/` (`pj_custom_widgets_core`, no widget class): `WireSample.{h,cpp}` (latest sample of a wire, identity hash,
  `PJ_widget_value_t`), `SettingsSchema.{h,cpp}` (`parseSettingsSchema`), `WidgetBindings.{h,cpp}` (wires,
  reconciliation, resolution, `<input>` XML).
- `widget/` (`pj_custom_widgets`): `CustomWidgetDockWidget` (`QWidget` + `IDataWidget`), `MissingWidgetPlaceholder`.
- Shell side lives in `pj_app`: `CustomWidgetController` (picker and side-panel pages, the active-widgets record),
  `PendingDisplayBinder`, `TopicDemandController::registerCustomWidgetDock`, `LayoutXml`.

## Tests

`./test.sh -R '^pj_custom_widgets_tests\.'`: unit and dock tests against an in-process plugin
(`tests/in_process_widget_plugin.h`). The end-to-end runs (`tests/E2E.cmake`, registered from `pj_app/CMakeLists.txt`)
are Linux only.

## Docs

[docs/ARCHITECTURE.md](./docs/ARCHITECTURE.md): panel and widget creation, inputs and wiring, value delivery,
settings, persistence, demand, lifetime, errors and crashes, known gaps.
