# pj_widgets

## Purpose

Reusable Qt widgets and UI helpers: controls that could be dropped into another Qt application without dragging PJ4-specific state with them. Static library, `Qt6::Core` / `Gui` / `Network` / `Svg` / `Widgets` only (public), plus a PRIVATE `Qt6::GuiPrivate` link for `<rhi/qrhi.h>`.

Each header's doc comment owns that widget's contract (threading, ownership, call order, pitfalls); the tables below say what exists and flag the one thing to know. No `docs/` folder by design.

## Hard constraints (from root CLAUDE.md)

- Depends only on **Qt and the C++ standard library** (plus the dependency-free, header-only `raster/ipc/` wire-protocol contract, which is not a PJ module).
- **No dependencies on `pj_runtime`, `pj_app`, or any other PJ module.** Anything that needs runtime state belongs upstream of here; widgets take plain types and emit intent signals, and the host converts.
- A widget specific to PlotJuggler workflows (e.g. plot-specific dockers) belongs in its widget-family module instead.

## Rules

- **Always use `PJ::ComboBox`, never a plain `QComboBox`** (promote `QComboBox` to `PJ::ComboBox` in `.ui` files, header `pj_widgets/ComboBox.h`); a plain combo loaded from a plugin `.ui` is upgraded in place with `applyComboBoxStyling()`.
- **Every filter field is `PJ::Search`**: promote `QLineEdit` search fields to `PJ::Search` in `.ui` files; do not hand-roll a glyph + line edit.
- **Every PJ4 table header** gets `HeaderResizePolicy::install`, (`HeaderDividerHighlight::install` adds the divider highlight); never mark a section `Stretch` or `ResizeToContents`, which makes its dividers dead.
- **Theme colours come from `FrameworkTokens.h`**: the QSS palette is the single ground truth, and self-painted widgets use the header because they cannot reach QSS `${token}`. A surface's fill is chosen by its role, never by a raw literal.
- **Every `QRhiWidget` requests `preferredGraphicsApi()`**: one API per top-level window.
- **Append-only enumerations:** `Colormap` (LUT row and persisted id) and the `StateColors` palette (index = hash % size). Reordering silently recolours or corrupts saved data.
- **Chrome heights are never re-derived:** use `ChromeMetrics::bandHeight()` / `titleBarHeight()`; never `setFixedHeight` a `SectionHeaderBand`.

## UI convention

Per root CLAUDE.md: **prefer `.ui` files**. `AUTOUIC` is on, search path `src/`. The only `.ui` is `Dialog.ui`; keep new chromed dialogs on the same pattern.

## Inventory

Dialogs, chrome and feedback:

| Header | What it is, and the one thing to know |
|---|---|
| `Dialog.h` + `Dialog.ui` | Frameless app-styled dialog chrome: title-bar actions, optional side drawer. Base of `FileDialog`, `ProgressDialog`. |
| `SideDrawer.h` | `makeSideDrawerSplitter`: draggable side column. The same `settings_key` shares the remembered width across presentations (`Dialog::setSideWidget`, `pj_app` toolbox panels). |
| `FileDialog.h` | `QFileDialog` in `Dialog` chrome; use the static helpers. A start folder that does not answer within 2 s (dead network share) falls back to home with a notice; Qt's shared `QtProject/FileDialog` memory is filtered the same way before construction. |
| `MessageBox.h` | Themed `QMessageBox` replacement. `question()` is desktop-only in practice: see Usage notes. |
| `ProgressDialog.h` | Modal progress with domain-neutral Primary/Secondary stop actions; no close affordance. |
| `ProgressBar.h` | App-styled `QProgressBar` (`PJ--ProgressBar` QSS class). `Q_OBJECT` is mandatory or the selector never matches. |
| `IngestProgressWidget.h` | Title-bar progress strip; shows or hides only on `setActive()` (timing is owner policy). |
| `ToastManager.h`, `ToastNotification.h` | Bottom-right toasts over a host widget; forward the host's `resizeEvent` to `updatePosition()`. Message is rich text with external links. |
| `OwnedMenu.h` | `showOwnedMenu`: self-deleting context menu, modal on desktop and non-blocking in the browser; act through signal handlers, never read results after the call. |
| `RecentFilesMenu.h` | Persistent MRU menu; `enabledChanged` is not emitted by the constructor, seed it via `isEmpty()`. |
| `RenderingWarningLabel.h` | Hidden themed warning label the 2D/3D docks show while their GPU view cannot render. |

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
| `IntScrubber.h`, `DoubleScrubber.h`, `ScrubberBase.h` | Drag-to-scrub numeric inputs; spin-box API, promote in `.ui`. |
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
| `CurveTreeView.h` | Curve-catalog tree with caller-driven decorations (demand states, load progress with keep/discard stop affordances, drag opt-out, derived-topic glyphs). The header owns every contract. |
| `HeaderResizePolicy.h`, `HeaderDividerHighlight.h` | Splitter-style header resizing and its divider highlight; install once per header, owned by it. |
| `Scrollbar.h` | Fading pill scrollbar; `attachPillScrollbars` (reserved gutter) is the app-wide default, `kOverlayViewport` only for hosts owning a content-free viewport strip. |
| `Timeline.h` | Multi-track Source Timeline: `TimelineScene` (Qt-free math) + `Timeline` (view). Runtime-agnostic: the host (`SourceTimelineController` in `pj_app`) converts. |
| `StateTransitionsView.h` | State Transitions strip: `StateTransitionsScene` (display-free math) + view; runtime-agnostic like `Timeline`. |
| `TailFollowOverlay.h` | "Jump to the latest" HUD button for a tail-following scroll area. |
| `VisualizationPlaceholderWidget.h`, `VisualizationKind.h` | Empty-dock placeholder with five clickable family icons (Plot, State Transitions, 2D, 3D, Custom Widget); `VisualizationKind` is the neutral vocabulary the app shell maps to concrete widget kinds. |
| `RasterStreamView.h`, `RasterFrame.h` | External renderer hosted over shared memory (`raster/ipc/`), and its frame decoder. `RasterStreamView` is not built for WebAssembly. |

Helpers:

| Header | What it is, and the one thing to know |
|---|---|
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
- **Toast child names are stylesheet selectors:** `toastIcon`, `toastMessage`, `toastCloseButton` appear in the `PJ--ToastNotification` QSS rules; do not rename them.

## Tests

`tests/` holds a focused gtest source per widget whose behavior is non-trivial (e.g. `curve_tree_view_test.cpp`, `messagebox_test.cpp`, `colormap_test.cpp`, `file_dialog_test.cpp`; `framework_tokens_test.cpp` guards drift between `FrameworkTokens` and the QSS palette). Add ordinary cases to the `pj_widgets_tests` runner in `tests/CMakeLists.txt`, using the shared `MAIN gui` QApplication setup instead of defining a `main()` or creating another application in a test. The runner links `pj_widgets`, `pj_resources`, and `Qt6::Test`; `pj_add_test_runner` discovers each case with the `pj_widgets_tests.` prefix and runs it in its own process with a 120-second timeout.

`raster_stream_view_test` is a separate executable and ctest entry (`RasterStreamView`): it checks the helper process's lifetime and has its own `PJ_STUB_HELPER_PATH`; `raster_stub_helper` is its protocol peer. No test here requires a real GL context.
