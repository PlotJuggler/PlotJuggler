# Derived objects: carving tracker

**What this is.** A temporary tracker for delivering the derived-objects work to
`release-4.1` as a stack of small pull requests. It is deleted once the stack has landed.
The prototype (pull requests #697, #698 and #699, branch `feat/derived-objects-editor-host`)
stays open as the reference implementation and is not modified. Each slice is cut from its
final code, not replayed commit by commit: the prototype's history rewrites itself (for
example, a `pj.scene_views.v1` host was added and later replaced by plot-tab scene tabs).
Each slice folds in the review fixes for the code it introduces.

Read [ARCHITECTURE.md](ARCHITECTURE.md) for the target, and [SCENARIOS.md](SCENARIOS.md) for
the behaviours each slice must keep, change by decision, or defer.

## Rules for every slice

- It builds and passes the whole suite on its own. It adds no scaffolding that a later
  slice deletes.
- Every scenario it touches in SCENARIOS.md is either ported with its test, changed by a
  named decision (with a new test), or marked deferred.
- Behaviour changes follow strict TDD: capture the failing test first. Refactors say so.
- Performance claims come with a number (GUI time, paint rate at or below 60 Hz,
  benchmark).
- Before merge: /simplify, a Codex review, and a final Fable review.
- `release-4.1` gets no automatic CI, so verify with a local build plus a
  `workflow_dispatch` run of the CI workflows.
- Update ARCHITECTURE.md's section statuses and this tracker in the same pull request.

## Slices

The `SDK` column says what a slice needs from the SDK:
- **none:** builds on the released 0.35.0;
- **client:** the header-only 0.36.0 additions (field tables, metadata keys,
  `unprojectPixel`);
- **contract:** the 0.36.0 host contract, after the #202 decisions.

| # | Slice | Depends on | SDK | Folded-in decisions and fixes | Status |
|---|---|---|---|---|---|
| 0 | These documents: requirements, architecture, scenarios, tracker | none | none | none | in review |
| T | Independent: do not store repeated identical TF samples (`TransformBuffer`, still in `pj_scene3D` on `release-4.1`) | none | none | ARCHITECTURE §12 R10. Equal means exactly equal, component by component (no tolerance). | in review |
| 1 | The engine renames, no behaviour change (no aliases) | 0 | none | none | in review |
| 1t | `pj_tf` extraction and the `TransformService` move to `pj_runtime`; TF R2 (fixed-frame memory to the scene side) and R3 (refuse a topic from another dataset, test-first) | 1 | none | none | in review |
| 1b | Two timestamps per TF sample (ARCHITECTURE §12 R6) | 1 | none | Test-first, with a fixture whose log time differs from its header stamps, and a simulated-time fixture. Visual check on the nuScenes cameras. | planned |
| 2 | `EnginePool` plus the GUI-freeze benchmark. Baseline measured on the old engines. | 1 | none | none | in review |
| 3 | `BatchEngine` off the GUI thread: reload brake, modes, windows | 2 | none | D30 for batch: publish every valid run, keep the stale flag; markers keep updating while streaming | planned |
| M | Marker topics described by metadata (ARCHITECTURE §13): recognition and routing read metadata; names unchanged; a 0.35 direct-write name is stamped at registration | 3 | none | On-demand-only logic tests the `pj_derived` value. The metadata key names are host constants until the host-contract minor adds them to the SDK, with the typed `scope`. | planned |
| 4 | Incremental replay off the GUI thread, streaming semantics, the 50 ms poll fix | 2 | none | none | planned |
| 5 | Luau object scripts in `pj_scripting`: binder, native operations, sandbox, memory budget, TF lookup | 2 | client | TF R1 (snapshot type in `pj_tf`, first consumer here); the Luau cancel check; lazy list access in the binder; output declarations limited to the binders' types; TF R4; TF R5 and Luau VM reuse only if their measurements say so | planned |
| L | One life-cycle protocol for the three engines; layout persistence moves out of `MainWindow` | 3, 4, M | none | Unchanged generators keep their topics (test-first); a mixed-dataset layout restores the same way whatever the load order (GAP-48); runtime hosts assembled once (no "KEEP IN SYNC"); real layouts saved by a `release-4.1` build as restore fixtures | planned |
| S | Spike: pull versus ring (ARCHITECTURE §9). A throwaway branch off the prototype. The first build-heavy work. | none | client | Step 0: measure the publication cost. One Scene2D source besides the 3D layers; list every reader of the output types. Pass and kill criteria in ARCHITECTURE §9. | next |
| 6a | `OnDemandService` core, carved already split: `RecipeGraph`, `InputStager`, `ResultHistory`, `NodeScheduler`, `GridRunner`, a transitional facade. Possibly 2-3 slices. | 5, L, S | client | D30 in `NodeScheduler`, test-first with a 90 ms Luau function during playback. Pull or ring per the spike. One global byte budget. Decoding in the worker. Inline chains. An internal budget type instead of the ABI struct. The declared range. | planned |
| 6b | Following the playhead; on-demand layout persistence on the protocol from slice L | 6a, L | client | Restore fixtures include layouts saved by the prototype | planned |
| 7 | Scene layers on derived topics: readiness, the lag indicator, "why is it empty", framing | 6a | client | The D30 lag indicator on the layer row, with the number; `frameAt` branches if pull | planned |
| 8 | Plugin bridges: data processors v2, evaluations, catalog v2, plot-tab scene tabs, marker window | 6a, 7 | contract | `DataProcessorsKindRouter` checks `struct_size` first; the #202 decisions applied; each plot-tab slot filled on its own; a known but unservable kind rejected by name | blocked on the SDK tag |
| 9 | Media for on-demand: images, depth, video | 6a, 7 | client | Image and depth derived branches (pull); no materialization window | planned |
| 10 | The Python lane | 5, 6a | client | Python N1: compile once per revision, read-only modules (test-first: `math.pi = 3` does not leak); native operations release the GIL | planned |
| 11 | Series: on-demand, demand-gated, unified with the window loop, union clock | 6a | client | D10 as revised; decoding in the worker; entries batched; the visible range first; "not computed" and "computing N%"; assistant queries count as demand | planned |
| 12 | Custom Topics panel (from #698) | 6a, L | client | none | planned |
| 13 | Transform Editor host support (from #699): output inference, embedded scene views, previews owned by the host | 8, 11 | contract | `INFER_OUTPUTS` folded; one `scene_view` key; attach failures reported to the plugin | planned |
| P | Spike: Python in the browser (CPython 3.14 for Emscripten) | 10 | client | ARCHITECTURE §14 | proposed |

## Open decisions, by the slice they block

Slices 0-4 and L need no new SDK and no open decision, so carving can start once slice 0
is merged.

| Decision | Needed before | Why |
|---|---|---|
| Approve slice 0 (these documents) | Carving starts | Reviewers judge each slice against the written target |
| ~~Split SDK #202~~: decided (split). Releasing the header-only 0.36.0 is now the task. | Slice 5 | Slices 5-7 and 9-11 need the field tables in a released SDK |
| ~~Start the pull-versus-ring spike~~: agreed 2026-10-13; it runs first | Slice 6a | 6a builds `ResultHistory` one way or the other |
| ~~Two timestamps per TF sample~~: decided 2026-10-13 (slice 1b) | Slice 5 | Slice 5 gives scripts TF lookups, and the default lookup clock is part of their contract |
| A chain in one worker task, under the two conditions in ARCHITECTURE §8 | Slice 6a design | Part of the `InputStager` and `NodeScheduler` shape |
| ~~SDK host-contract items~~: decided 2026-10-13 with the prototype's author (ARCHITECTURE §5, §18) | Applied before the host-contract tag | They become permanent at the tag |
| Python trust model | Slice 10 | It decides how far the Python sandbox goes |
| TF R5, Luau VM reuse | When their measurements are in (R10 first; the series slice) | Conditional by design |

## SDK

- **#202 (0.36.0)** is open. The review comment on it lists twelve contract decisions to make
  before the tag.
- **#203,** stacked on #202, holds the cleanups that do not change the contract.
- **#202 follow-up** (2026-10-11): eight items decided, five left to the author. The author
  agreed to all of them on 2026-10-13, replacing the `unprojectPixel` tolerance with the
  full inverse of K.
- **Before the header-only 0.36.0 tag:** `unprojectPixel` with the inverse of K, `pj_derived`
  as an open value set, and a build of the header-only part without the host-contract
  declarations.
- **Decided 2026-10-10: split.**
  - 0.36.0 is released with only the header-only additions: field tables,
    `PinholeIntrinsics` and `unprojectPixel`, the ImageAnnotations wire additions, metadata
    keys, and their #203 cleanups. It unblocks slices 5-7 and 9-11.
  - #202's host contract stays open as the next minor release.
  - It folds in the cheap decided items now, settles the rest with evidence from carving,
    and is tagged just before slice 8.
  - Nothing can use the host contract before slice 8 anyway.

## Log

| Date | Event |
|---|---|
| 2026-10-10 | Strategy decided. Slices target `release-4.1` and are carved jointly. Decisions recorded in REQUIREMENTS.md and ARCHITECTURE.md. |
| 2026-10-11 | Slice 0 opened as #704. Follow-up with the decided SDK items posted on SDK #202. |
| 2026-10-13 | The prototype's author agreed to every proposal. Folded in: the `unprojectPixel` inverse, `hasSceneTabs()` removed, the five host-contract items, marker metadata (slice M, names kept), one global byte budget, TF R6, the spike first, the D7 wording, and the series, lag, isolation and layout-fixture details. |
