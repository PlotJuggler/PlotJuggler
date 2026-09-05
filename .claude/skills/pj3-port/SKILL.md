---
name: pj3-port
description: Use when porting any widget, dialog, panel, transform, or helper from PlotJuggler 3 into PJ4, when asked to add a UI element or feature that PJ3 already has, or when tempted to write a new one from scratch.
---

# Porting from PlotJuggler 3

## Overview

PJ4's core strategy is the **wholesale lift** (PJ4_PLAN.md §5.3, §8): port PJ3 files largely intact, then rebind only the data reads. The recorded failure mode is rewriting from scratch, "improving" while porting, or renaming things users depend on. **Port, don't rewrite. Rebind, don't redesign.**

PJ3 lives at `~/ws_plotjuggler/PlotJuggler/` — **read-only, never modify it from PJ4.**

## Procedure

**1. Locate the PJ3 source first.** Before writing anything, search PJ3:

```bash
grep -ril "<feature keyword>" ~/ws_plotjuggler/PlotJuggler/plotjuggler_app/ \
    ~/ws_plotjuggler/PlotJuggler/plotjuggler_base/
```

- `plotjuggler_app/` — plot widgets, dockers, zoomers, tracker, drag-drop, dialogs, Lua UI
- `plotjuggler_base/` — shared base types (cross-check against `pj_base` before porting; it may already exist)
- `plotjuggler_plugins/` — already ported; reference only

If PJ3 has it → port it. If PJ3 genuinely doesn't → say so explicitly and get agreement before greenfielding.

**2. Worktree first.** `./worktree-new.sh <branch>` off `origin/main` before any edit (see the `worktree` skill).

**3. Decide placement** with root CLAUDE.md placement rules. Quick map: plot-family code → `pj_plotting`; reusable-by-any-Qt-app control → `pj_widgets`; app dialog / wiring → `pj_app`; service/state logic → `pj_runtime`. When the fit is ambiguous, ask — don't invent a location.

**4. Lift the files.** Copy `.h/.cpp/.ui` across, then apply *only* these transformations:

| Transform | Do | Don't |
|---|---|---|
| Style | `PascalCase.{h,cpp}`, `PJ::` namespace, `trailing_underscore_` members, clang-format | Restructure logic that already reads cleanly |
| License | Add `// SPDX-License-Identifier: MPL-2.0` header | Copy PJ3 header boilerplate verbatim |
| `.ui` widget classes | Upgrade `QComboBox` → `PJ::ComboBox` (via `<customwidgets>`); checkbox rows → `QLabel` + `PJ::ToggleSwitch` | — |
| `.ui` `objectName`s | **Preserve byte-identical** (`buttonLoadDatafile`, `displayTime`, …) — layout files, QSS selectors, muscle memory depend on them | Rename anything, ever, without explicit approval |
| Comments | Keep PJ3's why-comments; delete narration | Add "ported from PJ3" / history comments |

**5. Rebind the data paths — the only systematic rewrite.**

| PJ3 | PJ4 |
|---|---|
| `PlotDataMapRef` reads | `DatastoreCurveAdapter` over `pj_datastore::DataReader` |
| `TransformsMap` | `TransformRegistry` (`pj_runtime`) |
| Direct main-window state | `pj_runtime` services: `CatalogModel`, `SessionManager`, `PlaybackEngine` |
| Widget↔widget time sync | `IDataWidget::onTrackerTime(double)` contract |

Keep PJ3's per-pixel behavior: **no adapter-side decimation** (Qwt's paint-time filtering owns that; pre-decimating breaks zoom).

**6. Preserve PJ3's performance guards.** If the PJ3 code has a gate (e.g. `use_opengl`), port the gate; each one exists because the ungated path is measurably slower.

**7. Tests.** Add a gtest binary in the owning module's `tests/` covering the rebound adapter/wiring (a factory-registration or wiring test protects against silent merge drops).

**8. Improvements: flag, don't apply.** Cleaner shape spotted while porting? List it with cost/benefit at the end and **ask**. Silently refactoring and silently skipping obvious wins are both violations.

## Done means

- [ ] PJ3 source read; diff vs PJ3 is style + rebinding only (anything else called out)
- [ ] `.ui` `objectName`s byte-identical (verify: `grep -o 'name="[^"]*"' old.ui new.ui | diff`)
- [ ] SPDX header on every new file; builds clean; module tests green
- [ ] Improvement list surfaced as questions, not commits

## Red flags — stop and re-read this skill

- "The PJ3 code is old/ugly, cleaner to rewrite" — no; greenfield needs explicit agreement
- "I'll modernize the names while I'm here" — objectNames are frozen
- "This helper is small, faster to just write it" — search PJ3 first; it almost always exists
- "I improved X along the way" — should have been a question, not a change
