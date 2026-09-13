# pj_dialog_host

## Purpose

Qt host for **plugin-provided dialogs**. Plugins (DataSource, MessageParser, Toolbox, Dialog) describe their UIs declaratively via the dialog protocol in `plotjuggler_sdk/pj_plugins`; this module turns those descriptions into actual `QWidget` trees and drives the typed event loop back to the plugin.

Single static-library target: `pj_dialog_engine_qt`. Links `Qt6::Widgets`, `Qt6::UiTools`, `Qt6::SvgWidgets`, and the vendored `plotjuggler_qwt` (which backs `ChartPreviewWidget` — there is no `Qt6::Charts` dependency).

## What belongs here

- The dialog engine that interprets the SDK's `WidgetData` graph and instantiates Qt widgets.
- Widget bindings (the per-widget glue that maps SDK events ↔ `QObject` signals).
- Specialty embedded widgets that only make sense inside plugin dialogs (e.g. `chart_preview_widget`, `drop_event_filter`).
- Syntax-highlighter implementations for embedded code editors (Lua, Python).

## What does NOT belong here

- **General app dialogs** (Preferences, Diagnostics, etc.) → `pj_app`.
- **Reusable dialog chrome and controls** (`Dialog`, `FileDialog`, `MessageBox`, scrubbers, …) → `pj_widgets`.
- **The dialog protocol / SDK** itself → `plotjuggler_sdk/pj_plugins` (this module is the host, not the contract).

## Public surface

Public headers live under `include/pj_plugins/host_qt/` — the namespace mirrors `pj_plugins` because this is conceptually a Qt-side companion to that SDK family.

| Header | Role |
|---|---|
| `dialog_engine.hpp` | Top-level engine: takes a `WidgetData` tree, returns a constructed `QWidget`, drives events. The async core is `openDialog(QWidget* parent, Completion completion)` — builds and opens the dialog application-modal via `show()` without entering a nested event loop (completion runs on the GUI thread when the dialog finishes) — plus `cancelActiveDialog()` to synchronously tear down a still-open dialog; `pj_app`'s `FileLoader` coroutine awaits this path for every data-source config dialog. The blocking `showDialog()` facade (a `QEventLoop` wrapper over the async core) remains for callers that need a synchronous answer, e.g. `StreamingSourceManager`. |
| `panel_engine.hpp` | Hosts a long-lived interactive panel built from a plugin's typed-dialog UI. Sibling of `dialog_engine.hpp`: same .ui loader/binding/tick-and-diff, but returns a bare `QWidget*` via `openPanel()` (no modal `exec()`) and is closed by plugin-initiated `requestClose("<reason>")`. A hidden panel root (e.g. a toolbox pinned into a non-current central tab) ticks at 1/10 rate — plugin logic keeps advancing while the invisible UI mostly skips poll+diff — with a queued catch-up tick on Show. |
| `widget_binding.hpp` | Per-widget event/data binding plumbing. |
| `pj_ui_loader.hpp` | `QUiLoader` subclass (`PjUiLoader`) that teaches `QUiLoader` to instantiate host-provided custom widgets (RangeSlider, DateRangePicker, CredentialsEditor, MarkerTimeline) from plugin `.ui` files; shared by both the dialog and panel engines. |
| `chart_preview_widget.hpp` | Embedded chart widget used by toolboxes (e.g. FFT preview). Qwt-based (no Qt Charts dependency). |
| `drop_event_filter.hpp` | Event filter that turns Qt drops into SDK drag-drop events. |

## `.ui` dynamic-property contracts

Beyond the typed `WidgetData` protocol, the host honours a few **opt-in dynamic
properties** a plugin sets directly in its `.ui` file. They are structural (read
when the tree is loaded or adapted, not per tick) and carry no widget names in
the host, so they stay domain-neutral: any plugin whose layout fits the shape can
adopt one.

| Property | Set on | Effect |
|---|---|---|
| `pjColumnSelectorList` (string) | `QTableWidget` | Names a sibling `QListWidget` that drives this table's column selection. The table becomes a read-only mirror: selecting list rows highlights the matching table **columns** (paired by header text) and direct user selection on the table is blocked, so the list stays the only driver. |
| `pjInteriorGrid` (bool) | `QTableView` / `QTableWidget` | Draws an interior-only cell grid (no outer border ruling). |
| `pjInlineGroup` (bool) | the radio buttons of a group | Keeps the group in its authored layout slot instead of letting the host re-place it when adapting radios into a `DualOptionsWidget`. |
| `pjButtonsFillWidth` (bool) | `QDialogButtonBox` | Stretches the box's buttons to share the full row width. |
| `pj_enable_when` (string) | any widget | `"<comboObjectName>:<index>[,<index>...]"`, or several such clauses joined by `;` (all must hold) — the host keeps the widget enabled only while every clause's named sibling `QComboBox` sits on one of that clause's indices, and re-asserts the rule after every data apply (so it wins over a plugin-pushed `enabled`). Lets a plugin gate fields declaratively inside a modal sub-dialog, whose nested event loop it cannot drive itself. A clause that fails to parse (unknown combo, no numeric index) invalidates the whole rule; the widget is then left as authored. |
| `pj_visible_when` (string) | any widget | Same syntax and AND-across-`;`-clauses semantics as `pj_enable_when`, but governs visibility instead of `enabled` (redirected through the checkbox/radio adapter's visibility hook when the widget was swapped for a styled replacement). Hides label and editor separately — tag both widgets of a `QFormLayout`/`QGridLayout` row for the row to collapse. Also re-asserted after every data apply, and inside modal sub-dialogs, for the same reason `pj_enable_when` is. |
| `pjFollowTail` (bool) | `QPlainTextEdit` | The widget is a transcript/log rather than a document: its first population and any wholesale replacement (another conversation swapped in) open on the newest lines; genuine growth keeps a reader who scrolled up in place, and a "jump to the latest" overlay button (`pj_widgets`' `TailFollowOverlay.h`) sits at the viewport's bottom-right while they are away from the end. Untagged plain text keeps Qt's default (opens at the top; sticky-bottom only on updates). |
| `pjMarkdown` (bool) | child `QPlainTextEdit` in a layout | After `.ui` loading, promotes the edit to a read-only `QTextBrowser` with the same object name and dynamic properties. Existing `plain_text` WidgetData is rendered with Qt's GitHub Markdown dialect (bold, lists, tables and fenced/inline code), raw HTML disabled. Resource loading is blocked; only an explicit click on an `http`/`https` link opens externally. Combine with `pjFollowTail` for a streaming transcript. The structural adapter cannot promote a `QPlainTextEdit` used as the `.ui` root itself. Older hosts ignore the property and show the raw Markdown in the authored plain edit. |
| `pjToolboxChromeAction` (bool) + `chromeActionIcon` (string) + `chromeActionSlot` (string) | `QAbstractButton` in a toolbox panel | `pj_app` hides the button in the content and stands a proxy in for it in the panel's chrome (the takeover/pinned banner, or the floating window's title bar), forwarding clicks to the original so its existing routing is untouched. `chromeActionIcon` is the SVG resource path the proxy shows; `chromeActionSlot="leading"` puts it before the title (which then centres itself), anything else after it. |
| `pjToolboxSideDrawer` (bool) | one `QWidget` in a toolbox panel | `pj_app` hoists the widget out of the content into a full-height, draggable column at the LEFT of the whole panel chrome — beside the banner / floating title bar, not under it — in a `QSplitter` (handle in place of the old separator; resizable down to the drawer's own `.ui` minimum width, and persisted per plugin in `QSettings` as `ToolboxDrawerWidth/<plugin id>`). The plugin keeps driving it by name (`setVisible`, list items, …): the panel root records the hoisted widget in the `pjHoistedWidgets` property, and every by-name lookup in `widget_binding` searches those too. |
| `pj_context_actions` (string) | `QListWidget` | `"id=Label[;id2=Label 2...]"` — a right-click on a row builds a context menu from the clauses, in order, and popping only over a row (nothing on empty space). Choosing an entry emits `WidgetEventBuilder::itemContextAction(index, id)` with the delivered-order row index (the same `kPluginRowRole` translation `onItemDoubleClicked`/`onItemDeleteRequested` use). A malformed clause (no `=`, empty id or label) is skipped; the rest of the menu still builds. The action set is host-rendered UI, not part of the dialog protocol — only the fired id crosses the ABI (`onItemContextAction` in the SDK). An absent or empty property is today's behaviour: no context-menu policy set. |

`pjHoistedWidgets` (`QVariantList` of `QObject*`, `kHoistedWidgetsProperty` in `widget_binding.hpp`) is host-internal: it is how a presentation that moved a plugin widget elsewhere in the window keeps the binding's `findNamedIn`/`findAllIn` lookups reaching it.

When adding another such contract, hoist the name to a `constexpr const char* k…Property`
next to its reader and add a row here — a bare string literal in one `.cpp` is
invisible to the plugin authors who are supposed to use it.

## Tests

Four test executables: `tests/dialog_engine_test.cpp`, `tests/panel_engine_test.cpp` (with `tests/mock_panel_plugin.cpp`), `tests/widget_binding_test.cpp`, and `tests/combo_rules_test.cpp` (the `pj_enable_when`/`pj_visible_when` rule pair). Add tests when extending the protocol coverage.

`pj_dialog_host` has no `docs/` folder — the protocol itself is documented in `plotjuggler_sdk/pj_plugins/docs/dialog-plugin-guide.md`; this host just realizes that protocol in Qt.
