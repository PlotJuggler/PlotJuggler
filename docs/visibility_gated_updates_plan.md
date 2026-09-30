<!-- SPDX-License-Identifier: MPL-2.0 -->
# Don't update non-visible widgets — design & plan

> **Status (2026-06-22).** A first slice shipped in the
> `perf/playback-update-cost` PR: a ~30 Hz rate cap on the playhead fan-out
> (`CoalescingTrigger` + the lowered tick) and a **conservative active-top-level-tab
> gate** (`MainWindow::broadcastTrackerTimeToVisible`). What remains in this
> document is the **fuller, deferred** gate: an `isVisible()`-based predicate
> covering maximized-sibling / floating-dock / scene-streaming-seam cases, a
> `showEvent` catch-up, and the latent floating-dock enumeration fix. It is kept
> here because the corner cases below were adversarially verified and should guide
> that follow-up.

Synthesized from a Claude multi-agent investigation (42 agents) + an independent
Codex pass, both grounded in the code. Every corner case below was adversarially
verified.

## Problem

During playback and streaming, per-tick work is delivered to docks the user
**cannot see** — plots on an inactive plot tab, and docks hidden behind a
maximized ("fullscreen") dock. Each wasted tick costs a Qwt `replot()` (plot) or
a layer-advance + `refreshView()` and, for 2D, an eager off-thread image decode
(scene). The window-compositing tax that dominated profiling came from the
zero-size `rhi_bootstrap` widget, which is gone; this work removes the
*per-widget* waste behind it.

## Two delivery seams (both must be gated)

1. **Per-tick playhead fan-out.** `PlaybackEngine::currentTimeChanged` →
   `MainWindow::broadcastTrackerTime` (`MainWindow.cpp:759`) →
   `forEachDock`/`forEachDocker` (`:1868–1885`) → `DockWidget::onTrackerTime`
   (`DockWidget.cpp:253`) → `PlotWidget::setTrackerPosition` (**unconditional**
   `replot()`, `PlotWidget.cpp:687`) and `SceneDockWidget::onTrackerTime`
   (renderKey-gated but **visibility-blind**, `scene_dock_widget.cpp:353`).
   `forEachDocker` walks **every** tab's docker (`TabbedPlotWidget::dockerAt`),
   not just the visible one; within a tab ADS already enumerates only the front
   dock.
2. **Streaming self-subscriptions.** Scene docks connect **directly** to
   `SessionManager::samplesIngested` (`Scene2DDockWidget.cpp:344`,
   `Scene3DDockWidget.cpp:343`) and self-drive `driveVisibleLayersToLiveEdge` —
   bypassing `forEachDock` entirely. A gate placed only at the playhead seam
   leaves hidden scene docks doing full per-sample work.

## Design

### Visibility predicate
Use **`QWidget::isVisible()`** on the dock content — it goes false for an
inactive-tab dock (QStackedWidget hides non-current pages), a maximize-hidden
sibling (`DockWidget` fullscreen path `setVisible(false)`), and a minimized
window. **Do not** key on "active tab" (`currentTab()`) alone: it misses
maximize-within-the-active-tab and mis-classifies floating docks. Optionally add
`!window()->isMinimized()`.

### Gate placement (altitude)
- **Playhead seam:** skip `!isVisible()` docks. Keep enumerating all dockers for
  *structural* ops (layout save) — only the tracker fan-out is gated.
- **Streaming seam:** add the same `isVisible()` check inside the scene docks'
  `samplesIngested` handlers (they bypass the host), or surface one shared
  "visible/active dock" query the host owns and both paths consult.
- **DO NOT gate the structural seed broadcasts** — restore (`:2380/:2503/:2782`),
  undo/redo, and per-create seeds (`:497/:511/:1133`). These are the *sole* seed
  for freshly-restored/created docks because `currentTimeChanged` fires only on a
  time *change*. Mixing them under the gate is the headline failure mode.

### Catch-up on show (mandatory, paired with the gate)
A gated-out widget is push-only and freezes at its last received time. When it
becomes visible it must catch up to the **current** playhead — a *pull* of
`PlaybackEngine::currentTime()` (collapses N skipped ticks → 1; all refresh
primitives are time-addressable, including the `SceneEntitiesLayer` accumulator).
Hooks:
- **Tab switch:** extend the existing `TabbedPlotWidget::currentTabChanged`
  handler (`MainWindow.cpp:1084`, today only `onDockFocused`) to re-push
  `onTrackerTime(toAxisDouble(currentTime()))` to the now-active docker's docks.
- **Maximize/restore within a tab:** a `DockWidget` `showEvent`/`QEvent::Show`
  re-seed (no `showEvent` override exists today — must be added). This also
  uniformly covers any future ADS-native maximize.
- **Scene specifics:** route through `onTrackerTime` (so `last_tracker_` updates)
  and, while following a live stream, re-drive `driveVisibleLayersToLiveEdge`; for
  Scene3D call `invalidateTrackerRenderKey()` after the out-of-gate live-edge push
  so the coalescing key matches on-screen pixels.

### Floating docks (latent bug — fix in the same change)
`forEachDocker` only walks each `PlotDocker`'s own `d->DockAreas`; a tab-dragged
**floated** dock moves into a separate `CFloatingDockContainer` that enumeration
never visits — so a floated dock gets **no** tracker time *today*, before any
gate. (Float is reachable: `DockWidgetMovable` stays on even though
`DockWidgetFloatable` is cleared.) Fix: also enumerate floating containers, and
let `isVisible()` (true for an on-screen floating window) drive the gate.

## Corner cases — all CONFIRMED by adversarial verification

| Scenario | Risk if gate added naively | Safeguard |
|---|---|---|
| Tab A→B (esp. paused) | B shows stale tracker/scene; `currentTabChanged` only rebinds the right panel | re-push current time on tab change |
| Maximize → restore | restored sibling stale; `currentTab()` can't even detect it | `isVisible()` gate + `showEvent` re-seed |
| Floating dock | gets no tracker time *today*; tab-membership gate mis-classifies it | enumerate floating containers + `isVisible()` |
| Layout restore (incl. `--layout` before show) | hidden-tab docks render blank if the seed is gated | exempt structural seeds; first-show re-seed |
| Streaming into hidden tab | scene self-subscriptions keep working unless also gated; then stale on show | gate streaming seam + live-edge catch-up on show |
| Repeated hidden scrubbing | N→1 collapse is sound, but the *trigger* (catch-up) doesn't exist today | pull `currentTime()` once on show (no tick replay) |

## Test plan (TDD)
- Plot: hidden-tab plot receives no `replot()` per tick; on tab-show, tracker
  marker matches `currentTime()` (offscreen-friendly — assert tracker position,
  not pixels).
- Scene: hidden dock's `onTrackerTime`/`setTimestamp` not invoked per tick; on
  show, `last_tracker_` equals current time and one repaint is requested.
- Streaming: hidden scene dock's `driveVisibleLayersToLiveEdge` suppressed; on
  show, advances to the live edge in one call.
- Floating: a floated dock is enumerated and receives tracker time.
- Restore: multi-tab restore leaves hidden tabs correct on first reveal (structural
  seed not gated).

## Why this is a separate PR
Larger surface (pj_app host + pj_plotting + pj_scene_common + scene2D/3D), a
latent floating-dock enumeration bug to fix alongside, and a new `showEvent`
re-seed contract — each with corner cases worth their own review.
