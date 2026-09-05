# Undo/Redo State Integrity — consolidated delivery plan

Status: **delivered** — all five PRs merged (#373 identity, #377 time/viewport,
#379 transactional undo, #380 processor transactionality, #382 scene docks).
This document merges two prior solution attempts into one delivery plan aiming
at completeness, correctness, and simplicity; it remains the reference for the
shipped design. Post-merge follow-ups are ranked in the PR #382 description;
the rank-1 item has its own draft: `was_pending_followup_plan.md`.

## 1. Context & goals

### The bug

With two datasets loaded that share topic names (the compare-two-runs
workflow), undo/redo silently rebinds curves plotted from the second dataset
onto the first. Root cause: a workspace snapshot identifies each curve by a
dataset-agnostic `(topic, field)` pair (`SeriesPath`,
`pj_app/src/LayoutXml.h`), and every restore re-resolves that pair against the
loaded datasets taking the **first match in load order**
(`resolveSeriesPath`, `pj_app/src/PendingDisplayBinder.cpp`). The curve then
renders the other dataset's samples at the other dataset's display offset — a
jump in time that presents as "timeline alignment broke after undo".

### The two prior branches

- **Branch A** (single commit, green; never published — kept only as the
  description below): minimal
  fix — a dataset-label hint on each saved curve, preferred at resolve time
  with a first-match fallback. Correct for the reported case, but the hint is
  the *deduplicated catalog display label* ("name (2)"), which is
  load-order-dependent for same-basename files. **Never merges**; its
  mechanism is superseded by PR 1 below, and its regression test
  (`multi_dataset_rebind_test.cpp`) is carried forward.
- **Branch B** (~13k lines / 105 files; never published — kept only as the
  description below):
  a broad state-integrity rework with largely correct designs and strong test
  suites, but delivered uncompilable (two missing includes) with one of its
  own new tests red. It is treated as a **design donor**: nothing is imported
  on trust; every line is re-verified in the slice that adopts it. Neither
  branch was pushed, so there is no ref to extract from: treat both entries as a
  design record, and re-derive any code they describe.

### Goals

1. **Completeness** — every guarantee pinned by branch B's tests survives in
   the end state (see §8 for the mapping).
2. **Correctness** — each slice builds green with its tests before review;
   donor code is design input, not trusted code.
3. **Simplicity** — where a simpler mechanism is *equally robust*, it wins.
   Each proposed simplification was adversarially gap-checked against branch
   B's code and tests; the design below is post-gap-check (several
   simplifications were scoped back where the check found real regressions).

## 2. Design principles

- **Session state avoids serialization surfaces.** Undo snapshots live in
  memory; only layout files and the clipboard cross a trust boundary, and only
  they need schemas, validation, and migration.
- **Identity: exact in-session, portable across sessions.** Serialized curve
  and layer elements carry `dataset_id` + `dataset_source` (and a full source
  path stamped at layout-file save). Undo-stack entries may *trust* the raw id
  because history resets whenever a dataset identity vanishes or is reminted —
  but elements still carry the source so clipboard paste (which outlives
  history epochs) verifies source agreement instead of trusting a recycled id.
  Unqualified (portable/legacy) paths resolve **reject-ambiguity**: bind only
  when the topic+field is unique across loaded datasets, otherwise prompt.
  Never guess by load order.
- **Validate at trust boundaries.** Layout-file load and clipboard paste
  reject unknown *elements* and malformed *values*, but tolerate unknown
  *attributes* (forward compatibility — a deliberate inversion of branch B's
  strict-leaf rule). The parse-into-locals-before-mutate pattern is kept
  everywhere regardless: it is what keeps the destructive-restore-then-rollback
  path rare.
- **Undo atomicity comes from the transaction**, not from validation:
  capture-before + rollback (`MissingCurvePolicy::kExact`) in every restore
  mode.
- **One owner per piece of state.** Scene pending intents are owned by the
  scene dock's queue; the display binder only tracks topic-demand references.

## 3. Target architecture

Each mechanism is tagged **[from B]** (adopted as designed), **[simplified]**
(simpler equally-robust form), or **[scoped]** (B's mechanism kept for a
subset of paths where the simplification was shown not to hold).

### Dataset identity — [from B, simplified]

`SessionManager` owns a source-path registry (`datasetSourcePath` /
`setDatasetSourcePath`, populated by `FileLoader`);
`CatalogModel::resolveDatasetIdentity(id, source, path)` resolves: exact id
(trusted only while the live source name agrees) → unique full-path match →
unique source-name match → reject with an explicit ambiguity flag.
Simplification retained: layout files strip bare numeric ids
(`removeUnvalidatedDatasetIds`) — a session-local id must never be accepted in
a later session. **Not** simplified away: the full in-session resolver.
Fan-out source replacement remints DatasetIds mid-session and restores its
pre-remint workspace snapshot via path stamping
(`sourceReplacementAboutToCommit`); raw-id-only snapshots cannot survive that
remint.

### Time model — [from B]

Total display offset = per-source alignment (`sourceDisplayOffset`) + global
time reference (`globalTimeReference`), kept strictly separate. Layout schema
v4: `<fileInfo>` gains per-dataset children carrying `source_name`,
**`source_index`** (fan-out members with identical names must not swap
offsets), `display_offset_ns`, `timeline_order`; a read-side migration flag
(`display_offset_includes_global_reference`) reinterprets v3 offsets without
changing their placement. Much of the runtime split already exists on main —
the remaining work is the schema, the migration, and
`notifyGlobalTimeReferenceIfChanged` (emit the frame-change signal only when
the numeric origin actually moved).

### Plot viewport — [simplified + guard]

Saved X ranges: a single rounded `qint64` nanoseconds per edge plus an
explicit `x_basis="absolute"|"value"` marker. No sub-nanosecond split (1 ns
granularity is permanently sub-pixel). Two mandatory companions, without which
this simplification is *less* robust than branch B: clamp `PlotMagnifier`'s
minimum x-width to ≥ 2 ns, and restore a degenerate saved range
(`left_ns == right_ns`) as ±0.5 ns instead of failing — otherwise a snapshot
taken while zoomed below 1 ns wedges both undo and redo permanently.

### Undo/redo core — [from B + simplified]

Transactional `onUndo`/`onRedo`: capture live state before navigating, restore
with `MissingCurvePolicy::kExact`, roll the whole workspace back on any
failure; a failed undo leaves both stacks usable.
`captureHistoryDataUniverse` records the exact raw-data identities backing the
history; a load that removes/remints any of them resets history, a purely
additive load hydrates the current tip instead.

Timeline state (per-dataset `display_offset_ns` + `timeline_order` **and**
view chrome: zoom, scroll, snap, name-column width) rides in an in-memory
struct, not XML:

```cpp
struct CapturedWorkspace {
  QByteArray xml;           // xmlSaveState() output
  TimelineState timeline;   // offsets + order + view chrome
};
```

`CapturedWorkspace` is the snapshot unit for **all four carriers**: the
undo/redo stacks, the restore-rollback capture, the progressive-restore
rollback (timeline half filtered to surviving datasets), and the
source-replacement snapshot. Dedup, coalescing, tip-hydration, and redo
capture all compare/assign the *pair* — a timeline-only edit changes no XML
bytes, so comparing bytes alone would silently drop it. The struct-apply path
ports branch B's pre-mutation guards (live-id check, offset-overflow fit).

### Pending curves — [simplified, partially]

A curve whose topic has not materialized is an ordinary `<curve>` element with
an unresolved key plus one keep-alive attribute ("real workspace intent — do
not prompt/strip at drain"), not a parallel `<pending_curve>` element type.
The two completion-policy booleans (`preserve_viewport_on_completion`,
`zoom_on_completion`) are **kept** as binder runtime state: a layout-restored
plot whose only curve materializes late must keep its finalized viewport,
while a fresh interactive plot auto-fits. Branch B's red test
(`PlaceholderCompletionPreservesPopulatedPlotZoom`) sits exactly here and must
be root-caused, not deleted — its failure is not evidence the behavior is
dispensable.

### PlotDocker — [from B]

Placeholder docks serialize as `<placeholder>` (undo restores the chooser, not
an empty dock); the layout tree is validated before any node is applied
(splitter sizes finite, counts consistent, malformed branch rejects the whole
splitter); splitter moves emit `undoableChange` outside restores.

### Scene docks — [from B + simplified]

Scene docks join the undo stack via a `workspaceChanged` signal (suppressed
during restores). Pending layer intents have **one owner** — the dock's own
deferred queue; the binder only holds topic-demand references keyed off queue
completion. Branch B's `authoritative_scene_queue` arbitration flag is dropped
(near-dead code once ownership is single); the binder must never regrow a
direct `addTopic` fallback path. Layer/camera XML validation follows §2:
strict on elements and values, tolerant of unknown attributes.

Note for PR 5 scoping: scene docks are NOT greenfield here — a pre-existing
resolver (`SceneDockWidget::resolveDatasetId`, `pj_scene_common`) already reads
the same `dataset_id`/`dataset_source` attributes and falls back to the **first
source-name match in iteration order** (the bug class PR 1 fixed for plots).
PR 5 replaces/reconciles it with `SessionManager::resolveDatasetIdentity` —
whose object-topic variant (`resolveObjectDatasetIdentity`) ships in PR 1
tested but caller-less, awaiting exactly this.

### Data processors — [scoped]

Recipes capture each input as a `TransformInputBinding`
(`dataset_source`, `topic_name`, `field_path`, `column_index`, plus the
session id as a hint) at install time — identity that survives column
reordering and dataset reloads. Two distinct consumption paths, deliberately
different:

- **Workspace restores (undo/layout)**: the existing clear-all-and-recreate
  reconcile, with a `bool` result so a failed processor rebuild fails the
  whole restore transaction. Curves re-resolve afterwards via
  `rebindCurveKeys`, so freshly minted output TopicIds are harmless here.
- **Data reloads (RefillGuard path)**: branch B's in-place machinery is kept —
  `rebindAndRecomputeForReplacedSources` + DerivedEngine
  `inputBindingState` / `resolvedInputBindingState` /
  `restoreInputBindingState`. Catalog curve keys embed TopicIds, so
  recreate-on-reload would mint new output ids and silently delete every
  plotted derived curve (pinned by
  `ProcessorOutputsReplayBeforePruneAndKeepStableTopicIds`).

Also kept: `recomputeBatch` (multi-root topological recompute),
`replaceSisoTransform`/`replaceMimoTransform` rollback, and TopicId-exact
dependency cascades (same-named topics in different datasets stay isolated).

### FileLoader — [from B]

Fan-out reloads retire superseded datasets atomically at commit; partial
fan-out failures erase only the failed entries; the
`sourceReplacementAboutToCommit` signal lets the shell capture a path-stamped
workspace snapshot before ids are reminted; source-path identity lives in
SessionManager, not in a FileLoader-private map.

## 4. Deliberately not adopted from branch B

| B mechanism | Why the simpler form is equally robust |
|---|---|
| `<workspace_timeline>` XML block + validation + strip-on-save | Session-only state; the in-memory `CapturedWorkspace` pair has no serialization surface to corrupt |
| `<pending_curve>` element type + identity/dedup machinery | An unresolved `<curve>` + keep-alive flag reuses the existing staging path |
| Sub-nanosecond viewport split (`*_subns`) | Single ns + magnifier ≥2 ns clamp + degenerate-range tolerance covers every reachable zoom |
| Unknown-attribute rejection (`isStrictLeafPayload` strict mode) | Actively harms forward compatibility; element/value strictness is retained |
| Binder/dock dual ownership of scene intents + `authoritative_scene_queue` | Single ownership makes the arbitration flag structural |
| Bare-id trust outside the undo stack | Ids are stripped at file save; source/path attributes carry portable identity |

Fallback rule: if a test ported from branch B fails under a simplification,
escalate to B's mechanism *for that slice* and record the decision as a short
ADR in this document.

## 5. Delivery: five PRs

| # | PR | ~Size | Depends on |
|---|---|---|---|
| 1 | Dataset identity foundation | 1.5k | — |
| 2 | Time model & viewport | 0.9k | 1 |
| 3 | Undo/redo integrity | 2.7k | 1, 2 |
| 4 | Processor & engine transactionality | 1.4k | 1 (parallel with 2–3) |
| 5 | Scene dock persistence & undo | 3.5k | 1, 3 |

The donor branches were never published, so there is nothing to `git checkout`
from. Each theme re-derives its slice from the design notes above; the four hot
files (`MainWindow.cpp/.h`, `PlotWidget.cpp`, `LayoutXml.*`, `SessionManager.*`)
are worked per theme rather than moved wholesale.

### PR 1 — Dataset identity foundation

Source-path registry + `resolveDatasetIdentity`; qualified `SeriesPath`
(`dataset_id` + `dataset_source` attributes on saved curves; path stamping at
layout save via `stampDatasetSourcePaths`; bare-id strip via
`removeUnvalidatedDatasetIds`; relative-path resolution at load); strict
reject-ambiguity resolver; clipboard-paste source-agreement check. Fixes the
reported bug. Donor map: `pj_runtime/{SessionManager,CatalogModel}.*`,
`pj_app/src/{LayoutXml,PendingDisplayBinder}.*`, identity hunks of
`pj_plotting/widget/src/PlotWidget.cpp` (incl. the missing
`<QScopedValueRollback>` include), `pj_app/src/FileLoader.*` registry hunks.
Tests: donor `session_manager_test` identity cases, donor `layout_xml_test` /
`pending_display_binder_test` identity additions, branch A's
`multi_dataset_rebind_test.cpp` retargeted to the new attributes.
Acceptance: two same-topic datasets; curve plotted from dataset 2 survives
snapshot→rebind on its own dataset; unqualified ambiguous paths prompt instead
of guessing.

### PR 2 — Time model & viewport

Schema v4 `<fileInfo>` per-dataset children (incl. `source_index`) + v3
read-side migration; viewport single-ns attributes + `x_basis` +
`normalizePlotRangeBasis`; `PlotMagnifier` ≥2 ns clamp + degenerate-range
tolerance; `notifyGlobalTimeReferenceIfChanged`. Donor map: `LayoutXml.*`
schema hunks, `PlotWidget.cpp` range save/load hunks, `PlotMagnifier.cpp`,
`SessionManager` notify seam. Tests: donor `plot_widget_range_offset_test`
rewrite (retargeted to single-ns), fan-out fileInfo round-trip halves of
`main_window_source_layout_test`, `time_test` additions.

### PR 3 — Undo/redo integrity

Transactional undo (`kExact`, capture-before, rollback);
`captureHistoryDataUniverse` reset/hydrate — the review checklist must
enumerate **every** reset/hydrate call site from the donor (~14 in
`MainWindow.cpp`; the raw-id-in-undo invariant is only as strong as the least
ported site, with the kExact live-id check as backstop); `CapturedWorkspace`
across all four carriers; timeline+chrome struct with pre-mutation guards;
pending-curve keep-alive flag + completion-policy booleans (root-cause the
donor's red viewport test here); PlotDocker validation/placeholder/splitter;
the `bool`-returning `restoreDataProcessors` reconcile primitive (without it,
kExact undo silently commits partial processor graphs); the missing
`pj_runtime/TopicDemandTracker.h` include. Interim limitation, documented in
the PR: scene docks join the transaction only in PR 5; PR 3's scene hooks are
written against guard-checked APIs so PR 5 slots in without touching the
transaction skeleton. Tests: donor `main_window_history_test` (8 cases),
`dock_widget_placeholder_test` additions, `plot_state_transaction_test`.

Kickoff notes accumulated while delivering PRs 1–2:

- **Real-MainWindow test binaries are cheap now**: PR 2 landed the
  `pj_app_shell` OBJECT library, the `add_pj_app_gui_test()` CMake helper, the
  friend-peer pattern (`MainWindowSourceLayoutTestPeer`), and
  `pj_app/tests/dataset_test_helpers.h` (own-TimeDomain + explicit-timestamp
  variants). Port `main_window_history_test` onto these; still one MainWindow
  instantiation per binary.
- **Progressive-restore interplay (§9) has concrete seams**:
  `pending_timeline_sources_` + `applyPendingTimelineState()` run at drain
  BEFORE the saved-viewport re-apply. The `CapturedWorkspace` carriers must
  compose with — not double-apply against — that ordering.
- **Warm-pin origin invariant** (documented on
  `SessionManager::global_min_cache_`): any new path that can RAISE a
  dataset's earliest sample must invalidate that dataset's pin AND the global
  memo. The timeline/undo work touches offsets everywhere — respect it.
- **`x_basis` attribute strings** are duplicated between `PlotWidget.cpp` and
  `normalizePlotRangeBasis` (tests pin the contract). Give them a shared home
  when this PR settles the viewport-basis question — not before.

### PR 4 — Processor & engine transactionality

`TransformInputBinding` capture; reload-path in-place rebind
(`rebindAndRecomputeForReplacedSources` + engine binding-state primitives);
`recomputeBatch`; replace-transform rollback; TopicId-exact cascades;
`RefillGuard::recomputeProcessors` ordering (replay before prune). Donor map:
`pj_datastore/derived_engine.*`, `pj_runtime/DataProcessorService.*`,
`SessionManager` RefillGuard hunks. Tests: donor
`data_processor_service_transform_test`, `derived_engine_test` changes,
RefillGuard cases of `session_manager_test`. Runtime-layer work — expect
rebase friction (not correctness conflicts) with PR 3 in `MainWindow.cpp`.

### PR 5 — Scene dock persistence & undo

`pj_scene_common` `workspaceChanged` + dock-owned deferred intents +
restore-failure reporting; scene3D persistence (camera/controls validation,
config-topic TF, layer order) and scene2D persistence (view state, layer-kind
tracking, transactional rollback), both with §2 validation semantics; scene
failures join the kExact transaction. Donor map: `pj_scene_common/*`,
`pj_scene3D/widgets/*` (+ `layer_xml_validation.h`, attribute-strictness
relaxed), `pj_scene2D/widgets/*`. Tests: donor `scene_common_test`,
`scene3d_dock_persistence_test`, `scene3d_layer_xml_validation_test`
(attribute cases inverted), `scene2d_dock_widget_test` additions.

Kickoff note from PR 2's review: extract `MainWindow`'s fan-out timeline
matcher (`applyTimelineStateFromLayout`'s inline policy, pinned today by
GUI-binary tests) into a testable free function alongside the scene-resolver
reconciliation — both are "resolve saved reference → live dataset" policies
and should share a shape and a lightweight test pattern.

## 6. Migration & compatibility

- **v3 → v4 offsets**: v3 wrote `displayOffset()` (global reference baked in);
  v4 writes `sourceDisplayOffset()` only. The loader subtracts its current
  global reference when reading v3 (read-only migration, placement preserved).
- **Unqualified legacy layouts** on multi-dataset sessions: reject-ambiguity +
  missing-curve prompt. This is an intentional behavior change from silent
  load-order guessing.
- **No branch-A compatibility** is needed anywhere: branch A never merged, so
  no layout in the wild carries its `dataset` label attribute.
- **Older builds** doing load→edit→save will strip attributes they don't know
  (accepted cost of attribute tolerance; unknown *elements* still reject).

## 7. Execution mechanics

- Neither donor branch survives as a ref; this document is the only record.
- Each PR: fresh worktree off `origin/main` via `./worktree-new.sh` once its
  dependencies merge. Every slice must build green and pass its tests before
  review — donor code is a design source, not trusted code.

## 8. Verification strategy

Per PR: module tests + full `ctest` (baseline 255 tests; the pre-existing
`extension_manager_test` failure is environmental and documented). Branch B's
test-backed guarantees, mapped to their carrying PR:

| Guarantee (donor test) | PR |
|---|---|
| Identity reject-ambiguity suite (`session_manager_test`, binder tests) | 1 |
| Clipboard identity safety (copy → reload → paste) | 1 |
| Fan-out fileInfo offsets/order round-trip (`main_window_source_layout_test`) | 2 |
| Sub-pixel viewport restorability after deep zoom (retargeted) | 2 |
| Transactional rollback incl. late PlotDocker failure (`main_window_history_test`) | 3 |
| Global rebase preserves absolute playhead | 3 |
| Additive-load history retention / remint rebaseline / progressive-drain-owned history | 3 |
| Timeline offsets **and chrome** undo round-trip | 3 |
| Late-materialization viewport policies (incl. donor's red test, root-caused) | 3 |
| Stable derived TopicIds on reload; replay-before-prune (`session_manager_test`) | 4 |
| Replace-transform rollback; cross-dataset processor isolation | 4 |
| Scene transactional restore, deferred-intent survival, validation | 5 |

Live checks: two same-topic MCAPs — plot from dataset 2 → edit → undo → curve
stays on dataset 2 (PR 1); copy plot → reload file → paste → binds correctly
or prompts (PR 1); timeline drag → undo → offsets and chrome restore (PR 3).

## 9. Risks & open questions

- **Viewport save basis (decided at PR 2, revisit in PR 3):** the saved absolute
  X range still converts through `displayOffsetSeconds()` (first datastore
  curve's total offset), so a multi-dataset plot's saved range depends on curve
  order — pre-existing behavior, kept to hold PR 2's scope. PR 3's snapshot
  rework decides whether to move the basis to `globalTimeReference()` (the
  donor's design, with its `legacy_source_*` read migration) or keep it.
- **Timeline offset-migration overflow guards** (`checkedTimelineDifference` et
  al.) are deferred to PR 3 with the rest of the pre-mutation-guard theme; the
  v3 `offset − globalTimeReference()` subtraction has int64 headroom at real
  epoch magnitudes.

- v3 offset-migration math against layouts saved under a *different* global
  reference than the loading session's.
- Progressive-load interplay with transactional undo
  (`progressive_layout_in_flight_` gating) — port carefully, it owns the
  history transaction during drains.
- The donor's red viewport test is unexplained until PR 3; its root cause may
  reveal a genuine design question in the late-materialization policies.
- The ~14 distributed reset/hydrate call sites are the soft underbelly of the
  raw-id-in-undo invariant; the kExact live-id check on apply is the backstop.
