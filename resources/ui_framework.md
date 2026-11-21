# UI Framework

A **semantic** styling framework: every color, size, and state is defined by *what
a thing is*, *what it does*, and *its current state* — never by a raw literal at
the call site. This indirection lets us re-theme the app by editing the token
values without touching the semantics that reference them.

The ground truth is the token block (`PALETTE START` to `PALETTE END`) of
`resources/stylesheet_{dark,light}.qss`; `pj_widgets/include/pj_widgets/FrameworkTokens.h` is a typed reader
over it and holds no values. The tables below state the current design intent. The unit of the framework is a
**role** with coupled facets (a fill, the ink legible on it per state, an outline), not a single color; an
extension grows `Surface` or `Variant` into such a bundle instead of adding a parallel flat color list.

The framework has two axes:

- **Surfaces** — the background roles a region can play (Surfaces below).
- **Interactions** — the per-state color ramp an interactive element walks through (Interactions below).

Every color is given for both the **Dark** and **Light** themes.

---

## Surfaces

A surface is a background role. Pick the surface by the region's *purpose*, then
read its color from the theme in use.

| # | Surface | Dark | Light |
|---|---|---|---|
| 1 | Banner | `#1C1C22` | `#D6D6D6` |
| 2 | Backdrop | `#373743` | `#EBEBEB` |
| 3 | Data Backdrop | `#212128` | `#FFFFFF` |
| 4 | Input | `#535365` | `#F5F5F5` |
| 5 | Separation | `#5A5A68` | `#ADADAD` |
| 6 | Banner Input | `#25252D` | `#E0E0E0` |
| 7 | Scroll Handle | `#007AF5` | `#47A3FF` |

### 1. Banner

A horizontal or vertical bar that visually separates the content that follows. It
usually holds a **title** on the leading side and a **workspace** on the trailing
side.

- The title is a text label.
- The workspace can contain any widget — usually a filter line edit, buttons, and
  chevrons.

A banner has a uniform height (if horizontal) or width (if vertical). A horizontal
banner is usually followed by a separator below it when at the top of the window,
and by separators above *and* below when in the middle.

- **Dark:** `#1C1C22`
- **Light:** `#D6D6D6`

**Examples**

- LHS panel — `"Datasets [Filter] Kebab"`, `"Custom Series [Filter]"`
- Mosaico — `"Sequences [Filter]"`, `"Topics [Filter]"`

### 2. Backdrop

The default window background color. It is the starting point for all windows,
frames, popups, and dialogs.

- **Dark:** `#373743`
- **Light:** `#EBEBEB`

### 3. Data Backdrop

The background of any widget, label, or frame that contains interactable, editable
data — charts, tree views, spin boxes, scrubbers. It is the surface farthest from
the Backdrop toward the text's opposite: *lighter* than the Backdrop in the light
theme, *darker* (near-black) in the dark theme, so curves, text and values have the
most contrast.

- **Dark:** `#212128`
- **Light:** `#FFFFFF`

**Examples**

- Main Plot
- Datasets tree view
- Mosaico Datasets table
- Mosaico Topics table
- CSV table

### 4. Input

Any editable widget with text entry — mostly line edits and text edits, including
filter searches.

- **Dark:** `#535365`
- **Light:** `#F5F5F5`

### 5. Separation

A line or gap that acts as a buffer or border between objects. Used mostly by the
draggable Separator widget, which turns a purple color when grabbed.

- **Dark:** `#5A5A68`
- **Light:** `#ADADAD`

### 6. Banner Input

The special case of a line edit or text edit that lives inside a Banner.

- **Dark:** `#25252D`
- **Light:** `#E0E0E0`

### 7. Scroll Handle

Anything that can be grabbed to move a page forward or backward — excluding
special sliders such as the timeline and playback controls.

- **Dark:** `#007AF5`
- **Light:** `#47A3FF`

### Surface variants

Interactable frames and buttons (elevated surfaces) split into these variants.
Each variant's per-state colors are defined under Interactions.

Named by *visual emphasis*, not by semantic state (we rarely mean a literal
error/warning):

- **Neutral** — grey; the quiet default surface.
- **Accent** — blue; the main accent / action / selection color.
- **Highlight** — magenta; a strong attention pop.
- **Emphasis** — yellow; a softer attention tone.
- **Special** — a per-state gradient from the Accent endpoint to the Highlight endpoint.

---

## Interactions

An interactive element that is not disabled walks through this state ramp:

1. Nominal
2. Hovered
3. Checked
4. Pressed
5. Checked Hovered
6. Checked Pressed
7. Disabled

Crossing the 5 surface variants with these 7 states yields a color grid per theme.

### Dark

| Variant | Nominal | Hovered | Checked | Pressed | Checked Hovered | Checked Pressed | Disabled |
|---|---|---|---|---|---|---|---|
| Neutral | `#8F8FA3` | `#787891` | `#5C5C70` | `#5C5C70` | `#40404F` | `#2E2E38` | `#25252D` |
| Accent | `#1F8FFF` | `#007AF5` | `#0066CC` | `#0066CC` | `#00478F` | `#003366` | `#25252D` |
| Highlight | `#FF33A7` | `#FF0A95` | `#CC0074` | `#CC0074` | `#8F0051` | `#66003A` | `#25252D` |
| Emphasis | `#F5D33D` | `#F3CA16` | `#C2A00A` | `#C2A00A` | `#887007` | `#615005` | `#25252D` |

### Light

| Variant | Nominal | Hovered | Checked | Pressed | Checked Hovered | Checked Pressed | Disabled |
|---|---|---|---|---|---|---|---|
| Neutral | `#E0E0E0` | `#CCCCCC` | `#B8B8B8` | `#B8B8B8` | `#A3A3A3` | `#999999` | `#333333` |
| Accent | `#D6EBFF` | `#C2E0FF` | `#99CCFF` | `#99CCFF` | `#70B8FF` | `#5CADFF` | `#333333` |
| Highlight | `#FFD6FF` | `#FFC2FF` | `#FF99FF` | `#FF99FF` | `#FF70FF` | `#FF5CFF` | `#333333` |
| Emphasis | `#F5D33D` | `#F3CA16` | `#C2A00A` | `#C2A00A` | `#887007` | `#615005` | `#333333` |

### Special

The **Special** variant is not a fixed color: for each state it is a gradient that
runs from the Accent value to the Highlight value of that same state (in the active
theme).

| State | Gradient |
|---|---|
| Nominal | Accent Nominal → Highlight Nominal |
| Hovered | Accent Hovered → Highlight Hovered |
| Checked | Accent Checked → Highlight Checked |
| Pressed | Accent Pressed → Highlight Pressed |
| Checked Hovered | Accent Checked Hovered → Highlight Checked Hovered |
| Checked Pressed | Accent Checked Pressed → Highlight Checked Pressed |
| Disabled | Accent Disabled → Highlight Disabled |

---

## Foreground

**Content ink** for text and icons. A caller asks for ink *relative to the fill
it sits on* — never a raw literal:

- `onSurface(surface, emphasis)` — ink on a background surface; emphasis ∈
  {Default, Muted, Disabled}.
- `onFill(variant, state)` — ink on a solid interaction fill. The legible ink
  **flips by state** (WCAG): e.g. dark Highlight *Nominal* `#FF33A7` needs dark ink
  but dark Highlight *Checked* `#CC0074` needs light ink, so ink is chosen per cell,
  not per variant.
- `onSpecial(state)` — ink over the Special (Accent→Highlight) gradient.

`text` is a compatibility alias for `onSurface(Backdrop, Default)` — the primary
body-text ink:

- **Dark:** `#FFFFFF`
- **Light:** `#333333`

On-fill ink is near-black `#111111` / near-white `#F0F0F0` chosen per cell for
≥4.5:1 contrast; a cell those cannot reach (for example Neutral Hovered in the dark theme) uses absolute
`#000`/`#FFF`.

## Icons

Icons carry their own **icon ink**, with a nominal and a disabled state. Nominal
icons match the body-text ink; disabled icons drop to a muted mid-grey.

- **Nominal**
  - **Dark:** `#FFFFFF`
  - **Light:** `#333333`
- **Disabled**
  - **Dark:** `#8F8FA3`
  - **Light:** `#808080`

## Outline / Stroke + Focus

**Stroke ink** for borders, dividers, gridlines, focus rings, and checked
outlines — distinct from `Surface::Separation`, which is a background *fill*.
Accessor: `outline(role, state)` with roles {Default, Interactive, Divider,
Gridline} and states {Rest, Hovered, Focused, Checked, Disabled}. Rest reuses the
old `border_default`, Hovered reuses `border_hover`, and **Focus is a distinct
ring** (`outline_*_focused`) rather than being collapsed into hover. Focus is drawn as a
ring over the nominal fill, so `State::Focused` on the interaction grid resolves
to Nominal. Push buttons are the exception: `PJ::Style` draws their keyboard-focus
ring as the button's own 2px edge, the fill mixed half and half with the theme's `text`
ink (soft pink on the dark destructive fill, deep purple on the light one), because the
blue ring has no contrast on the neutral grey fill (about 1.3:1 in dark). It shows only
after keyboard navigation.

## Status Text

Status text (a warning or an error a label reports) uses the `status_warning_ink` / `status_error_ink` tokens, which
reach 4.5:1 on the backdrop in both themes; `status_*` without `_ink` are the brighter tones for icons and dots. A
label opts in with the dynamic property `pjStatus` = `"warning"` or `"error"` (the `QLabel[pjStatus=...]` rules), so
a plugin `.ui` names a role instead of a hex colour that cannot read in both themes. Setting the property after the
label is shown needs `style()->unpolish(label)` and `polish(label)`: Qt evaluates attribute selectors at polish.

## Overlays and Selection

**Overlays** are alpha-bearing (`overlay(kind)`): `Hover` / `Pressed` / `Selected`
state layers (theme-inverted: white-alpha on dark, black-alpha on light), plus
`Scrim` (modal dim) and `Hud` (scene panel veil). **Selection** is a fill+ink pair
(`selectionFill`/`onSelection`), distinct from interaction `Checked`: `Item` for
opaque view/list/tab selection and `TextEdit` for the translucent editor
selection.

## Elevation Surfaces and Gradients

A parallel `ElevatedSurface` tier for raised/floating shades that aren't among the
base seven: `Dialog` (dialog/menu/popup body), `Card` + `CardHover`, `Toast`, and
`Disabled`. **Gradients** are named two-stop endpoint pairs (`gradient(role)` →
`{start, end}`): `Accent` (light-purple→light-blue) and `Brand`
(blue→purple); QSS composes `qlineargradient(... stop:0 ${gradient_accent_start},
stop:1 ${gradient_accent_end})`.

## Size / Non-Color Axes

Geometry, typography, and motion are semantic roles too (theme-agnostic; stored in
the same palette). Accessors: `space()` (None/Tight/Snug/Comfortable/Section =
0/2/4/8/16), `metric()` (the input min and outer heights), `radius()` (Square/Input/Card/Pill), `stroke()`
(Hairline/Emphasis), `duration()` (Fast/Base/Slow/RevealHold = 140/180/300/1200
ms), and `type()` → `{size, weight, family}` for Caption/Body/Title/Heading.
The values are the `space_*`, `radius_*`, `stroke_*`, `motion_*` and `type_*` keys of the palette; the family
is `sans-serif`.

---

## Scope

Categorical **data-series palettes** (curve colors, colormaps, the `SourceTimeline` qualitative palettes, 3D
materials) are data colors, not chrome, and are not part of this framework.

## Known gaps

- `Checked` and `Pressed` hold identical values in every variant, so the ramp cannot tell a persistent selection from
  a transient press.
- The state ramp has no `Selected` (distinct from `Checked`), `Active/Current` (current tab, live playhead) or
  `ReadOnly` state.
- Caret ink is not themed (reasoning from a review of the widgets, not re-checked). Link and placeholder ink are:
  the `link` and `on_input_muted` tokens reach `QPalette::Link`/`PlaceholderText` through pj_app `Theme.cpp`, and the
  `QLineEdit` rule sets `placeholder-text-color`.
- The `chrome_*` palette keys (bar height, button size, icon size, spacing, border width) are not referenced by any
  stylesheet rule or by C++; chrome heights come from `ChromeMetrics` (`pj_widgets/CLAUDE.md`).
