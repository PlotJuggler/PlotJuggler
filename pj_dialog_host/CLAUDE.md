# pj_dialog_host

Qt host for plugin-provided dialogs. Plugins (DataSource, MessageParser, Toolbox, Dialog) describe their UIs
declaratively through the dialog protocol in `plotjuggler_sdk/pj_plugins` (its `pj_plugins/docs/dialog-plugin-guide.md`
documents the protocol); this module turns those descriptions into `QWidget` trees and drives the typed event loop
back to the plugin. One static library, `pj_dialog_engine_qt`. Public links: `plotjuggler_sdk::plugin_host`,
`pj_widgets`, `Qt6::Widgets` / `UiTools` / `SvgWidgets`, `plotjuggler_qwt` (`ChartPreviewWidget`; no `Qt6::Charts`).
Private links: `pj_plotting` (the full `PlotWidget` for embedded chart frames) and `pj_runtime` (`AppSession`,
`CatalogModel`).

## Belongs here, and not

- Here: the dialog and panel engines, the per-widget bindings (SDK events to `QObject` signals), embedded widgets
  that only make sense in plugin dialogs (`chart_preview_widget`, `drop_event_filter`) and the Lua / Python syntax
  highlighters for code editors (`src/*_syntax_highlighter.hpp`).
- App dialogs (Preferences, Diagnostics) go to `pj_app`; reusable dialog chrome and controls (`Dialog`, `FileDialog`,
  `MessageBox`, scrubbers) to `pj_widgets`; the protocol itself to `plotjuggler_sdk/pj_plugins`.
- Never link `pj_scene2D` / `pj_scene3D`: a panel's `scene_view` frame gets its view through the `EmbeddedViewHost`
  callbacks the app supplies, which keeps this module free of scene libraries.

## Invariants

- `DialogEngine` runs no nested event loop: `openDialog()` shows the dialog application-modal with `show()`. `open()`
  would downgrade it to window-modal, which leaves floating dock tool-windows interactive. `showDialog()` is a
  desktop-only blocking wrapper (refused on WebAssembly): add no caller.
- Widget callbacks captured by reference are wrapped in `guardedCallback` and severed when the dialog completes: the
  widgets outlive the engine's coroutine frame, and a late signal would call into a destroyed one.
- `cancelActiveDialog()` must be called while the plugin context borrowed by the `DialogHandle` is alive; it delivers
  exactly one `on_rejected`, and afterwards nothing reaches the plugin.
- Both engines skip a byte-identical widget-data re-read. One-shot requests (accept, sub-dialog, close) change the
  bytes, and `PanelEngine` clears its memo after consuming one: a plugin answering "" while idle would otherwise
  leave it on the old request, and an identical request later would be dropped as a repeat.
- `PanelEngine` neither diffs nor applies a payload that requests its close: the panel is being torn down, and the
  full-payload parse, diff and apply would hold the main thread in the tick that closes it. A declined close applies
  that state on the next tick, which diffs against the last applied payload.
- `PanelEngineConfig` grows by appending members only: callers use positional initializers.
- Plugin widget data is applied by name to the tree the engine loaded (plus widgets recorded in `pjHoistedWidgets`).
  A sub-dialog or an injected parser dialog is a separate tree: its field states come from the declarative rules.
- A table row is identified by its `kPluginRowRole` tag, not its view position or text: with `sortingEnabled` the user
  re-orders the view, and every index the plugin sends or receives is translated through the tag.
- A table row the plugin both disables (`disabled_rows`) and selects is locked: its items stay disabled, so clicks,
  keyboard and selection never reach it and `selectionChanged` carries only rows the user can change (locked rows are
  the plugin's own), and they carry `kLockedRowRole` (`widget_binding.hpp`). A deletable table's trash icon on a
  locked row still reaches the plugin. `LockedRowDelegate` (`src/locked_row_delegate.hpp`), the base of every host
  table delegate, paints a tagged item selected and enabled, so the QSS `::item:selected` accent applies; a new table
  delegate derives from it or locked rows paint grey; an itemless cell paints with its row's first tagged item. A
  disabled row the plugin does not select keeps the disabled look and any selection stored for it (Qt hides it until
  the row is enabled). A row without any item is never locked. The lock pass issues no `select()` unless a lock is
  released: one without `Current` would finalize the user's in-progress drag or Shift range (deliveries run inside
  `selectionChanged`). The MCP `ui_tree` reports a tagged disabled row as selected.
- A lock comes only from the plugin's `disabled_rows` and selection. A delivery missing one of them keeps that aspect's
  current state (a lock released by `disabled_rows` alone leaves its row selected, and only a delivery carrying
  `disabled_rows` touches enabled flags); an explicit `disabled_rows`, even empty, enables every row it does not name;
  a rows rewrite that changes a row's key text (`tableRowKeyText`) clears its lock, other cells keep it.
- A table column never mixes typed (`TypedTableItem`) and plain cells: the comparison is then not a strict weak
  ordering and `std::stable_sort` is undefined. Code that writes cells keeps each column homogeneous.
- The `.ui` contracts name no plugin widget: behaviour comes from properties and protocol keys.

## Public headers (`include/pj_plugins/host_qt/`)

The namespace mirrors `pj_plugins`: this is its Qt-side companion. Each header's doc-comment owns the contract.

- `dialog_engine.hpp`: `DialogEngine`, a modal plugin dialog (`openDialog`, `cancelActiveDialog`). `pj_app`'s
  `DialogPresenter` wraps it.
- `panel_engine.hpp`: `PanelEngine`, a long-lived interactive panel (toolboxes) returned as a bare `QWidget*`, closed
  through the plugin's `requestClose("<reason>")` and the `onCloseRequested` callback. A hidden root ticks at 1/10
  rate with a catch-up tick on Show. A `requestSubDialog` is modal on desktop and non-blocking in the browser.
- `widget_binding.hpp`: `applyWidgetData`, `connectWidgetSignals`, the declarative `pj_enable_when` /
  `pj_visible_when` rules. `applyWidgetData` returns the `scene_view` requests it refused, for the engine to deliver
  after the apply.
- `widget_adapters.hpp`: swaps plain `.ui` controls for the styled `pj_widgets` ones (radio group to
  `DualOptionsWidget`, `QCheckBox` to `ToggleSwitch`, combo styling, `pjMarkdown` promotion, scrollbars). The
  originals stay hidden and remain the source of truth for WidgetData and events. Engines run it once after load.
- `embedded_view_host.hpp`: `EmbeddedViewHost`, the create / attach / detach callbacks that supply the view a
  `scene_view` frame embeds. A `PanelEngineConfig` carrying one announces `PJ_DIALOG_HOST_EMBEDS_SCENE_VIEWS`
  (`src/host_info.hpp`, from the engine's constructor; `DialogEngine` announces no capability) and delivers each
  refusal as a `scene_view_error` event.
- `pj_ui_loader.hpp`: `PjUiLoader`, the `QUiLoader` that instantiates the host widgets plugin `.ui` files name
  (`RangeSlider`, `MarkerTimeline`, `DateRangePicker`, `CredentialsEditor`, `SectionHeaderBand`, `Search`,
  `ToggleSwitch`, `PJ::ComboBox`). A new plugin-usable `pj_widgets` control is registered in its `createWidget`.
  Its `load` (hiding `QUiLoader::load`, so callers hold a `PjUiLoader`) swaps every `<iconset>` that names a host
  SVG (`:/resources/svg/...`) for the themed `loadSvg` rendering, once at load.
- `chart_preview_widget.hpp`: a light Qwt chart for toolbox previews. `widget_binding` builds it when the frame
  carries `chart_markers` or the panel has no session and catalog; otherwise `chart_series` renders with the full
  `PlotWidget`. Interaction is off until the `chart_zoom_enabled` key.
- `drop_event_filter.hpp`: turns Qt drops of catalog items into SDK drag-drop events.
- `browser_file_picker_fallback.hpp`: WebAssembly only, `ScopedOpenFilePickerFallback` masks Qt's File System Access
  probe for the length of a Qt file call. `src/browser_save_target.hpp` offers a toolbox's exports as browser
  downloads.

## `.ui` dynamic-property contracts

Plugins set these directly in their `.ui`. They are structural (read when the tree is loaded or adapted, not per
tick) and carry no widget names in the host: any plugin whose layout fits the shape can use one. This table is the
reference for plugin authors.

| Property | Set on | Effect |
|---|---|---|
| `pjColumnSelectorList` (string) | `QTableWidget` | Names a sibling `QListWidget` that drives the table's column selection. The table becomes a read-only mirror: selecting list rows highlights the table columns with matching header text, and direct selection on the table is blocked. |
| `pjInteriorGrid` (bool) | `QTableView` / `QTableWidget` | An interior-only cell grid, with no outer border ruling. |
| `pjInlineGroup` (bool) | the radio buttons of a group | Keeps the group in its authored layout slot instead of the host re-placing it when it adapts the radios into a `DualOptionsWidget`. |
| `pjButtonsFillWidth` (bool) | `QDialogButtonBox` | The buttons share the full row width. |
| `pj_enable_when` (string) | any widget | `"<comboObjectName>:<index>[,<index>...]"`, or clauses joined by `;` (all must hold). The widget is enabled only while every clause's sibling `QComboBox` sits on one of its indices. Re-asserted after every data apply, so it wins over a plugin-pushed `enabled`. Needed in a modal sub-dialog, whose nested event loop the plugin cannot drive. A clause that fails to parse (unknown combo, no numeric index) invalidates the whole rule and the widget stays as authored. |
| `pj_visible_when` (string) | any widget | Same syntax and semantics, for visibility (redirected through the checkbox or radio adapter when the widget was swapped for a styled replacement). Label and editor hide separately: tag both widgets of a `QFormLayout` / `QGridLayout` row for the row to collapse. |
| `pjFollowTail` (bool) | `QPlainTextEdit` | A transcript or log: its first population and any wholesale replacement open on the newest lines; growth keeps a reader who scrolled up in place, and a "jump to the latest" button (`pj_widgets/TailFollowOverlay.h`) shows while they are away from the end. Untagged text opens at the top. |
| `pjMarkdown` (bool) | child `QPlainTextEdit` in a layout | After load, promoted to a read-only `QTextBrowser` with the same object name and properties. `plain_text` renders as GitHub-dialect Markdown with raw HTML disabled, resource loading blocked, and only an explicit click on an `http` / `https` link opening externally. Combine with `pjFollowTail` for a streaming transcript. Cannot promote a `QPlainTextEdit` that is the `.ui` root. |
| `pjToolboxChromeAction` (bool), `chromeActionIcon` (SVG resource path), `chromeActionSlot` (string) | `QAbstractButton` in a toolbox panel | `pj_app` hides the button in the content and stands a proxy in the panel chrome (the banner, or the floating window's title bar), forwarding clicks to the original. `chromeActionSlot="leading"` puts it before the title; anything else after. |
| `pjToolboxSideDrawer` (bool) | one `QWidget` in a toolbox panel | `pj_app` hoists the widget into a full-height, draggable `QSplitter` column at the left of the whole panel chrome, resizable down to its own `.ui` minimum width and persisted per plugin in `QSettings` as `ToolboxDrawerWidth/<plugin id>`. The plugin keeps driving it by name (see `pjHoistedWidgets`). |
| `pj_context_actions` (string) | `QListWidget` | `"id=Label[;id2=Label 2...]"`: a right-click over a row (not empty space) builds a context menu from the clauses, in order. A choice emits `WidgetEventBuilder::itemContextAction(index, id)` with the delivered-order row index; only the id crosses the ABI. The menu goes through `PJ::showOwnedMenu`, so it is non-blocking in the browser and a row replaced while it is open fires nothing. A malformed clause (no `=`, empty id or label) is skipped. Absent or empty: no menu. |

- The `pj_enable_when` / `pj_visible_when` closures are cached per loaded root in an `InstalledRules` child
  (`widget_binding.cpp`), found again from any ancestor root on every data apply.
- `pjHoistedWidgets` (`kHoistedWidgetsProperty`, `widget_binding.hpp`) is host-internal: a presentation that moves a
  plugin widget elsewhere in the window records it on the panel root, so the by-name lookups still reach it. Any new
  hoisting presentation must record its widgets there, or they stop receiving data.
- Adding a contract: hoist the name to a `constexpr const char* k...Property` next to its reader and add a row above;
  a bare literal in one `.cpp` is invisible to the plugin authors who should use it (`pjInlineGroup` and
  `pjButtonsFillWidth` are still bare literals).

## Tests

Two gtest runners on the shared gui test main (`pj_add_test_runner`, `cmake/PjTests.cmake`; test files carry no
`main()`):

- `pj_dialog_host_tests`: `dialog_engine_test.cpp` (with the SDK's `mock_dialog.cpp`), `widget_binding_test.cpp`,
  `combo_rules_test.cpp`, `chart_preview_widget_test.cpp`, `browser_save_target_test.cpp`.
- `pj_dialog_host_panel_tests`: `panel_engine_test.cpp` with `mock_panel_plugin.cpp`. A separate runner because both
  mocks define the plugin entry point `PJ_get_dialog_vtable`: never merge them.

Run a runner with `./test.sh -R '^pj_dialog_host_tests\.'`.

## Known gaps

- `PanelEngine::openPanel` stores the initial payload as `prev_raw`, so the first poll skips as identical and a
  one-shot request in the initial widget data waits for the payload to change; `DialogEngine` keeps its memo empty
  for that reason.
- `PanelEngine` forwards events for the main panel and its drop filter through two inline copies of what
  `Impl::forwardEvent` does for the sub-panel.
- `PreviewScaleDraw` in `chart_preview_widget.cpp` duplicates `pj_plotting`'s `PlotScaleDraw`, which this target can
  link privately.
