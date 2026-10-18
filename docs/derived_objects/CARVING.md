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
- **none:** builds on 0.35.0, the SDK the slices before 0.36.0 shipped with;
- **client:** the header-only 0.36.0 additions (field tables, metadata keys,
  `unprojectPixel`), released 2026-10-17 and pinned since;
- **contract:** the host contract of the next minor, after the #202 decisions.

| # | Slice | Depends on | SDK | Folded-in decisions and fixes | Status |
|---|---|---|---|---|---|
| 0 | These documents: requirements, architecture, scenarios, tracker | none | none | none | in review |
| T | Independent: do not store repeated identical TF samples (`TransformBuffer`, still in `pj_scene3D` on `release-4.1`) | none | none | ARCHITECTURE §12 R10. Equal means exactly equal, component by component (no tolerance). | in review |
| 1 | The engine renames, no behaviour change (no aliases) | 0 | none | none | in review |
| 1t | `pj_tf` extraction and the `TransformService` move to `pj_runtime`; TF R2 (fixed-frame memory to the scene side) and R3 (refuse a topic from another dataset, test-first) | 1 | none | none | in review |
| 1b | Two timestamps per TF sample (ARCHITECTURE §12 R6) | 1 | none | Test-first, with a fixture whose log time differs from its header stamps, and a simulated-time fixture. Visual check on the nuScenes cameras. Stamped 3D layers move to their own stamp, which replaces the scene's "frame-locked at the tracker time" rule for them. | in review |
| 2 | `EnginePool` plus the GUI-freeze benchmark. Baseline measured on the old engines. | 1 | none | none | in review |
| 3 | `BatchEngine` off the GUI thread: reload brake, modes, windows | 2 | none | D30 for batch: publish every valid run, keep the stale flag; markers keep updating while streaming. A failed scheduled run keeps the last set and reports its error. A layout load's create waits at most 8 ms for its first run (decided 2026-10-13, narrowed the same day); plugin creates and history restores run it to completion. Deferred: the batch byte budget (slice 5) and the window/coverage ABI (slice 8) | in review |
| M | Marker topics described by metadata (ARCHITECTURE §13): recognition and routing read metadata; names unchanged; a 0.35 direct-write name is stamped at registration | 3 | none | On-demand-only logic tests the `pj_derived` value. The metadata key names are host constants until the host-contract minor adds them to the SDK, with the typed `scope`. | in review |
| 4 | Incremental replay off the GUI thread, streaming semantics, the 50 ms poll fix | 2 | none | Carved: retention gaps for incremental nodes (`IncrementalEngine::gaps`, `gaps` in the transform config; also for a time-sliced node stopped mid-chunk, a failed multi-input node, and a pause longer than the window via `flushTo`). The "50 ms poll fix" is slice 3's marker stamp filter: the poll no longer passes every topic to the marker recompute as a recompute. Slice 4 keeps the all-topics advance of eager processors, with a measured idle cost (500 topics, 20 filters, 3 two-input transforms, nothing new): 6 µs per tick at the default 5 s buffer, 76 µs at 60 s, 2.7 ms at 10 min before the engine's idle-node gate, about 1.5 µs at every buffer after it (ARCHITECTURE §16, §19); a per-tick changed-topic feed was built, found to risk evicting rows before a node consumed them, and dropped. Not carved: the incremental engine stays on the GUI thread (it already replays in 8 ms slices; worst measured stall 15 ms), and object-topic tracking waits for its consumer in slice 6a. | in review |
| 5 | Luau object scripts in `pj_scripting`: binder, native operations, sandbox, memory budget, TF lookup | 2 | client | TF R1 (snapshot type in `pj_tf`, first consumer here); the Luau cancel check; lazy list access in the binder; output declarations limited to the binders' types; TF R4; TF R5 and Luau VM reuse only if their measurements say so; a way to clear an optional field through the builtin field-table descriptors, which the binder needs to unset one (deferred from the SDK 0.36.0 review, which found it impossible today) | planned |
| L | One life-cycle protocol for the three engines; layout persistence moves out of `MainWindow` | 3, 4, M | none | Unchanged generators keep their topics (test-first); a mixed-dataset layout restores the same way whatever the load order (GAP-48: the first saved element's dataset wins; carved as L3 (#718), independent of L1 and L2); runtime hosts assembled once (no "KEEP IN SYNC"); real layouts saved by a `release-4.1` build as restore fixtures; history restore keeps unchanged generators live, so an undo creates only the rules it changed. Three PRs. L1 (in review): fixtures written by a scripted writer on release-4.1 (`pj_app/tests/fixtures/layouts/release-4.1/`; a generator window and a mixed-dataset dock are not producible there), `MainWindowRelease41LayoutTest` (green before and after the move), and a pure move of the persistence into `pj_runtime`'s stateless `DerivedObjects` with today's order and create wait; `applyWorkspace` parses once. L2 (in review): the protocol behaviours. An unchanged generator keeps its topic, published set and run state (per id; it runs again when a series it reads is removed or created by the restore, or, for an all-datasets rule, when a contributor's offset moved since its last published run); a layout load creates a changed rule with the bounded create, while an undo runs a changed rule to completion (decided 2026-10-13, kept 2026-10-17); the catalog is rebuilt before the generators install; closing a toolbox ends its previews in both engines and keeps its persistent recipes (`clearTransformsForPlugin` deleted, GAP-36); `DerivedRuntimeHosts` is assembled once for the GUI and headless sessions (headless gains `kind=markers`); the snapshot carries its intent and one diagnostics channel. L3: GAP-48 | L1, L2 in review |
| S | Spike: pull versus ring (ARCHITECTURE §9) | none | client | Ended after step 0 (the publication cost, measured on the prototype) and a paper check of D26. Ring chosen (ARCHITECTURE §9), 2026-10-14. No throwaway branch was built. | done |
| 6a | `OnDemandService` core, carved already split: `RecipeGraph`, `InputStager`, `ResultHistory`, `NodeScheduler`, `GridRunner`, a transitional facade. Possibly 2-3 slices. | 5, L, S | client | D30 in `NodeScheduler`, test-first with a 90 ms Luau function during playback. Ring (ARCHITECTURE §9): results stay in the store, publication is never refused, one global byte budget, findings at a fixed early store timestamp. Step 0 requirements: byte estimates never serialize (size from the decoded object); a compute-cache miss checks the store first; the compute cache is sized by time span, not entry count; decoding in the worker includes a lazy input entry's chunk decode. Inline chains. An internal budget type instead of the ABI struct. The declared range. | planned |
| 6b | Following the playhead; on-demand layout persistence on the protocol from slice L | 6a, L | client | Restore fixtures include layouts saved by the prototype | planned |
| 7 | Scene layers on derived topics: readiness, the lag indicator, "why is it empty", framing | 6a | client | The D30 lag indicator on the layer row, with the number; a plot-level computing/lag indicator next to the markers (slice 3's status-bar line is the interim); the dock's time-range clamp and live-edge fold skip findings | planned |
| 8 | Plugin bridges: data processors v2, evaluations, catalog v2, plot-tab scene tabs, marker window | 6a, 7 | contract | `DataProcessorsKindRouter` checks `struct_size` first; the #202 decisions applied; each plot-tab slot filled on its own; a known but unservable kind rejected by name; host-contract wording of `create_data_processor`: failures detected within the first budget are returned, later failures are reported asynchronously; the contract (#202) documents the marker config keys `computing` (bool) and `error` (string, present only after a failed run); host contract: an opt-in flag on the v2 create for a non-blocking marker create; released v1 creates stay synchronous | blocked on the SDK tag |
| 9 | Media for on-demand: images, depth, video | 6a, 7 | client | Images and depth publish into the ring under the global byte budget | planned |
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
| ~~Split SDK #202~~: decided (split). The header-only 0.36.0 was released 2026-10-17 (SDK #204). | Slice 5 | Slices 5-7 and 9-11 need the field tables in a released SDK |
| ~~Start the pull-versus-ring spike~~: agreed 2026-10-13; it runs first | Slice 6a | 6a builds `ResultHistory` one way or the other |
| ~~Pull or ring~~: ring, decided 2026-10-14 (ARCHITECTURE §9) | Slice 6a | Step 0 showed the publication cost is negligible, and a fixed early store timestamp meets D26 |
| ~~Two timestamps per TF sample~~: decided 2026-10-13 (slice 1b) | Slice 5 | Slice 5 gives scripts TF lookups, and the default lookup clock is part of their contract |
| A chain in one worker task, under the two conditions in ARCHITECTURE §8 | Slice 6a design | Part of the `InputStager` and `NodeScheduler` shape |
| ~~SDK host-contract items~~: decided 2026-10-13 with the prototype's author (ARCHITECTURE §5, §18) | Applied before the host-contract tag | They become permanent at the tag |
| Python trust model | Slice 10 | It decides how far the Python sandbox goes |
| TF R5, Luau VM reuse | When their measurements are in (R10 first; the series slice) | Conditional by design |

## SDK

- **0.36.0 released 2026-10-17** (SDK #204, tag `v0.36.0`): the header-only part. PJ4 pins it.
- **#202** is open, and becomes the host-contract minor. The review comment on it lists twelve
  contract decisions to make before the tag. Its rebase onto 0.36.0 moves its `@since`, its
  feature-floor group and its CHANGELOG entries to that minor.
- **#203,** stacked on #202, holds the cleanups that do not change the contract.
- **#202 follow-up** (2026-10-11): eight items decided, five left to the author. The author
  agreed to all of them on 2026-10-13, replacing the `unprojectPixel` tolerance with the
  full inverse of K.
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
| 2026-10-14 | Pull versus ring decided: ring (N7). The spike ended after step 0 and a paper check of D26. Recorded in ARCHITECTURE §8, §9, §11, §13, §17, §18, §20 and in SCENARIOS. |
