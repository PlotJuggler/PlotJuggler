# Visual guidelines

PJ4's theme tokens, border radii, icon pipeline and QSS authoring rules. Review a UI change against this file; when a
change conflicts with it, update the file in the same change.

## Source of truth

- **Palette.** `resources/stylesheet_light.qss` and `stylesheet_dark.qss` each start with a token block between
  `PALETTE START` and `PALETTE END` (`key: value` lines). `pj_app/src/Theme.cpp` parses it and substitutes `${key}`
  in the stylesheet body. Both blocks carry the same keys with per-theme values; the size, type and motion tokens are
  theme-agnostic and duplicated unchanged.
- **Roles.** What each token family means (surfaces, interaction variants and states, ink, outline, overlay,
  selection, elevation, gradients, size axes) is in [`ui_framework.md`](./ui_framework.md). Self-painted (QPainter)
  widgets read the same tokens through `pj_widgets/include/pj_widgets/FrameworkTokens.h`; a value copied into C++
  stops following the theme.
- **Chrome heights** (title bar, section header bands) are runtime metrics (`ChromeMetrics`, changed by the user
  live): use `bandHeight()` / `titleBarHeight()`, never a literal (`pj_widgets/CLAUDE.md`).

## Border radii

Every corner radius comes from the `radius_*` token scale. QSS rules reference `${radius_*}`; self-painted C++ calls
`PJ::theme::radius(PJ::theme::Radius::*)` (radii are theme-independent). The scale and its values are the
`radius_*` keys of the palette; their use:

| Token | Used by |
|---|---|
| `radius_square` | Flat surfaces: chrome bars, panels, tabs, tables/lists, scrollbars, menus, tooltips, popups, frames. |
| `radius_input` | Input chrome (`QLineEdit`, `QAbstractSpinBox`, `QComboBox`, `DoubleScrubber`) and every icon/tool button (`QToolButton`): the hover/press chip behind an icon is a rounded square. |
| `radius_card` | Cards, toasts, and the message-box action buttons. |
| `radius_dialog` | The `PJ::MessageBox` modal card and other floating hint banners. |
| `radius_pill` | Fully-rounded capsules: toggle switches, scrollbar pills, range-slider handles, progress bars. |

Input chrome shares one fill and focus ring (the `input` surface and the focused outline), so text fields, spin boxes
and combo boxes read as one inset-input family. A radius that is not on the scale is a new token, not a literal in one
rule.

App search/filter fields use the `PJ::Search` control (a non-interactive magnifier flush against a borderless field
on one flat background, styled by the `PJ--Search` class rule), so they opt out of the inset-input family. Plugin
fields that remain in a header band (`#seqFilter`, `#topicFilter`) and the prefix editor (`#lineEditPrefix`) opt
out the same way through id selectors.

All `QSplitter` instances use `setHandleWidth(1)` in C++; the handle paints with the divider outline token via QSS, so
there is no separate handle widget to style.

## Icons

### Source family

The Material icons in `resources/svg/` come from Google Material Symbols with the axes that are the defaults of
`resources/scripts/download_material_icons.py`: `rounded` style, weight 300, grade 0, fill 0, optical size 48.
`ICON_MAPPING` in the script maps each target filename to a Material Symbol name plus optional transforms
(`rotate`, `hflip`, `vflip`, a per-entry `fill` or `fill_color`).

To add an icon, add an `ICON_MAPPING` entry and run:

```bash
python3 resources/scripts/download_material_icons.py -o resources/svg/
```

The script downloads each SVG from the gstatic CDN (`--dry-run` prints the URLs, `--overwrite` replaces existing
files) and injects `--fill-color` (default `#3D3D3D`) as the `fill` of every `<path>`.

### Ink and recoloring

`#3D3D3D` is the authoring ink of a theme-following icon. `recolorSvgInk()` (`pj_widgets/include/pj_widgets/SvgUtil.h`,
applied by the SVG loaders) folds that ink, legacy black and white, and the opposite theme's ink onto the active
theme's icon ink, and gives a root `<svg>` without any `fill` one. An icon that must follow the theme is authored
with `#3D3D3D`; `fill="currentColor"` would not be recolored.

State variants are separate files for QSS `url()` references, which cannot be recolored at run time; their ink is
fixed in the file:

| Suffix | Fill / stroke | Used for |
|---|---|---|
| none / `_light` | `#3D3D3D` | Light theme. |
| `_dark` | `#E0E0E0` (used raw, never folded) | Dark theme. |
| `_disabled_light` / `_disabled_dark` | as `_light` / `_dark` | Disabled state. |
| `_hover` | `#1177FF` | QSS `:hover` (combo-box arrows). |
| `_pressed` | `#CC00CC` | QSS `:pressed`. |

Some SVGs are not from Material Symbols and keep their own ink: `branch_*_light.svg` `#B8B8B8` and
`branch_*_dark.svg` `#454545` (tree indicators), `close_windows_*`, `maximize` and `minimize` (strokes in the
`#3D3D3D` / `#E0E0E0` family), `panel_*` and `play_arrow*` (`#3D3D3D`), and `plotjuggler.svg`, a multi-color logo that
is not recolored.

## QSS authoring discipline

- **Tokens, not literals.** Reference a palette token for every color, size and padding in a stylesheet body. A
  hard-coded hex, rgba or px value is promoted to a token before a second rule reuses it, so a color or spacing
  changes in one place.

  ```css
  /* avoid */  QFrame#mySection { background: #e0e0e0; border-bottom: 1px solid #c0c0c0; padding: 4px; }
  /* use   */  QFrame#mySection { background: ${banner}; border-bottom: ${stroke_hairline}px solid ${separation}; }
  ```

- **Reuse, don't copy.** Widgets that share a treatment share one rule (`#TitleBar, QDialog QWidget#dialogTitleBar {
  ... }`); related rules cluster under a comment header (see "Frameless chrome bars" and "Chrome buttons" in the
  stylesheets).
- **Theme symmetry.** The light and dark stylesheets differ only in palette values. A selector or property in one
  and not the other is a bug.
- **Padding and margins inside chrome rows.** Outer row layout `contentsMargins(0, 0, 0, 0)`; no per-button padding
  (the icon's own padding is the only ring); a label that needs separation from the row's left edge gets a
  `space_*` token margin.
- **`flush="true"`** on a `QTabWidget` removes its global inset: QSS zeroes the widget and pane margins and padding
  and the tab-bar offset.

### Inline `setStyleSheet`

Inline `QWidget::setStyleSheet(...)` is the exception. Before using it:

1. Could it be an `objectName` plus a QSS rule (`QPushButton#foo { ... }`)? Then do that.
2. Is the value data, such as a per-row color swatch where each instance carries its own color? Then inline is the
   right expression.
3. Is it a polish for a Qt internal that ignores QSS (the `QwtPlotCanvas` backing store, `QComboBox` popup palettes in
   `WidgetTuner`)? Then it stays in C++ with a comment saying why.

`setStyleSheet("")` calls deliberately clear inherited styling (`PlotDocker` disabling Qt-Advanced-Docking's own QSS
layer).

## Runtime helpers

Some chrome lives in C++ because Qt's default styling has internals QSS cannot reach:

- `recolorSvgInk()` in `pj_widgets/include/pj_widgets/SvgUtil.h`, described above.
- `pj_app/src/WidgetTuner`: an application-wide event filter that gives auto-generated context menus
  `objectName="PJMenu"` (so the `QMenu#PJMenu` rules apply) and clamps `QComboBox` popup palettes.
- `PJ::Style` (`pj_widgets/include/pj_widgets/Style.h`): a `QProxyStyle` over Fusion that suppresses the platform's
  dialog-button icons (`SH_DialogButtonBox_ButtonsHaveIcons`) and mnemonic underlines (`SH_UnderlineShortcut`);
  installed in `pj_app/src/main.cpp`.

A new widget category that needs runtime polish extends one of these helpers.
