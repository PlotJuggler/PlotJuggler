# UI Framework — Roadmap / Missing Token Types

Backlog of framework extensions identified by a critical review (4 independent
agents) of [`ui_framework.md`](./ui_framework.md) + the `FrameworkTokens` layer.

**Status: the TOKEN LAYER for rows 1–8 is now BUILT** — QSS palette tokens (both
themes) + typed `FrameworkTokens` accessors + completeness/contrast tests (22
passing). What remains is **widget migration**: repointing the ~2000 lines of QSS
rule bodies and the self-painted C++ off the legacy `border_*`/`accent_*`/
`hover_overlay`/`chrome_*` tokens and hardcoded literals onto these roles, then
retiring the legacy tokens and `ThemeColors.h`. Row 9 and the state-ramp additions
below remain deferred. Per-row residual follow-ups are noted as "migrate …".

## Framing decisions already made

- **Ground truth = the QSS palette.** `FrameworkTokens.{h,cpp}` is a typed reader
  over it; it holds no values. Any new type below is added as QSS palette tokens
  first, then exposed through a typed accessor.
- **Values = design intent, not current look.** The framework carries the
  *intended* colors; migration repoints widgets onto them and accepts the visual
  shift. (Exception: the Foreground role ships with **placeholder** values to be
  tuned later.)
- **The unit is a role, not a color.** The deepest miss: a token was modeled as a
  single color. The real unit is a **role with coupled facets `{fill, ink,
  outline}`** — e.g. "Neutral Checked" = a fill + the ink legible on it (which
  flips by state) + maybe a border. Extensions below should grow `Surface` and
  `Variant` into facet-bundles rather than adding parallel flat color lists.

## Token types (rows 1–8: token layer BUILT ✅ — migration pending; row 9 deferred)

| # | Type | Status | Proposed shape |
|---|---|---|---|
| 1 | **Foreground / ink** | CRITICAL | `text` (shipping now, placeholder). Then grow to `on_surface(surface, emphasis)` + `on_fill(variant, state)` + `on_special(state)`, emphasis ∈ {default, muted, disabled}. Ink must vary by **state** (WCAG: Highlight Nominal wants dark ink, Highlight Checked wants light ink; Emphasis yellow always needs dark ink). |
| 2 | **Outline / stroke** (stateful) + **Focus ring** | CRITICAL | `outline(role, state)` where state adds **Focused**. Covers input borders, focus rings, checked outlines, gridlines. Distinct from `Surface::Separation` (a fill, not a stroke). Add `Focused` to the `State` enum. |
| 3 | **Overlay / alpha** | HIGH | Alpha-bearing tokens: `overlay(hover/pressed/selected)` state-layers, `scrim` (modal dim), `hud` (panel translucency). The interaction grid is solid RGB; there is no alpha concept today. |
| 4 | **Selection** | HIGH | `selection_fill` + `on_selection`, distinct from `Checked`. Possibly a separate text-edit selection pair. |
| 5 | **Status variants** | MED-HIGH | Add **Success / Info / Neutral** (+ hovers) to the variant set. Also fix the **inverted naming**: today "Neutral" is neutral-grey and "Accent" is the blue action accent — reconcile with the convention (Neutral = action/brand). App already uses `accent_success/info/neutral`. |
| 6 | **Elevation surface tier** | MED-HIGH | Raised/overlay/card/menu/dialog/toast/disabled surfaces beyond the 7. Dialogs/menus/popups currently paint `dark_background` (≠ `Backdrop`); `toast_background`, `marketplace_card_bg`, `widget_background_disabled` are off-framework shades. |
| 7 | **Gradient** | MED | Generalize beyond `special()` (Neutral→Highlight only): the shipping `light_purple→light_blue` (MessageBox, ComboBox delegate) and `blue→purple` (splash) are not expressible. |
| 8 | **Size family (non-color)** | HIGH (separate axis) | The user's framing ("not by fixed size") puts geometry in scope. Add role-named scales: **spacing** (219 hardcoded sites), **sizing** (wire the *dead* `chrome_*` tokens — 0 refs today), **radius** (collapse the 5 duplicated `kCornerRadius*` constants), **stroke width**, **typography** (family/size/weight — 31 hardcoded sites, zero tokens), **motion** (duration roles — 8 per-widget constants). Same ground-truth model: role → value in the palette, read by a typed accessor (`metric()/radius()/duration()/type()`). Consolidate/retire `visual_guidelines.md` §2 (a drifting shadow geometry-doc naming tokens by widget-instance, not role). |
| 9 | Link / placeholder / caret ink | LOW | Toast `<a href>` uses Qt-default `QPalette::Link`; fields set placeholders unthemed. |

## State-ramp additions to consider

Beyond the current 7 states: **Focused** (keyboard focus — required by #2),
**Selected** (≠ `Checked`), **Active/Current** (current tab, live playhead),
**ReadOnly** (enabled + readable but not editable). Note: `Checked` and `Pressed`
currently hold **identical** values — the ramp can't distinguish persistent
selection from a transient press.

## Explicitly OUT of scope (chrome framework)

**Categorical data-series palettes** — curve colors, colormaps (turbo/viridis),
`SourceTimeline`/tab10 qualitative palettes, 3D materials. These are *data*
colors, not chrome, and must NOT be tokenized into this framework.

## Migration blocker to resolve up front

The 7 framework surface **values** don't match what widgets paint today:
`Surface::Input` `#535365` vs shipping `#4D4D5A`; `Surface::ScrollHandle`
`#007AF5` vs `#787878`; `Surface::Backdrop` `#373743` vs dialogs' `#3B3B47`. Per
the values decision above, migration adopts the framework values (visual shift).

---

_The detailed analysis behind these values was produced in an uncommitted
review scratchpad and is not reproducible from this repository. The decisions it
supports are recorded in full above; treat this document as the record._
