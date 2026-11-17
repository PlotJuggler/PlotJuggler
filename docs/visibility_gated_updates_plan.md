<!-- SPDX-License-Identifier: MPL-2.0 -->
# Don't update non-visible widgets — open proposal

## What exists

The playhead fan-out is rate-capped (`CoalescingTrigger` on `PlaybackEngine::currentTimeChanged`, interval
`kTrackerBroadcastIntervalMs` in `MainWindow.cpp`) and gated to the active plot tab:
`MainWindow::broadcastTrackerTimeToVisible` walks only the current tab's docks (`forEachVisibleDock`) and the embedded
scene views that are `isVisible()`. `TabbedPlotWidget::currentTabChanged` re-seeds the tab that becomes active.
Structural seeds (restore, undo/redo, dock creation) use `broadcastTrackerTime`, which reaches every tab.

## What is still wasted

- **Docks hidden by a maximized sibling.** `PlotDocker::toggleFullscreen` hides siblings with `toggleView(false)`,
  but `forEachVisibleDock` still visits them, so each tick still costs a hidden plot's `replot()` or a hidden scene
  dock's layer advance. `currentTab()` cannot see this case at all.
- **The streaming seam.** `Scene2DDockWidget` and `Scene3DDockWidget` connect directly to
  `SessionManager::samplesIngested` and drive their layers to the live edge (`driveVisibleLayersToLiveEdge`),
  bypassing the host fan-out, so a scene dock on a hidden tab does full per-sample work while streaming.
- **A minimized window** keeps receiving every tick.

## Proposed design

- **Predicate:** `QWidget::isVisible()` on the dock. It is false for a dock on an inactive tab (`QStackedWidget`
  hides non-current pages), a maximize-hidden sibling and a minimized window. Keying on the active tab alone misses
  the maximize case.
- **Placement:** the per-tick playhead fan-out skips `!isVisible()` docks; the scene docks' `samplesIngested` handlers
  check the same predicate (or both consult one host-owned "visible dock" query). Structural operations (layout save)
  keep enumerating every dock.
- **Never gate the structural seeds.** `currentTimeChanged` fires only on a time change, so the restore, undo/redo and
  per-create seeds are the only seed a freshly restored or created dock gets; gating them leaves it blank.
- **Catch-up on show (paired with the gate).** A gated widget freezes at its last time. On becoming visible it pulls
  `PlaybackEngine::currentTime()` once (N skipped ticks collapse to one; every refresh primitive is time-addressable,
  including the `SceneEntitiesLayer` accumulator). Hooks: the existing `currentTabChanged` re-seed; a `DockWidget`
  `showEvent` re-seed for maximize/restore (no override exists); for scenes, route through `onTrackerTime` so the last
  tracker time updates, re-drive the live edge while following a stream, and for Scene3D call
  `invalidateTrackerRenderKey()` after an out-of-gate live-edge push so the render key matches the pixels.
- **Floating docks.** Plot docks clear `DockWidgetFloatable`, and `forEachDocker` walks only each `PlotDocker`'s own
  dock areas. If floating is ever enabled, the enumeration must also visit floating containers, and `isVisible()`
  (true for an on-screen floating window) drives the gate.

## Corner cases

| Scenario | Risk if the gate is added naively | Safeguard |
|---|---|---|
| Tab switch, especially paused | new tab shows a stale tracker or scene | re-push current time on tab change |
| Maximize, then restore | restored siblings are stale | `isVisible()` gate plus `showEvent` re-seed |
| Layout restore (including `--layout` before the first show) | hidden-tab docks render blank if the seed is gated | exempt structural seeds; re-seed on first show |
| Streaming into a hidden tab | stale on show once the streaming seam is gated | live-edge catch-up on show |
| Repeated scrubbing while hidden | none if the catch-up exists | pull `currentTime()` once on show, no tick replay |

## Test plan

- Plot: a maximize-hidden plot gets no `replot()` per tick; on restore its tracker matches `currentTime()` (assert the
  tracker position, not pixels, so it runs offscreen).
- Scene: a hidden dock's `onTrackerTime` / `setTimestamp` is not called per tick; on show the last tracker time equals
  the current time and one repaint is requested.
- Streaming: a hidden scene dock's live-edge drive is suppressed; on show it reaches the live edge in one call.
- Restore: a multi-tab restore leaves hidden tabs correct on first reveal.
