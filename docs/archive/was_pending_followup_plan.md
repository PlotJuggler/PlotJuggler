# Follow-up: unified was-pending semantics for scene restore queues

> **Retired 2026-11-12: a draft that was never scheduled or implemented.** The code
> still distinguishes pending entries by origin (`pending_intent`); nothing below
> describes current behaviour.

Epilogue to the undo/redo state-integrity epic (`undo_redo_integrity_plan.md`;
PRs #373, #377, #379, #380, #382). Rank-1 item from the two post-merge design
retrospectives on #382. Status: DRAFT — not scheduled.

## 1. Problem

PR 5 distinguishes two kinds of deferred scene-queue entries by origin:
interactive advertised-topic drops carry `pending_intent="true"` (durable
workspace intent), while layout-origin pends serialize indistinguishably from
bound layers. Because a snapshot cannot tell "this element was pending when
captured" from "this element was bound when captured", layout-origin pends
must be treated as **blocking**: an exact restore refuses them, and the shell
must actively maintain the invariant that committed baselines never carry
them (the `#382` drain-prompt arm added by `6bec75fe` after review found that
a pend-carrying baseline wedged every later exact undo).

The taxonomy is the bug's parent, not its fix: the shell patrols an invariant
("baselines are blocking-free") that a better snapshot encoding would make
structural.

## 2. The change

Serialize each queue entry's **binding state**, not its origin: every element
written out of the deferred queue (`appendPendingRestoreElements`) is stamped
`was_pending="true"`. The restore rule then needs no taxonomy:

- Element **without** the marker: the snapshot believed it was bound. If it
  cannot bind now — under `kExact` the transaction FAILS (unchanged
  semantics); under `kPrompt` it joins the missing-elements prompt.
- Element **with** the marker: the snapshot knew it was pending. It re-enters
  the queue — exact by construction, never blocking, never prompted at
  undo/redo.

"A committed baseline can always be exactly restored" stops being an invariant
the shell maintains and becomes a property of the encoding.

Backward compatibility falls out of attribute tolerance (plan §2): old
layouts carry no marker, so every element reads as was-bound → strict — which
is exactly today's blocking semantics for layout-origin elements. No version
bump, no migration.

## 3. UX decision (must be made before implementation)

A layout FILE that references topics which never arrive: elements in the file
that were saved as was-pending will silently wait. Options:

- (a) Silent pend + diagnostic warning only. Simplest; drops the current
  prompt for this case entirely.
- (b) **Recommended**: keep the missing-elements prompt at file-load (kPrompt)
  only, keyed on the marker: prompt lists elements the layout believed were
  BOUND that cannot bind now; was-pending elements never prompt. Undo/redo
  (kExact) never prompts either way.

## 4. Touchpoints

- `pj_scene_common/scene_dock_widget.{h,cpp}`
  - `appendPendingRestoreElements`: stamp `was_pending="true"` on every
    serialized entry (one scene-side constant; also collapse the three raw
    `pending_intent` literal sites flagged in review).
  - Restore path: marker-bearing elements enqueue without joining any
    blocking set.
  - DELETE: `unresolvedBlockingPendingRestores`, `clearBlockingPendingRestores`
    (keep `unresolvedPendingRestores` for diagnostics).
- `pj_app/src/MainWindow.{h,cpp}`
  - `SceneRestoreVerdict` loses `blocking_topics` under kExact (keep a
    was-bound-missing list if UX option (b) is chosen, consumed only by the
    kPrompt arms).
  - DELETE the `6bec75fe` drain-prompt scene arm (replaced by the marker rule
    under option (b), or removed under (a)).
  - `settleSceneRestores` consumers simplify accordingly.
- Dock `xmlLoadState`/`restoreLayerElement`: a was-bound element that resolves
  to `kDeferred` must surface as a failure under kExact (today "blocking"
  carries that; the marker's absence carries it after).

## 5. Tests

- SURVIVES UNCHANGED: `InvalidSceneElementFailsExactUndoTransactionally`
  (both arms — the ghost layer carries no marker, so it is was-bound-missing
  and still fails the exact transaction).
- REWRITE: `scene_common_test` blocking/clear cases → marker semantics;
  the drain-prompt scene-pend behavior per the chosen UX.
- NEW PINS: (1) an exact undo to a snapshot containing a was-pending entry
  succeeds and re-queues it (the previously-wedging case, now green by
  construction); (2) old-layout compatibility — unmarked unresolvable element
  fails kExact / prompts at kPrompt; (3) round-trip: pend → snapshot → restore
  → still pending → topic arrives → completes.

## 6. Estimate & sequencing

1–2 days, dominated by test rewrites. Standalone PR; no dependency on the
other recorded follow-ups (epoch ownership, restore template method) but
lands cleanest BEFORE the template-method refactor so the transaction shape
is settled first.
