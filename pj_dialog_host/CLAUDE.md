# pj_dialog_host

## Purpose

Qt host for **plugin-provided dialogs**. Plugins (DataSource, MessageParser, Toolbox, Dialog) describe their UIs declaratively via the dialog protocol in `plotjuggler_sdk/pj_plugins`; this module turns those descriptions into actual `QWidget` trees and drives the typed event loop back to the plugin.

Single static-library target: `pj_dialog_engine_qt`. Public links: `plotjuggler_sdk::plugin_host`, `pj_widgets`, `Qt6::Widgets` / `UiTools` / `SvgWidgets`, and `plotjuggler_qwt` (backs `ChartPreviewWidget`; there is no `Qt6::Charts` dependency). Private links: `pj_plotting` (the full `PlotWidget` for embedded chart frames) and `pj_runtime` (`AppSession` + `CatalogModel`).

The protocol itself is documented in the SDK's `pj_plugins/docs/dialog-plugin-guide.md`; this module only realizes it in Qt. No `docs/` folder by design.

## What belongs here

- The dialog engine that interprets the SDK's `WidgetData` graph and instantiates Qt widgets.
- Widget bindings (the per-widget glue that maps SDK events ↔ `QObject` signals).
- Specialty embedded widgets that only make sense inside plugin dialogs (e.g. `chart_preview_widget`, `drop_event_filter`).
- Syntax-highlighter implementations for embedded code editors (Lua, Python; `src/*_syntax_highlighter.hpp`).

## What does NOT belong here

- **General app dialogs** (Preferences, Diagnostics, etc.) → `pj_app`.
- **Reusable dialog chrome and controls** (`Dialog`, `FileDialog`, `MessageBox`, scrubbers, …) → `pj_widgets`.
- **The dialog protocol / SDK** itself → `plotjuggler_sdk/pj_plugins` (this module is the host, not the contract).
- **Scene libraries**: a panel's `scene_view` frame gets its 3D/2D view through `EmbeddedViewHost` callbacks the app supplies; never link `pj_scene2D` / `pj_scene3D` here.

## Public surface

Public headers live under `include/pj_plugins/host_qt/` (the namespace mirrors `pj_plugins`: this is the Qt-side companion of that SDK family). Each header's doc-comment owns the contract; the table says what exists and the one thing to know.

| Header | Role |
|---|---|
| `dialog_engine.hpp` | Modal plugin dialog. `openDialog(parent, completion)` is the core: application-modal via `show()`, no nested event loop. `showDialog()` is a desktop-only blocking facade (rejected on WebAssembly); never add a new blocking caller. `cancelActiveDialog()` tears a still-open dialog down synchronously. `pj_app`'s `DialogPresenter` wraps both paths. |
| `panel_engine.hpp` | Long-lived interactive panel (toolboxes): same loader, binding and tick-and-diff as `DialogEngine`, returns a bare `QWidget*`, closed by the plugin's `requestClose("<reason>")` (the `onCloseRequested` callback owns the teardown decision). A hidden panel root ticks at 1/10 rate with a catch-up tick on Show. A `requestSubDialog` is modal on desktop and non-blocking in the browser. `PanelEngineConfig` is extended by appending members only: callers use positional initializers. |
| `widget_binding.hpp` | Per-widget event/data binding plumbing. `applyWidgetData` returns the `scene_view` requests it refused, for the engine to deliver after the apply. |
| `widget_adapters.hpp` | Swaps plain `.ui` controls for the styled `pj_widgets` equivalents (radio group → `DualOptionsWidget`, `QCheckBox` → `ToggleSwitch`, combo styling, `pjMarkdown` promotion, scrollbars). The originals stay hidden and remain the source of truth for WidgetData/events. Structural: engines run it once after load. |
| `embedded_view_host.hpp` | `EmbeddedViewHost`: the create/attach/detach callbacks through which the app supplies the view a `scene_view` frame embeds. A `PanelEngineConfig` carrying one announces `PJ_DIALOG_HOST_EMBEDS_SCENE_VIEWS` (`src/host_info.hpp`, announced once from each engine's constructor; `DialogEngine` announces no capability) and delivers each refusal as a `scene_view_error` event. |
| `pj_ui_loader.hpp` | `PjUiLoader`: the `QUiLoader` that instantiates host-provided custom widgets named in plugin `.ui` files (`RangeSlider`, `MarkerTimeline`, `DateRangePicker`, `CredentialsEditor`, `SectionHeaderBand`, `Search`, `PJ::ComboBox`). Shared by both engines; a new plugin-usable `pj_widgets` control is registered here. |
| `chart_preview_widget.hpp` | Light Qwt chart for toolbox previews (e.g. FFT). `widget_binding` builds it only when the frame carries `chart_markers` or the panel has no session/catalog; otherwise a `chart_series` frame renders with the full `PlotWidget`. Interaction is off until the `chart_zoom_enabled` key. |
| `drop_event_filter.hpp` | Event filter that turns Qt drops into SDK drag-drop events. |

## `.ui` dynamic-property contracts

Beyond the typed `WidgetData` protocol, the host honours a few **opt-in dynamic
properties** a plugin sets directly in its `.ui` file. They are structural (read
when the tree is loaded or adapted, not per tick) and carry no widget names in
the host, so they stay domain-neutral: any plugin whose layout fits the shape can
adopt one. This table is the reference plugin authors use.

| Property | Set on | Effect |
|---|---|---|
| `pjColumnSelectorList` (string) | `QTableWidget` | Names a sibling `QListWidget` that drives this table's column selection. The table becomes a read-only mirror: selecting list rows highlights the matching table **columns** (paired by header text) and direct user selection on the table is blocked, so the list stays the only driver. |
| `pjInteriorGrid` (bool) | `QTableView` / `QTableWidget` | Draws an interior-only cell grid (no outer border ruling). |
| `pjInlineGroup` (bool) | the radio buttons of a group | Keeps the group in its authored layout slot instead of letting the host re-place it when adapting radios into a `DualOptionsWidget`. |
| `pjButtonsFillWidth` (bool) | `QDialogButtonBox` | Stretches the box's buttons to share the full row width. |
| `pj_enable_when` (string) | any widget | `"<comboObjectName>:<index>[,<index>...]"`, or several such clauses joined by `;` (all must hold). The host keeps the widget enabled only while every clause's named sibling `QComboBox` sits on one of that clause's indices, and re-asserts the rule after every data apply (so it wins over a plugin-pushed `enabled`). Lets a plugin gate fields declaratively inside a modal sub-dialog, whose nested event loop it cannot drive itself. A clause that fails to parse (unknown combo, no numeric index) invalidates the whole rule; the widget is then left as authored. |
| `pj_visible_when` (string) | any widget | Same syntax and AND-across-`;`-clauses semantics as `pj_enable_when`, but governs visibility (redirected through the checkbox/radio adapter's visibility hook when the widget was swapped for a styled replacement). Hides label and editor separately: tag both widgets of a `QFormLayout`/`QGridLayout` row for the row to collapse. Also re-asserted after every data apply and inside modal sub-dialogs. |
| `pjFollowTail` (bool) | `QPlainTextEdit` | A transcript/log rather than a document: its first population and any wholesale replacement open on the newest lines; genuine growth keeps a reader who scrolled up in place, and a "jump to the latest" button (`pj_widgets`' `TailFollowOverlay.h`) shows while they are away from the end. Untagged plain text keeps Qt's default (opens at the top; sticky-bottom only on updates). |
| `pjMarkdown` (bool) | child `QPlainTextEdit` in a layout | After `.ui` loading, promotes the edit to a read-only `QTextBrowser` with the same object name and dynamic properties. `plain_text` WidgetData renders as GitHub-dialect Markdown, raw HTML disabled. Resource loading is blocked; only an explicit click on an `http`/`https` link opens externally. Combine with `pjFollowTail` for a streaming transcript. Cannot promote a `QPlainTextEdit` that is the `.ui` root itself. |
| `pjToolboxChromeAction` (bool) + `chromeActionIcon` (string) + `chromeActionSlot` (string) | `QAbstractButton` in a toolbox panel | `pj_app` hides the button in the content and stands a proxy in for it in the panel's chrome (the takeover/pinned banner, or the floating window's title bar), forwarding clicks to the original so its routing is untouched. `chromeActionIcon` is the SVG resource path the proxy shows; `chromeActionSlot="leading"` puts it before the title (which then centres itself), anything else after it. |
| `pjToolboxSideDrawer` (bool) | one `QWidget` in a toolbox panel | `pj_app` hoists the widget into a full-height, draggable column at the LEFT of the whole panel chrome (beside the banner / floating title bar, not under it) in a `QSplitter`, resizable down to the drawer's own `.ui` minimum width and persisted per plugin in `QSettings` as `ToolboxDrawerWidth/<plugin id>`. The plugin keeps driving it by name; see `pjHoistedWidgets` below. |
| `pj_context_actions` (string) | `QListWidget` | `"id=Label[;id2=Label 2...]"`: a right-click over a row (not empty space) builds a context menu from the clauses, in order. Choosing an entry emits `WidgetEventBuilder::itemContextAction(index, id)` with the delivered-order row index (the same `kPluginRowRole` translation `onItemDoubleClicked`/`onItemDeleteRequested` use). A malformed clause (no `=`, empty id or label) is skipped. Only the fired id crosses the ABI (`onItemContextAction` in the SDK); the menu is host-rendered. Absent or empty: no context menu. |

Implementation notes for these contracts:

- The `pj_enable_when`/`pj_visible_when` closures are cached per loaded root in an `InstalledRules` QObject child (`widget_binding.cpp`, `Q_OBJECT`, found again by `findChildren<InstalledRules*>` from any ancestor root on every data apply).
- `pjHoistedWidgets` (`QVariantList` of `QObject*`, `kHoistedWidgetsProperty` in `widget_binding.hpp`) is host-internal: a presentation that moves a plugin widget elsewhere in the window records it on the panel root, so the binding's `findNamedIn`/`findAllIn` lookups still reach it. Any new hoisting presentation must record its widgets there.
- **Adding a contract:** hoist the name to a `constexpr const char* k…Property` next to its reader and add a row above. A bare string literal in one `.cpp` is invisible to the plugin authors who are supposed to use it.

## Tests

Two gtest runners on the shared gui test main (`pj_add_test_runner` in `cmake/PjTests.cmake`); test files carry no `main()`:

- `pj_dialog_host_tests`: `tests/dialog_engine_test.cpp` (with the SDK's `mock_dialog.cpp`), `tests/widget_binding_test.cpp`, `tests/combo_rules_test.cpp` (the `pj_enable_when`/`pj_visible_when` rule pair) and `tests/chart_preview_widget_test.cpp`.
- `pj_dialog_host_panel_tests`: `tests/panel_engine_test.cpp` with `tests/mock_panel_plugin.cpp`. A separate runner because both mocks define the plugin entry point `PJ_get_dialog_vtable`; never merge them.

Run one runner with `./test.sh -R '^pj_dialog_host_tests\.'`.
