# pj_widgets

## Purpose

Reusable Qt widgets and UI helpers: controls that could be dropped into another Qt application without dragging PJ4-specific state with them. Static library, `Qt6::Core` / `Gui` / `Network` / `Svg` / `Widgets` only (public), plus a private `Qt6::GuiPrivate` link for `<rhi/qrhi.h>`.

Each header's doc comment owns that widget's contract (threading, ownership, call order, pitfalls); the tables below say what exists and flag the one thing to know. This file is the catalogue other modules search before writing a widget: add a line here with every new reusable control or helper. No `docs/` folder: the headers carry the contracts.

## Hard constraints

- Depends only on **Qt and the C++ standard library** (plus the dependency-free, header-only `raster/ipc/` wire-protocol contract, which is not a PJ module).
- **No dependencies on `pj_runtime`, `pj_app`, or any other PJ module.** Anything that needs runtime state belongs upstream of here; widgets take plain types and emit intent signals, and the host converts.
- A widget specific to PlotJuggler workflows (e.g. plot-specific dockers) belongs in its widget-family module instead.

## Rules

- **Always use `PJ::ComboBox`, never a plain `QComboBox`** (promote `QComboBox` to `PJ::ComboBox` in `.ui` files, header `pj_widgets/ComboBox.h`); a plain combo loaded from a plugin `.ui` is upgraded in place with `applyComboBoxStyling()`.
- **Every filter field is `PJ::Search`**: promote `QLineEdit` search fields to `PJ::Search` in `.ui` files; do not hand-roll a glyph + line edit.
- **Every PJ4 table header** gets `HeaderResizePolicy::install` (`HeaderDividerHighlight::install` adds the divider highlight); never mark a section `Stretch` or `ResizeToContents`, which makes its dividers dead.
- **Theme colours come from `FrameworkTokens.h`**: the QSS palette is the single ground truth, and self-painted widgets use the header because they cannot reach QSS `${token}`. A surface's fill is chosen by its role, never by a raw literal.
- **Every `QRhiWidget` requests `preferredGraphicsApi()`**: one API per top-level window.
- **Append-only enumerations:** `Colormap` (LUT row and persisted id) and the `StateColors` palette (index = hash % size). Reordering silently recolours or corrupts saved data.
- **Chrome heights are never re-derived:** use `ChromeMetrics::bandHeight()` / `titleBarHeight()`; never `setFixedHeight` a `SectionHeaderBand`, or its height diverges from every other band.
- **Q_OBJECT is mandatory on a widget a QSS type selector targets** (`PJ--ProgressBar`, `PJ--Search`, `PJ--ToastNotification`): the selector matches `metaObject()->className()`, and without its own meta-object it silently stops matching.
- **Self-painted widgets read the app palette** (`theme::appTheme()`), never their own: QStyleSheetStyle rewrites a widget's palette under QSS and can report the wrong theme.

## UI convention

Per root CLAUDE.md: **prefer `.ui` files**. `AUTOUIC` is on, search path `src/`. The only `.ui` is `Dialog.ui`; keep new chromed dialogs on the same pattern.

## Inventory

Dialogs, chrome and feedback:

| Header | What it is, and the one thing to know |
|---|---|
| `Dialog.h` + `Dialog.ui` | Frameless app-styled dialog chrome: title-bar actions, optional side drawer. Base of `FileDialog`, `ProgressDialog`. |
| `SideDrawer.h` | `makeSideDrawerSplitter`: draggable side column. The same `settings_key` shares the remembered width across presentations (`Dialog::setSideWidget`, `pj_app` toolbox panels). |
| `FileDialog.h` | `QFileDialog` in `Dialog` chrome; use the static helpers. A start folder that does not answer within 2 s (dead network share) falls back to home with a notice; Qt's shared `QtProject/FileDialog` memory is filtered the same way before construction. A save over an existing file asks with `MessageBox` (Replace, Cancel), not Qt's native `QMessageBox`: `DontConfirmOverwrite` is set and the inner dialog's `accept()` asks, through `questionAsync`. `setUseSystemDialog()` hands the helpers (except `getSaveFileNameWithOptions`) to the platform picker, on Windows and macOS only. |
| `MessageBox.h` | Themed `QMessageBox` replacement. `question()` is desktop-only in practice: see Usage notes. |
| `ProgressDialog.h` | Modal progress with domain-neutral Primary/Secondary stop actions; no close affordance. |
| `ProgressBar.h` | App-styled `QProgressBar` (`PJ--ProgressBar` QSS class). `Q_OBJECT` is mandatory or the selector never matches. |
| `IngestProgressWidget.h` | Title-bar progress strip; shows or hides only on `setActive()` (timing is owner policy). |
| `ToastManager.h`, `ToastNotification.h` | Bottom-right toasts over a host widget; forward the host's `resizeEvent` to `updatePosition()`. Message is rich text with external links. |
| `OwnedMenu.h` | `showOwnedMenu`: the one way to show a context menu. Self-deleting, modal on desktop and non-blocking in the browser; act through signal handlers, never read results after the call. |
| `RecentFilesMenu.h` | Persistent MRU menu; `enabledChanged` is not emitted by the constructor, seed it via `isEmpty()`. |
| `RenderingWarningLabel.h` | Hidden themed warning label the 2D/3D docks show while their GPU view cannot render. |
| `AppSettings.h` | `appSettings()` / `appSetting()`: the per-thread default `QSettings` (a constructed one reads from memory, which matters in the browser, where constructing one costs 2-3 ms), and `valuePrecision()`, the app-wide value-text precision. Fix the format and path before the first call (the browser switches the default format at startup). |

Inputs and small controls:

| Header | What it is, and the one thing to know |
|---|---|
| `ComboBox.h`, `ComboBoxGradientDelegate.h` | App-styled combo and its gradient popup delegate (QSS cannot paint a gradient on combo popup items). See Rules. |
| `Search.h` | The canonical search/filter input; two background tones via `setVariant()`. See Rules. |
| `SectionHeaderBand.h` | Canonical section header strip, with optional inline filter, trailing combo/buttons/toggle driven by `.ui` properties (usable from plugin panels). A parent-only constructor lets uic build it from an app `.ui` file, with the title in the `text` property. |
| `SvgButton.h` | `QToolButton` whose SVG icon re-tints on theme switch with no host wiring; `setIconPath()` swaps state glyphs. Icons use `loadSvg`'s 64-px cache; large ones need `renderSvgPixmap`. |
| `ToggleSwitch.h` | Sliding switch replacing `QCheckBox`; optional inline label. |
| `CheckButton.h` | Checkable pill toggle with an inline label; colours are QSS `qproperty-*`. |
| `ColorPickerPopup.h`, `ColorPickerWidget.h` | Colour popup and the one swatch control (never hand-roll per-layer swatches). `colorChanged()` fires on user picks only; `setColor()` is silent. |
| `IntScrubber.h`, `DoubleScrubber.h`, `ScrubberBase.h` | Drag-to-scrub numeric inputs; spin-box API, promote in `.ui`. `DoubleScrubber` never holds NaN or infinity: it ignores them and refuses typed `nan`/`inf`. |
| `RangeSlider.h`, `RealSlider.h` | Two-handle range slider (plugin `.ui` by class name); floating-point `QSlider` with click-to-seek and handle dragging. |
| `DualOptionsWidget.h` | Segmented control replacing an exclusive `QRadioButton` group; colours via `qproperty-*` QSS. |
| `MarkerTimeline.h` | Editable Region/Event mark strip; a dumb editor, the owner supplies marks and placement policy. |
| `DateRangePicker.h` | Date/time range picker bundle for plugin `.ui` files. |
| `CredentialsEditor.h` | Domain-neutral credentials editor for plugin `.ui` files. |
| `ConfigPanelHost.h` | Single-slot panel host; owns and `deleteLater()`s the previous panel. |
| `LayerListView.h` | Reorderable scene-layer list. |
| `ElidingLabel.h`, `FlowLayout.h` | Eliding (optionally n-line) label; wrapping flow layout. |

Views and tables:

| Header | What it is, and the one thing to know |
|---|---|
| `CurveTreeView.h` | Curve-catalog tree with caller-driven decorations (demand states, load progress with keep/discard stop affordances, drag opt-out, derived-topic glyphs, displayed-series marks on rows and their groups). The header owns every contract. |
| `HeaderResizePolicy.h`, `HeaderDividerHighlight.h` | Splitter-style header resizing and its divider highlight; install once per header, owned by it. |
| `Scrollbar.h` | Fading pill scrollbar; `attachPillScrollbars` (reserved gutter) is the app-wide default, `kOverlayViewport` only for hosts owning a content-free viewport strip. |
| `Timeline.h` | Multi-track Source Timeline: `TimelineScene` (Qt-free math) + `Timeline` (view). Runtime-agnostic: the host (`SourceTimelineController` in `pj_app`) converts. |
| `StateTransitionsView.h` | State Transitions strip: `StateTransitionsScene` (display-free math) + view; runtime-agnostic like `Timeline`. |
| `TailFollowOverlay.h` | "Jump to the latest" HUD button for a tail-following scroll area. |
| `VisualizationPlaceholderWidget.h`, `VisualizationKind.h` | Empty-dock placeholder with six clickable family icons (Plot, 2D, 3D, Log, State Transitions, Custom Widget) in a grid three columns wide; `VisualizationKind` is the neutral vocabulary the app shell maps to concrete widget kinds. |
| `RasterStreamView.h`, `RasterFrame.h` | External renderer hosted over shared memory (`raster/ipc/`), and its frame decoder. `RasterStreamView` is not built for WebAssembly. |

Helpers:

| Header | What it is, and the one thing to know |
|---|---|
| `TimeFormat.h` | `TimeLabelFormat` + `formatTimeLabel`: seconds or clock-time labels shared by the plot X axis, the playback readout and the Source Timeline's absolute labels so they agree. A clock value below `kMinWallClockSeconds` prints as a duration (`readsAsDuration`). |
| `CoalescingTrigger.h` | Leading + trailing rate limiter; the one "cap the rate at the origin" primitive. Non-copyable. |
| `ChromeMetrics.h` | Shared icon/layout metrics broadcast by `MainWindow`. |
| `Hatch.h` | The one app-wide "no data" hatch, phased to a global grid so every caller is continuous. |
| `Colormap.h` | Scientific colormaps shared by the 2D and 3D views (CPU LUT, GLSL, persisted tokens). |
| `StateColors.h` | Stable categorical colour per state string. |
| `Fnv1a.h` | 64-bit FNV-1a, header-only and Qt-free; the INTERFACE target `pj_fnv1a` (`fnv1a/include/`, linked publicly by `pj_widgets`) so a module without widgets links the hash alone. |
| `FrameworkTokens.h` | Typed reader over the semantic UI palette; holds no values. |
| `Style.h`, `SvgUtil.h` | App `QProxyStyle`; SVG load/recolour helpers. |
| `GraphicsApi.h` | QRhi API policy plus the plumbing every `QRhiWidget` shares. |
| `RhiTestHooks.h`, `RhiBlend.h` | GPU-widget test hooks (tests-only builds); straight-alpha blend (includes `<rhi/qrhi.h>`, so only QRhi renderers include it). |

## Usage notes

- **`MessageBox::question()` in the browser:** it uses `exec()`, which the Asyncify-free browser build cannot nest, so it returns at once. Any confirmation reachable in the browser uses `questionAsync` (modal on desktop, non-blocking in the browser, so never rely on either ordering). `information` / `warning` / `critical` are non-blocking in the browser.
- **Every context menu goes through `showOwnedMenu`:** a `QMenu::exec()` on the browser's main thread aborts the whole application (it cannot nest an event loop). Allocate the menu on the heap, connect each action's lambda to members of a surviving object (never `[&]` over the caller's stack, which is gone once the browser call returns), read nothing after the call, then hand it over. `scripts/pre-commit/check-forbidden-patterns.py` refuses `menu.exec(` (a variable named `*menu*`) outside `OwnedMenu.cpp`.
- **Toast child names are stylesheet selectors:** `toastIcon`, `toastMessage`, `toastCloseButton` appear in the `PJ--ToastNotification` QSS rules; do not rename them.

## Tests

`./test.sh -R '^pj_widgets_tests\.'` runs the main runner. `tests/` holds a focused gtest source per widget whose behavior is non-trivial (`curve_tree_view_test.cpp`, `messagebox_test.cpp`, `file_dialog_test.cpp`, ...); `framework_tokens_test.cpp` guards drift between `FrameworkTokens` and the QSS palette. Add cases to the `pj_widgets_tests` runner in `tests/CMakeLists.txt` with the shared `MAIN gui` QApplication setup: no `main()` and no second application in a test. The runner links `pj_widgets`, `pj_resources`, `pj_runtime` (for `app_settings_test`) and `Qt6::Test`; each case runs in its own process with a 120 s timeout.

`raster_stream_view_test` is a separate executable and ctest entry: it checks the helper process's lifetime with its own `PJ_STUB_HELPER_PATH`, and `raster_stub_helper` is its protocol peer. No test here needs a real GL context.
