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
- Code written by a subagent, then /simplify, one Codex review and one Opus review; each
  finding weighed by the complexity of its fix, the probability of its case and its
  gravity.
- CI on a pull request into `release-*` runs Linux, Windows and a WebAssembly build;
  macOS and Pixi are dispatched by hand on merge-ready heads (repo variable
  `PJ_CI_RELEASE_PR_LITE`).
- Update ARCHITECTURE.md's section statuses and this tracker in the same pull request.

## Slices

The `SDK` column says what a slice needs from the SDK:
- **none:** builds on 0.35.0, the SDK the slices before 0.36.0 shipped with;
- **client:** the header-only 0.36.0 additions (field tables, metadata keys,
  `unprojectPixel`), released 2026-10-17 and pinned since;
- **contract:** the host contract of the next minor, after the #202 decisions.

| # | Slice | Depends on | SDK | Folded-in decisions and fixes | Status |
|---|---|---|---|---|---|
| 0 | These documents: requirements, architecture, scenarios, tracker | none | none | none | merged (#704) |
| T | Independent: do not store repeated identical TF samples (`TransformBuffer`, still in `pj_scene3D` on `release-4.1`) | none | none | ARCHITECTURE §12 R10. Equal means exactly equal, component by component (no tolerance). | merged (#705) |
| 1 | The engine renames, no behaviour change (no aliases) | 0 | none | none | merged (#706) |
| 1t | `pj_tf` extraction and the `TransformService` move to `pj_runtime`; TF R2 (fixed-frame memory to the scene side) and R3 (refuse a topic from another dataset, test-first) | 1 | none | none | merged (#707) |
| 1b | Two timestamps per TF sample (ARCHITECTURE §12 R6) | 1 | none | Test-first, with a fixture whose log time differs from its header stamps, and a simulated-time fixture. Visual check on the nuScenes cameras. Stamped 3D layers move to their own stamp, which replaces the scene's "frame-locked at the tracker time" rule for them. | merged (#712) |
| 2 | `EnginePool` plus the GUI-freeze benchmark. Baseline measured on the old engines. | 1 | none | none | merged (#708, #714) |
| 3 | `BatchEngine` off the GUI thread: reload brake, modes, windows | 2 | none | D30 for batch: publish every valid run, keep the stale flag; markers keep updating while streaming. A failed scheduled run keeps the last set and reports its error. A layout load's create waits at most 8 ms for its first run (decided 2026-10-13, narrowed the same day); plugin creates and history restores run it to completion. Deferred: the batch byte budget (slice 5) and the window/coverage ABI (slice 8) | merged (#711) |
| M | Marker topics described by metadata (ARCHITECTURE §13): recognition and routing read metadata; names unchanged; a 0.35 direct-write name is stamped at registration | 3 | none | On-demand-only logic tests the `pj_derived` value. The metadata key names are host constants until the host-contract minor adds them to the SDK, with the typed `scope`. | merged (#715) |
| 4 | Incremental replay off the GUI thread, streaming semantics, the 50 ms poll fix | 2 | none | Carved: retention gaps for incremental nodes (`IncrementalEngine::gaps`, `gaps` in the transform config; also for a time-sliced node stopped mid-chunk, a failed multi-input node, and a pause longer than the window via `flushTo`). The "50 ms poll fix" is slice 3's marker stamp filter: the poll no longer passes every topic to the marker recompute as a recompute. Slice 4 keeps the all-topics advance of eager processors, with a measured idle cost (500 topics, 20 filters, 3 two-input transforms, nothing new): 6 µs per tick at the default 5 s buffer, 76 µs at 60 s, 2.7 ms at 10 min before the engine's idle-node gate, about 1.5 µs at every buffer after it (ARCHITECTURE §16, §19); a per-tick changed-topic feed was built, found to risk evicting rows before a node consumed them, and dropped. Not carved: the incremental engine stays on the GUI thread (it already replays in 8 ms slices; worst measured stall 15 ms), and object-topic tracking waits for its consumer in slice 6a. | merged (#716) |
| 5a | Immutable TF snapshot in `pj_tf` (R1) and whole-state replacement (R4): copy-on-write of a buffer's state, `TransformService::snapshot` | 1t, 1b | none | A reload commit adopts the staging state into the existing buffer, so every holder sees it without a rebind. Consumer rule: while a dataset's TF ingest is active, take a new snapshot no faster than the brake rate (each new snapshot makes the next write copy the state). | merged (#732) |
| 5b | Marker scripts honour cancellation inside the Luau interpreter (GAP-27), and their native output has a byte ceiling (BUD-21) | 3 | none | A run that `BatchEngine` cancels stops at the next interrupt instead of running to its fuel limit | merged (#733) |
| 5n | Native point-cloud and scene-entities operations (D13-D15, D27) and the memory budget they charge | 5b | none | Image, depth and video operations go to slice 9. The XYZ/XYZI/XYZRGB presets (D14) wait for the first binder that builds a cloud from scratch; no planned slice does yet | in review |
| 5s | One Luau sandbox state shared by the filter, marker and (next) object-script engines; the isolation test (GAP-38) | 5b | none | A refactor: no behaviour change | in review |
| 5c-1 | Luau object scripts: compile, run, field-table reads, `pj.tf` | 5a, 5b, 5n, 5s | client | Fresh VM per call (40-47 µs over compiled bytecode) until the series slice measures reuse. Lazy list access (100,000 elements in 11 ms); output declarations limited to the types a script can produce: point cloud, scene entities, image annotations, image, depth image, camera info (GAP-42; video frames wait for slice 9, frame transforms for a decision on the TF ingest tap); P3 folded (lenient binding only; no inferred-outputs list, an output's type is its value's). Scripts read TF in the entry clock by default; `pj.tf.lookup(target, source, object)` reads the header clock at the object's own stamp, and an object input's stamp enters the script on the dataset clock (`NamedValue::stamp_shift`, GAP-29). No script writes a field yet, so the optional-field setters wait for the builders (5c-2). Decided: a 3D dock's robot-model bridges may appear in a script's snapshot (they only add frames). The native budget is a run parameter, a flat 64 MiB by default (8 MB fails the transform of a 128-beam lidar scan); when a native op fails on it, the binder runs a full collection and retries the op once. Strings leaving the VM are charged to the same ledger, and an unavailable marker keeps its reason (cut to 1 KiB) inside the VM. The per-run ledger bounds memory during the run; after it, 6a sizes results from the decoded objects (§9) and the in-flight reservation ends at publish, so no charge list is needed (5c-2 included: a builder's reservation dies with its box at `lua_close`). A failed run reports what it observed (`ScriptError`), so 6a knows whether it may succeed at another TF revision or instant. | in review |
| 5c-2 | Scene and annotation builders, `pj.unavailable`, the US1 + US5 reference script | 5c-1 | client | `pj.unavailable` was already exposed by 5c-1 (D25). `pj.scene` is the binder half of the 5n builder; `pj.annotations` brings a native builder of the same shape (`image_annotations_ops`: budget on creation, `finish()` consumes it). Both are chainable boxed builders over the 5c-1 helpers (checked userdata access, the `{x, y, z}` vector, locked metatables); every error names its field and a spent builder raises. A finished object's reservation is shared by its script box and every view read from it, so a retained output keeps counting however soon its builder or root box dies, and lapses with them at `lua_close` (no charge list). A method reads its options table raw, so no script code runs while a call holds its builder. The annotations stay unstamped (`timestamp` 0): a run knows its inputs by name, not by topic, so the binder cannot find the annotated image's time (the prototype asked a media host, slice 9), and the store entry takes the result's time. Builders write through their native half, not a field table, so the optional-field setters stay unneeded until a script writes fields. A builder that outgrows the native budget fails there, with no collect-and-retry (that exists for the large cloud outputs). The reference script runs unchanged. | in review |
| L | One life-cycle protocol for the three engines; layout persistence moves out of `MainWindow` | 3, 4, M | none | Unchanged generators keep their topics (test-first); a mixed-dataset layout restores the same way whatever the load order (GAP-48: the first saved element's dataset wins; carved as L3 (#718), independent of L1 and L2); runtime hosts assembled once (no "KEEP IN SYNC"); real layouts saved by a `release-4.1` build as restore fixtures; history restore keeps unchanged generators live, so an undo creates only the rules it changed. Three PRs. L1 (#719): fixtures written by a scripted writer on release-4.1 (`pj_app/tests/fixtures/layouts/release-4.1/`; a generator window and a mixed-dataset dock are not producible there), `MainWindowRelease41LayoutTest` (green before and after the move), and a pure move of the persistence into `pj_runtime`'s stateless `DerivedObjects` with today's order and create wait; `applyWorkspace` parses once. L2 (#720): the protocol behaviours. An unchanged generator keeps its topic, published set and run state (per id; it runs again when a series it reads is removed or created by the restore, or, for an all-datasets rule, when a contributor's offset moved since its last published run); a layout load creates a changed rule with the bounded create, while an undo runs a changed rule to completion (decided 2026-10-13, kept 2026-10-17); the catalog is rebuilt before the generators install; closing a toolbox ends its previews in both engines and keeps its persistent recipes (`clearTransformsForPlugin` deleted, GAP-36); `DerivedRuntimeHosts` is assembled once for the GUI and headless sessions (headless gains `kind=markers`); the snapshot carries its intent and one diagnostics channel. L3: GAP-48 | merged (L1 #719, L2 #720, L3 #718; follow-ups #717, #728) |
| S | Spike: pull versus ring (ARCHITECTURE §9) | none | client | Ended after step 0 (the publication cost, measured on the prototype) and a paper check of D26. Ring chosen (ARCHITECTURE §9), 2026-10-14. No throwaway branch was built. | done |
| 6a-0 | Store primitives: atomic bundle publication without per-topic ceilings, with non-prefix eviction; derived-topic metadata | none | none | N7: nothing is refused for size at the primitive level. `pushOwnedBundle` returns the UIDs it created; `append = false` plus a `drop` predicate evicts entries anywhere in a topic, so the ring needs no other eviction call. Not carved: the per-topic ceilings, `evictWhere`, `indexOf`, `contentIdentity` (MED-25, slice 9), the publication generation and the metadata JSON-object check (ABI-36, slice 8a-2), `image_codec` on a derived image topic (slice 9). The `drop` predicate stays a `std::function` called per entry (follow-up h). A retention jump that expires N entries still runs N `evictFront` shifts. A bundle of three topics costs 3 µs at 100 retained entries per topic and 70 µs at 3,600 (`ObjectStoreBundleBench`, `PJ_BENCH=1`) | merged (#738); in-place follow-up merged (#742) |
| 6a-1 | `RecipeGraph`: recipes, bindings, the DAG, validation, ownership, previews | 5c-1 | client | Pure logic; no series state (slice 11). Inputs bind through an injected resolver, so the graph has no catalog dependency; only an object output is an upstream edge. A recipe is Luau only until slice 10; its script is compiled once per revision at upsert, and the node keeps the bytecode for the scheduler. A revision is graph-wide and monotonic, and an identical upsert keeps it. A binding carries the bare topic it reads, for the stager (6a-3). `unchangedRecipeKeys` takes no list of transform outputs the restore re-creates (BatchEngine's `removed_upstream`): 6b adds it if an on-demand recipe reading a re-created transform output must run again. No label field, no `recipeJson`, no listing: their consumers (8a-2, 12) add them | merged (#739) |
| 6a-2 | `ResultHistory` and the ring (ARCHITECTURE §9) | 6a-0, 6a-1 | client | Two global budgets, 256 MiB each by default: the ring (serialized bytes, evicts the entry farthest from its node's cursor, never refuses) and the compute cache (decoded bytes, sized by time span); the results never evicted (each node's displayed one, pinned findings) may exceed the budget, reported separately. Findings at the minimum store timestamp. Step 0 requirements 1-4. A derived object keeps the stamp its script left on the payload; the store entry uses the result time. The ring evicts all of a node's outputs at a stamp in one `pushOwnedBundle` (`append = false` plus a `drop` predicate): atomic, so a node's outputs never disagree. The dedup ledger is per entry, stamp -> (cache key, uid), checked with `at(id, uid)`, which is exact and stays valid after the ring's own evictions. The time-bounds scan and the refill prune must skip derived topics (`isDerivedObjectTopic`, as they skip marker topics), or a finding at the minimum timestamp stretches the playback range. The methods the scheduler needs (`lookup` by stamp, `resultAt`, `shownAt`, `supersedeTombstones`, `serializeOutputs`, the publish options, `clear`) land here with their unit tests, so the contract is reviewed once; their consumers are 6a-3 (`resultAt`) and 6a-4a. A failed install changes nothing, and the compute cache keeps no decoded copy of a result the ring holds (decided 2026-10-18): 600 results of a 260k-point cloud leave 11 KB in it, not 254 MiB. Not done, since each needs an outside writer on a derived topic or many late results at one stamp: a partial external eviction of one output of a result leaves the surviving entry untracked, an external replacement that keeps the entry count can mislead which entry the budget spares as displayed, and several cache-only results at one stamp are scanned linearly | in review |
| 6a-3 | `InputStager`; decoding on workers | 6a-2 | client | Step 0 requirement 5 (a lazy entry's chunk decode too); a parser-backed topic decodes under its per-topic parser lock. Staging reads identities only (`latestEntryIdAt` and the topic's parser, a scalar sample, `ResultHistory::resultAt`): 0.1-0.24 µs at 1 and 260k points, against 4-5 ms in the prototype, and 35 µs among 10k topics and 10k scalar names; the decode (0.3 µs for the 260k cloud, a view over the store's bytes, plus the fetch) runs on any thread and returns the parser keepalives, which the compute cache keeps with a result. A pinned finding is an input only from its real stamp (decided 2026-10-18); an unavailable upstream output is a missing input with its reason (D25), which the ring's ledger records per output. Not carved: a lookup cache per node revision (measured unneeded), string scalar inputs, video and image inputs (slice 9), TF snapshots (6a-4) | in review |
| 6a-4a | `NodeScheduler` under D30, the `OnDemandService` facade, the in-flight budget for on-demand runs | 6a-3 | client | New code; the prototype's scheduling (cancel per tick, upstream runs on the GUI thread) is not ported. D30 suite written first against a fake 90 ms run; D30 wording decided 2026-10-18. Every request stages and is served from `ResultHistory` when it can; a completion settles the current request it serves, or serves it again; a late result is only cached; a success drops the tombstones it supersedes; a remembered failure fails and clears without a run. Chains: one task per node, upstream first; every publication or settlement of an upstream serves its readers (ASY-14). `InFlightBudget`: an RAII reservation of each run's VM ceiling, waiters first in, first out, a run larger than the budget alone (BUD-03 and BUD-06 lose the staged-input budget); output serialization on the worker. A failed install keeps the previous recipe at its revision, its run in flight included (the SDK contract of `create_data_processor_v2`). Lower slices: `ResultHistory` (`lookup` by stamp, `shownAt`, `supersedeTombstones`, `serializeOutputs`, publish options, `clear`), `RecipeGraph` (shared bytecode). Deferred to 6a-5: demand, the epoch hook. Not done: `Slot::readers` is not pruned, so an upstream still serves a node that stopped reading it, one wasted run before that node converges | in review |
| 6a-4b | `BatchEngine` admission joins the in-flight budget (BUD-11) | 6a-4a | none | One budget for both engines on the pool; a release by one engine wakes the other's waiting runs | planned |
| 6a-5 | Session wiring: cursor, demand, dataset epoch, removal and merge mid-flight | 6a-4a | client | DEM-03 (demand leaving drops the pending request; a running run completes into history); GAP-32 | planned |
| 6c | On-demand at the live edge | 6a-5 | client | The ingest announcement plus a per-node newest-stamp check; no object-change feed, which missed objects written by toolboxes (STR-05/07 removed) | planned |
| 6b | On-demand recipes join the layout protocol | 6a-5, L | client | The prototype's `<on_demand>` element, without `infer_outputs` (read and ignored); restore fixtures saved by the prototype (GAP-39); an unchanged recipe whose upstream the restore re-creates runs again | planned |
| 7 | Scene layers on derived topics: readiness, the lag indicator, "why is it empty", framing | 6a | client | The D30 lag indicator on the layer row, with the number; a plot-level computing/lag indicator next to the markers (slice 3's status-bar line is the interim); the dock's time-range clamp and live-edge fold skip findings | planned |
| 8a-1 | `GridRunner`: window and instant evaluations with reports (the engine half of slice 8) | 6a-4a | none | The internal budget type; the decided report shape | planned |
| 8a-2 | The on-demand plugin bridge: v2 create, submit/poll/release, the router's third backend | 8a-1, 7 | contract | `DataProcessorsKindRouter` checks `struct_size` first; the #202 decisions applied; each plot-tab slot filled on its own; a known but unservable kind rejected by name; host-contract wording of `create_data_processor`: failures detected within the first budget are returned, later failures are reported asynchronously; the contract (#202) documents the marker config keys `computing` (bool) and `error` (string, present only after a failed run); host contract: an opt-in flag on the v2 create for a non-blocking marker create; released v1 creates stay synchronous | blocked on the SDK tag |
| 8b | Catalog v2, plot-tab scene tabs, dialog scene views, the non-blocking marker create | 8a-2 | contract | Each plot-tab slot filled on its own; a known but unservable kind rejected by name | planned |
| 9 | Media for on-demand: images, depth, video | 6a, 7 | client | Images and depth publish into the ring under the global byte budget | planned |
| 10 | The Python lane | 5, 6a | client | Python N1: compile once per revision, read-only modules (test-first: `math.pi = 3` does not leak); native operations release the GIL | planned |
| 11 | Series: on-demand, demand-gated, unified with the window loop, union clock | 6a | client | D10 as revised; decoding in the worker; entries batched; the visible range first; "not computed" and "computing N%"; assistant queries count as demand | planned |
| 12 | Custom Topics panel (from #698) | 6a, L | client | none | planned |
| 13 | Transform Editor host support (from #699): output inference, embedded scene views, previews owned by the host | 8, 11 | contract | `INFER_OUTPUTS` folded; one `scene_view` key; attach failures reported to the plugin | planned |
| P | Spike: Python in the browser (CPython 3.14 for Emscripten) | 10 | client | ARCHITECTURE §14 | proposed |

Performance follow-ups (2026-10-18 hot-loop review):

- (a) The point iterator divides per point and switches types through `readXyz` (`nextFinitePoint`, binder `xyz_points`): about 3 ms per evaluation of 260k points.
- (b) A list element read allocates a userdata box and scans field names linearly (binder).
- (c) `estimateBytes` walks fixed-size list elements.
- (d) `ResultHistory::present()` fetches an entry to test it exists (needs `ObjectStore::contains`).
- (e) `InputStager` looks topics up by linear `findTopic`.
- (f) `RecipeGraph` queries: `missingInput` grows exponentially on diamonds, `dependentsOf` is O(V·(V+E)).
- (g) The SDK's `serializeBuiltinObject` reallocates once more than needed (a `reserve` in the SDK writer).
- (h) Replace the `drop` predicate with a time range plus a sorted stamp list, if the per-entry `std::function` shows up in a profile.

## Open decisions, by the slice they block

Slices 0-4, M, L and S have landed (batch 1). Batch 2 is slices 5a to 7, planned
2026-10-17 from two independent plans (Codex and Fable) reconciled against the code.

| Decision | Needed before | Why |
|---|---|---|
| Approve slice 0 (these documents) | Carving starts | Reviewers judge each slice against the written target |
| ~~Split SDK #202~~: decided (split). The header-only 0.36.0 was released 2026-10-17 (SDK #204). | Slice 5 | Slices 5-7 and 9-11 need the field tables in a released SDK |
| ~~Start the pull-versus-ring spike~~: agreed 2026-10-13; it runs first | Slice 6a | 6a builds `ResultHistory` one way or the other |
| ~~Pull or ring~~: ring, decided 2026-10-14 (ARCHITECTURE §9) | Slice 6a | Step 0 showed the publication cost is negligible, and a fixed early store timestamp meets D26 |
| ~~Two timestamps per TF sample~~: decided 2026-10-13 (slice 1b) | Slice 5 | Slice 5 gives scripts TF lookups, and the default lookup clock is part of their contract |
| ~~A chain in one worker task~~: decided 2026-10-17, one task per node (6a-4) | Slice 6a-4 | Fan-out is free and no script runs on the GUI thread; the chain task stays the upgrade if the measured 2-hop latency calls for it |
| ~~A pinned finding as an input before its pin time (SCENARIOS DAG-11)~~: decided 2026-10-18, computations see it only from its real stamp (slice 6a-3); the display shows it at any time | Slice 6a-3 | Causal results for windows and series, against a whole-log reference |
| ~~D30 wording~~: decided 2026-10-18 (REQUIREMENTS D30): after a backward seek, "the display only moves forward" means not older than the result shown for the current request; the lag is how far the shown result is behind the one the current request would produce | Slice 6a-4a | The written lag counts a sensor's sampling interval as backlog |
| Where a finding sits in a moving frame: where it was measured (§12 placement), or with the frame | Slice 7 | §12 already places a stamped object where it was measured |
| Batch boundary: rework the host contract in the SDK in parallel and end the batch with 8a-1 and 8a-2, or pull slice 12 forward | Slice 6a-1, if the SDK work runs in parallel | No plugin can create an on-demand recipe before the host contract (§5 removed the v1 route) |
| ~~Robot-model bridges in a script's TF snapshot (D2)~~: decided for slice 5c-1, accepted: they only add frames, never change a lookup between recorded frames | Slice 5c-1 | A 3D dock injects fixed-joint edges into the dataset's buffer, so a lookup would depend on whether such a dock is open |
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
| 2026-10-17 | Batch 1 landed. SDK 0.36.0 released and pinned (#731). Batch 2 planned: slice 5 split into 5a, 5b, 5n, 5c-1 and 5c-2; slice 6a into 6a-0 to 6a-5 plus 6c; slice 8 into 8a-1, 8a-2 and 8b. |
| 2026-10-18 | D30 wording decided (the lag's reference, "only moves forward" after a backward seek). Slice 6a-4 split into 6a-4a (the scheduler, the facade, the in-flight budget for on-demand) and 6a-4b (batch joins the budget). |
