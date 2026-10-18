# Derived objects: target architecture

**What this is.** This is the design the derived-objects work is being delivered toward,
on `release-4.1`, as a stack of small pull requests (see [CARVING.md](CARVING.md)). It
folds in the decisions taken on 2026-10-10, the prototype author's review of them on
2026-10-13, and the ring decision of 2026-10-14 (§9). A functional prototype exists: pull
requests #697, #698 and #699, the reference implementation, at branch
`feat/derived-objects-editor-host`. Where the prototype and this document differ, **this
document is the target**.

**How to read it.**
- Requirements and decision IDs (D1-D30) are in [REQUIREMENTS.md](REQUIREMENTS.md). The
  behaviours each step must keep are in [SCENARIOS.md](SCENARIOS.md).
- The rationale for each change against the prototype is in the review notes (N1-N10,
  `derived_objects_critical_review.md` on the `planning/lua-derived-object-dag` branch).
- Every section carries a status:
  - **Decided:** agreed, to be built as written;
  - **Proposed:** recommended, not yet agreed;
  - **Spike:** to be validated by an experiment before deciding;
  - **Built:** present in the prototype and kept as is;
  - **Pending SDK:** depends on the host-contract decisions (the minor after 0.36.0).

## 1. Goal

**Status: Decided.**

PlotJuggler already derives data from scalar series: a Luau transform yields a series, and
a marker rule yields a `PlotMarkers` set. This work lets any client of the plugin ABI declare
a function over any mix of scalar and object inputs (point clouds, transforms, scene
entities, images, annotations) and get typed results. Object outputs appear as ordinary
topics, which viewers render without knowing they are derived. The minimum walkthrough is
US1 (crop a cloud, preserving its fields) plus US5 (a measurement with a visible witness
from one evaluation) (D28).

## 2. Vocabulary

- **Topic:** a named stream inside a dataset. **Entry:** one timestamped item of a topic.
- **Recipe:** a stored declaration: engine kind, language, inputs, typed outputs,
  parameters, script. **Node:** a live recipe bound to concrete inputs and outputs.
- **Bundle:** the result of one evaluation of a node. It holds one value per declared
  output, produced together from the same resolved inputs, plus provenance.
- **Result:** a bundle together with the stamp it is shown at (the newest resolved input
  timestamp).
- **Finding:** a result the user or an agent chose to keep, pinned to one data time (D26).
- **Preview:** an ephemeral recipe owned by one plugin instance. It is never persisted.
- **Demand:** a consumer of a node's output: a displayed layer, a plotted series, an
  explicit request.

## 3. Three engines, named by trigger

**Status: Decided.** The names are kept, with no further renames. The plugin-facing kinds
stay `transform`, `markers` and `on_demand`, with no aliases.

| Engine | Module | Kind | Triggered by | Runs over | Produces | State |
|---|---|---|---|---|---|---|
| `IncrementalEngine` | `pj_datastore` | `transform` | an entry arriving | that entry, in ascending order | one output per input entry (a history) | carried between entries |
| `BatchEngine` | `pj_runtime` | `markers` | a commit, a refresh, an explicit request | all entries of its inputs, or a window | one bundle | none (one run) |
| `OnDemandService` | `pj_runtime` | `on_demand` | a consumer asking for time `t` | the inputs resolved at `t` | one bundle for `t`, cached by dependency identity | none; required (D2) |

None can replace another:
- on-demand never builds a history;
- batch answers for a whole series;
- only on-demand evaluates at an arbitrary `t`, with no scene open (D19).

The leftovers of the earlier rename (`DerivedEngine` → `IncrementalEngine`,
`MarkerService` → `BatchEngine`) are removed: the alias header, the second accessor, stale
comments and stale `CLAUDE.md` mentions.

## 4. Invariants

These rules hold in every slice. A change to one of them is a decision, recorded in
REQUIREMENTS.md or here.

1. **Result policy (D30): the newest completed result wins.**
   - It applies to on-demand requests from the cursor and the live edge, and to batch
     runs.
   - A completed valid result is published whenever it is newer than what is shown. It is
     never thrown away because a newer request exists.
   - Work is superseded only when its meaning changes: recipe, parameters, dataset epoch or
     removal.
   - Each node has one running run and at most one pending request, which keeps only the
     latest time.
   - The display only moves forward. State comes from the newest request. The scene shows
     the newest result at or before the cursor, plus how far it lags.
   - Explicit evaluations (submit/poll) are outside this rule: each handle is its own
     consumer.
2. **Statelessness by isolation (D2, D29).** A function's result depends only on its
   resolved inputs, parameters and observed host services. It is enforced by isolating
   what a script can reach, not by rebuilding the interpreter for every call (§14).
3. **One dataset per node (D7).** A recipe whose inputs span datasets is rejected. Each
   dataset has its own TF buffer. A 3D dock binds to one dataset.
4. **Nothing is computed without demand (D1).**
   - On-demand evaluation runs for a displayed or requested node.
   - Batch runs for a consumed output, behind a reload brake.
   - Series replay runs while the series is displayed or explicitly requested.
5. **Missing input and errors clear the display (D6).** An unavailable output is reported
   per output (D25), separately from an empty result, a missing input and an execution
   error.
6. **Threading.**
   - Scripts never run on the GUI thread.
   - The GUI thread resolves inputs and applies results, in bounded passes.
   - No script runs while a store lock is held.
   - Python runs on its own single-thread lane.
7. **The cursor stays independent.** Evaluating or showing a result never moves the cursor
   (D22). An explicit-time evaluation is independent of the cursor (D19).
8. **Domain-neutral host.** Mechanisms are generic; plugins supply the domain meaning.

## 5. Recipe and bundle

**Status: Built.** The contract details are decided and land with the host-contract release.

- Outputs are declared with fixed types (D8, D9): number, string, or a canonical object
  type.
- A script returns a table keyed by output name. Omitting a declared output is an error.
  `pj.unavailable(reason)` marks one output unavailable (D25).
- A bundle is atomic and carries provenance: the requested time, each resolved input's
  real timestamp, the output frame, and the script and parameter revisions.
- A preview evaluation that only wants values returns them as a JSON report. Nothing is
  published.

**Decided for the host contract** (SDK #202; agreed with the prototype's author on
2026-10-13). They are applied before the host-contract release, the minor after the
header-only 0.36.0:
- **Poll report:** only what clients act on: `coverage.{candidates, evaluated, stopped,
  error, gaps}`, and per bundle the requested time, the resolved input times as a map, and
  the outputs. The SDK gains `parseEvaluationReport()`, which also derives the bundle stamp
  (the latest resolved input time), so the editor and the assistant parse it one way.
- **One error shape:** `FAILED` carries the report with `stopped:"error"`. `CANCELLED` is
  reserved and never emitted; a client treats it as not-success.
- **The v1 `"<name>:<type>"` route for `on_demand` and `createOnDemand`** are removed.
- **`INFER_OUTPUTS` is folded.** A scripted submit with no declared outputs infers them,
  and lenient binding is the only on-demand binding. It still rejects an undeclared key
  and a missing declared output. The flag bit, its floor entry and the persisted
  `infer_outputs` layout attribute go.
- **A nonzero evaluation-budget `reserved`** is rejected.
- **WINDOW and INSTANT** are mutually exclusive.
- **An optional typed dataset id on the v2 request** (D7). Empty means today's resolution:
  a unique name, or an error. A plugin copies the id from catalog v2, and layouts already
  map dataset ids on load (`SessionManager::resolveDatasetIdentity`), so nothing new is
  persisted. The released per-input `source:topic` qualifier stays.

## 6. Types and bindings

**Status: Built.** The Luau binder (slice 5c-1, `pj_scripting/src/object_binder.cpp`) has both
fixes below; the Python binder comes with slice 10.

- **SDK field tables** describe each builtin struct's members: name, kind, accessors. They
  drive generic binders in Luau and Python, so a script and an agent see the same fields.
- **Heavy operations are native C++ per type,** in `pj_scripting/native/`: filter, crop,
  transform, bounds, builders, presets. They serve both languages.
- **Lazy lists (built, slice 5c-1):** binders read list elements one at a time through
  `list_at`, instead of materializing whole lists on every field access (the prototype's
  O(N²) iteration). Iterating 100,000 elements takes 11 ms (§19).
- **Producible outputs (built, slice 5c-1):** an output declaration accepts only the object
  types a script can produce: point cloud, scene entities, image annotations, image, depth
  image and camera info. Video frames wait for slice 9, frame transforms for a decision on
  the TF ingest tap (a derived TF topic would feed the dataset's buffer). The prototype's
  `parseOutputSpec` accepted any builtin type, so a script could declare an output it could
  never return.

## 7. Time and alignment

**Status: Built** (D5, D17).

- **Inputs** are resolved independently at their closest sample at or before the
  evaluation instant. There is no primary clock.
- **The result's stamp** is the newest resolved input timestamp.
- **The query clock** (`pj.tf.now` and the default lookup instant) enters a result's
  identity only when the run observed it.
- **TF lookups are a zero-order hold** in the lookup's clock (entry or header, §12): the
  newest sample at or before `t`, with no interpolation.

The TF clock question is in §12.

## 8. The on-demand pipeline

**Status: Decided.** Each component's row says whether it is built.

`OnDemandService` is a thin facade over concrete classes, with no single-implementation
interfaces. It starts with the public API that `SessionManager` and the plugin hosts use
today, and is transitional: each consumer moves to the component it needs (a layer reads its
content from the store and its state from `NodeScheduler`, the Custom Topics panel moves to
`RecipeGraph`). The facade keeps only the orchestration that spans components: install,
remove and restore.

| Component | Responsibility |
|---|---|
| `RecipeGraph` | Recipes, bindings, the DAG and cycle rejection, validation, ownership, previews, findings, dependents. Pure logic, no Qt. Built (slice 6a-1). |
| `InputStager` | Resolves inputs at `t` on the GUI thread by identity (an object entry's topic, UID and parser, a scalar sample, an upstream's result) without fetching a payload, and builds the request's cache key; `decode` fetches and decodes them on any thread, a parser-backed entry under its per-topic parser lock and a canonical point cloud as a view over the store's bytes, and returns the parser keepalives its objects need, which `ResultHistory` keeps with a cached result. Built (slice 6a-3). |
| `ResultHistory` | Per node: the compute cache and the ring (§9). Results keyed by cache identity (§10), ordered by stamp and sized by time span: a window around the node's cursor, then one global byte budget each that evicts the result farthest from its node's cursor. A miss checks the store first (§9), and the cache keeps no decoded copy of what the ring holds. Built (slice 6a-2). |
| `NodeScheduler` | Per node: one running run plus one pending request, demand, the in-flight budget, submission to `EnginePool`, supersede on meaning change (invariant 1), and the node state that layers read (computing, missing input, error). |
| `GridRunner` | The per-instant loop shared by window evaluations and series, with a report sink and a column sink (§15). |

Data flow for one cursor request:

```
cursor / live edge ──► NodeScheduler ──► InputStager (GUI: resolve by identity)
                                              │
                                   EnginePool worker: decode, run script, stamp
                                              │
                                   GUI drain ──► ResultHistory + store write ──► consumers read the store
```

**Chains.** A downstream node reads its upstream's object output from the ring
(`ResultHistory::resultAt`) and decodes it like any input, a point cloud as a view over the
store's bytes: the compute cache keeps no decoded copy of a result the ring holds (§9). The
scheduler asks after the upstream's own request at that time, so the result read is the
upstream's displayed one, which the ring never evicts.
**Proposed:** compute a whole chain in one worker task, instead of
staging upstream nodes synchronously on the GUI thread. If adopted, two conditions hold:
- Each intermediate result lands in its node's `ResultHistory` and in the store, so an
  upstream node that is also displayed, or read by another node, does not run twice.
- The budget and cancellation cover the whole chain, and the report names the node where
  it stopped.

## 9. Where results live

**Status: Decided** (N7, 2026-10-14). The pull-versus-ring spike ended after step 0 and a
paper check of D26. Pull is rejected and the ring is the target.

**Target (ring).**
- **Results keep being written into the ObjectStore.** Each derived topic's window is a
  ring: it evicts instead of refusing publication. Nothing is refused, so a node never
  goes to error or hides its layer because of a ceiling.
- **One global byte budget covers every derived topic.** Today the compute cache has a
  global budget, but the store's ceilings are per topic: 55 derived topics could hold about
  7 GB. When the budget is full, the result farthest from its node's cursor (the node's
  last requested time) is evicted, across all nodes. Each node's displayed result (the
  newest at or before the cursor) is never evicted; that is the per-node floor.
- **The compute cache holds no decoded copy of what the ring holds** (decided 2026-10-18),
  so a large result is held once, not once in each budget. A cached result keeps its number
  and string outputs; it keeps its decoded object outputs only while the ring does not hold
  it at its stamp (a late result, cached only), and it goes with its ring entry, since it
  cannot serve a hit alone. A reader of a ring-held output decodes it from the store, a
  point cloud as a view over the store's bytes.
- **Readers see a derived topic like any store topic:** the newest entry at or before `t`.
  Plugin reads, the catalog, drag and drop, layouts and the Custom Topics panel work
  unchanged. A derived topic exposes the results in its window to plugin reads at any `t`.
- **The state of a node** (computing, missing input, error) comes from the node, not from
  the store (invariant 1). The lag is the cursor minus the stamp of the entry read.
- **Findings (D26).**
  - A pinned node writes its single entry at a fixed early store timestamp. The payload
    keeps its real stamp, and scene placement uses the payload stamp.
  - Every at-or-before read then returns the finding at any `t`, with no reader change and
    no datastore change. Plot markers already work this way (`Timestamp{0}`, one entry).
  - It counts against the budget, is exempt from eviction and is bounded by the
    per-owner pin cap.
  - The computation-time label comes from the recipe's pinned time, not from the store.
  - A computation reads a finding only from its real stamp (decided 2026-10-18): an input
    resolved at `t` before the finding was computed is missing, while the display shows the
    finding at any instant.
  - A finding ignores a model entity's finite lifetime (otherwise it would count from the
    fixed timestamp). The dock's time-range clamp and the live-edge fold skip findings.
  - **Accepted consequence:** a plugin reading a finding through the object-read service
    sees the fixed timestamp in `out_timestamp`. The real one is in the payload.
  - **Revisit if** a generic store read must report a finding's real computation time: a
    plugin's `out_timestamp`, a TF lookup at the finding's own stamp in a moving frame, or
    lifetimes counted from the real stamp. The fallback is a datastore "latched" flag (the
    first entry is returned for earlier `t`), but `rangeByTime` and `maxUidAtOrBefore` do
    not honour it, so it pushes work into the readers.
- **The declared range:** a derived layer's time range comes from its inputs' range, not
  from what has been computed.

**Requirements for slice 6a** (from step 0):
1. Byte estimates never serialize. Size inputs and outputs from the decoded object (for
   example point count times stride). Today the input estimate serializes the 2.88 MB scan:
   2.4 ms median and 13.5 ms worst per result, on the GUI thread.
2. A compute-cache miss checks the store first. 37-48% of backward-seek recomputes were
   already stored, and were then discarded as duplicates.
3. The per-node compute cache is sized by time span, not entry count. With 8 entries it had
   a 0% hit rate on a 1.5 s jog.
4. Publication is never refused (see the target).
5. A lazy input entry's chunk decode (3.2 MiB of LZ4, 2.2 ms) moves off the GUI thread,
   with the rest of the decoding in the worker.

**Evidence.**
- **Step 0** measured today's GUI-thread publication cost on a Waymo recording (prototype
  766b20eeb). Stamp, serialize, store write and ledger cost at most 0.16 ms per result for
  small objects, and 0.4-0.85 ms for a 1.5 MB point cloud: about 0.4-0.9% of GUI time at
  10 Hz. The large GUI costs were prototype debt that does not depend on the model
  (requirements 1-3 and 5).
- **The D26 paper check.** The gap under store publication is the at-or-before read
  (`latestAt` returns nothing for `t` before the finding's stamp), not retention: pinned
  entries are already exempt from the window trim. The fixed early timestamp closes it in
  about 15-25 production lines.
- **The kill criterion** (the cost is negligible and D26 is accepted another way) is met.

**Rejected: pull** (layers read from the evaluator through `frameAt`, with one lazy store
entry per derived topic). Its D26 handling is about 5 lines, but only after every layer
moves onto an injected reader, and it needs the same fixed timestamp for plugin reads and
the same lifetime rule. With the publication cost negligible, it buys nothing the ring does
not.

## 10. Cache identity

**Status: Built**, with a change proposed.

- A result is cached under everything that determined it: the script and parameter
  revision; each resolved input's identity (UID and source generation); upstream
  revisions; the representation; and the services the run observed.
- A stored key serves a request when the base fields are equal and what its run observed
  (the query clock, the TF revision) is the same now (`CacheKey::serves`). A lookup reads
  only the request's stamp, so it costs 0.1 µs at 1 to 10,000 results per node (§19).
- **Proposed:** one observed-dependency list per result, with clock, TF and video as
  entries, instead of key variants. The TF entry records the edges walked and the sample
  chosen on each (§12, R5), so unrelated live TF traffic no longer invalidates results.

## 11. Scheduling and execution

**Status: Built (slice 2)** (the pool), **Built (slice 3)** (the batch queue per node) and
**Decided** (the on-demand queue, worker decoding, GIL release). The chain-in-one-task item
is proposed.

- **`EnginePool`** (`pj_runtime/include/pj_runtime/EnginePool.h`) runs ready tasks. It keeps
  no per-node state: no mailboxes, no coalescing, no supersede by generation.
  - A task is a `compute` (worker thread, its captured inputs only) and a `publish` (GUI
    thread).
  - The shared lane is a dedicated pool bounded to `max(1, ideal/2)` threads. The Python
    lane is one separate thread, since the GIL serializes Python anyway. In the browser
    both run on Web Workers.
  - Both lanes request `QThread::LowPriority`. Qt ignores thread priorities on Linux (its
    documentation says so; threads stay SCHED_OTHER), so there the bound on the shared
    lane, half the cores, is what keeps the GUI thread responsive.
  - `submit` returns a cancel token, which the consumer keeps to cancel the run when its
    meaning changes. Cancellation is cooperative (`compute` polls the token), and is
    reserved for that: a cancelled run still publishes, even when its compute had finished.
  - Every run publishes exactly once with an `Outcome`: `kCompleted`, `kCancelled` (wins
    over `kFailed`) or `kFailed` (`compute` threw; `error` carries `what()`, and the worker
    and the pool survive). `publish` runs some time after `submit`, also after a cancel,
    so it must not capture a raw pointer to an object that may be gone (`QPointer` or a
    weak guard).
  - Publication happens only in a GUI-thread drain, at most 32 per pass, then the event
    loop turns. A drain pops one result at a time, so a `publish` that pumps events (or
    calls `waitIdle`) sees the rest as pending, in order, and one that destroys the pool
    ends the drain.
  - Destroying the pool cancels every token, waits for the running computes and calls no
    `publish`.
- **The queue per node** follows invariant 1 and belongs to the consumer (`BatchEngine` in
  slice 3, `NodeScheduler` in slice 6a), not to the pool. The pending slot holds a request,
  not a task: inputs are captured on the GUI thread only when a run can start, so a burst of
  refreshes costs one capture. The consumer submits only when the node has no run in flight.
  - **Batch (built, slice 3).** Per generator binding: one run in flight and at most one
    pending `refresh`; the full result policy is the `BatchEngine.h` class comment. An
    input change, or a catalog addition for an all-datasets rule, only marks the binding
    stale (a streaming tick reports only the topics whose last timestamp, row count,
    retention floor or content generation moved); the brake (one single-shot timer at the
    reload rate, 2 Hz by default; Manual mode stops it) submits the stale, demanded
    bindings with no run in flight that did not fail on their current inputs, and the
    runs one pool drain publishes refresh the plots once. A bound rule is demanded when
    its dataset is; an all-datasets rule when one of its contributors is. A display
    reference wakes the brake (`TopicDemandTracker::referenceDemandChanged`), so does an
    active-set change. A layout load's create waits at most 8 ms for its first run; an
    undo's or redo's, like a plugin's, runs to completion (§13). The whole-dataset recomputes (reload,
    rollback, dataset removal) stay synchronous and record a failure like a scheduled
    run.
- **The in-flight memory budget** reserves one VM ceiling per evaluation; staging fetches
  no payload, so the inputs are sized after the worker decodes them
  (`scripting::estimateBytes`), and slice 6a-4 decides how they count against it. A request
  that does not fit waits. Not built for batch yet: the prototype's
  batch admission needs the marker script's result ceiling in `pj_scripting` (slice 5).
- **GUI-thread work still to move:**
  - input decoding moves to the worker (decided; `InputStager::decode`, slice 6a-3, runs
    anywhere; `NodeScheduler`, slice 6a-4, runs it on the pool), including a lazy input entry's chunk
    decode;
  - a chain runs in one worker task (proposed; decided while designing slice 6a);
  - byte estimates read the decoded object and never serialize (§9);
  - the batch staging copy, and TF snapshot copies (§12), get measured first. The batch
    re-run over 500k rows now costs the GUI thread 15 ms at most per pass (§19), nearly
    all of it the resolver's copy of the series (staging alone: 13-16 ms).
- **Incremental engine.** Its per-ingest cycle runs on the GUI thread under `lockEngine`,
  and the first replay runs in 8 ms slices. Moving it to the pool is later work, after
  measurement.
- **Native operations release the GIL** while they work on C++ data (decided). Without
  this, a native call on the Python lane blocks any GUI-thread Python work.

## 12. TF

**Status: Built** (the per-dataset service). R1 and R4 are built (slice 5a), R2 and R3
(slice 1t), R6 (slice 1b) and R10 (slice T); the rest are proposed or conditional.

- **`pj_tf`** (Built, slice 1t) is a Qt-free leaf library. `TransformBuffer` keeps a forest of edges with a
  timestamped sample history per edge and zero-order-hold lookups.
- **`TransformService`** (`pj_runtime`) owns one buffer per dataset and the ingest tap,
  which is active from startup. `SessionManager`'s remove, merge and reload paths call its
  life-cycle hooks.
- **Scripts read an immutable per-dataset snapshot** (Built: the snapshot in slice 5a, the
  `pj.tf` binding that reads it in slice 5c-1). `TransformService::snapshot(dataset)` returns a
  `std::shared_ptr<const pj::tf::TransformSnapshot>` with `tryLookupTransform(target,
  source, t, clock)`, `getAllFrames()` and `revision()`. It is never null, it never
  changes, and it is read from any thread without a lock. Calls at one revision return one
  object while a caller holds it.
- **Copy-on-write buffer state (R4).** A buffer's state *is* a `TransformSnapshot`:
  `TransformBuffer::snapshot()` shares it in O(1), and the buffer copies it before its next
  write while a snapshot holds it. Every replacement swaps the state behind the same buffer:
  `clear()` (invalidation, removal, the merge rebuild) swaps in an empty one, and a reload
  commit makes the active buffer adopt the staging buffer's state (`adoptStateOf`, O(1),
  no copy). The dataset's buffer object never changes, so every holder (3D docks, their
  layers, the robot model's fixed-joint bridges, which write into it) sees each replacement
  without rebinding; `epoch()` counts the replacements, for a reader that extends a cached
  result (the TF trail rebuilds instead of appending). A complete file buffer is shared
  without a copy and needs no seal, and an old snapshot always keeps its state. Revisions
  come from one process-wide counter, so a revision names one state across buffers and
  buffer replacements (the script cache key). `setCacheWindow` bumps the revision when it
  evicts.

| # | Change | Status |
|---|---|---|
| R1 | Move the immutable snapshot type into `pj_tf`; drop the scripting-side interface and adapter | Built (slice 5a): `pj::tf::TransformSnapshot` in `tf_buffer.h`; no interface, no adapter |
| R2 | Move the fixed-frame memory (a 3D-view preference) out of `TransformService` to the scene side (`FixedFrameMemory` in `pj_scene3d_widgets`) | Built (slice 1t) |
| R3 | A 3D dock refuses a topic from another dataset, instead of warning | Built (slice 1t; follows D7) |
| R4 | Replace a buffer as a whole object instead of rewriting it in place; seal complete file buffers, so snapshots share them without copying | Built (slice 5a), as copy-on-write of the buffer's state: a write copies a state a snapshot shares, so no seal is needed (the robot model's bridges write into file buffers, which a seal would have refused); the replaced object is the state, behind a buffer that keeps its identity, a reload commit included |
| R5 | Report which edges and samples a lookup used, for the cache dependency | Proposed; measure after R10, which already removes most spurious revision bumps |
| R6 | TF clock contract. The buffer was keyed by header stamps, while lookups used the dataset's entry clock. Fix: two timestamps per sample (header and entry). The cursor and the query time use the entry clock; placing an object uses its own stamp in the header clock. On the nuScenes recording (log time), log time minus header stamp is 0 ms on the lidar, 0-40 ms on the front camera and -37 to +48 ms on `/tf`: the lidar hid the mismatch. See [the two clocks](#the-two-clocks-r6) below. | Built (slice 1b). Decided 2026-10-13. Visual confirmation on the nuScenes cameras pending. |
| R7 | Frames published once are valid before their first stamp | Verify first |
| R8 | Chunked copy-on-write edge storage | Only if measured |
| R9 | Split the ingest pipeline out of `TransformService` | Later |
| R10 | Do not store a sample equal to its predecessor. Exact under the zero-order hold; skips the revision bump unless the repeat evicts something. Equal means exactly equal, component by component (no tolerance): a tolerance would change the user's data. Each edge still records the newest stamp it received, so the latest sample, `latestCommonTime` and the eviction cutoff are unchanged. Eviction keeps the newest sample at or before the cutoff, which holds the value valid there, so a run of repeats longer than the window never loses its value. | Built (slice T) |

### The two clocks (R6)

Each TF sample keeps its header stamp and its entry time: the store timestamp of the message
that carried it, already on the dataset clock after a merge. Every lookup names its clock
(`pj::tf::Clock`).

- **Entry clock:** per edge, the newest-stamped sample that had arrived by `t` ("what the
  recording knew at t"). The cursor and query times are in this clock.
- **Header clock:** per edge, the newest sample stamped at or before `t` ("where the frame
  was when this was measured"). An object placed at its own stamp reads this clock.
- **Equal clocks:** when header == entry for every sample (simulated-time logs, bags logged
  with header stamps), both clocks give the results the header-keyed buffer gave.
- **Storage:** one deque sorted by stamp. Each sample's entry key is the earliest entry time by
  which it or a later-stamped sample had arrived (a suffix minimum), so both keys are sorted
  and one binary search serves either clock: 72 bytes per sample instead of 64.
- **R10 stays exact in both clocks:** a skipped repeat's entry time is folded into the sample
  it repeats. The out-of-order trade-off of R10 applies to both clocks alike.
- **Eviction** drops a sample only when neither clock's live window (newest stamp or entry
  time minus the window) still needs it. `latestCommonTime` and `chainSampleTimes` take a
  clock too; `getLatestSample` reports the header stamp.
- **Object placement** is one rule, `placeObject()` (`pj_scene3d_core/tf/object_placement.h`),
  composed through the root of the tree that holds both frames:
  `fixed <- source = (fixed <- root, entry clock, at the cursor) * (root <- source, header
  clock, at the object's stamp)`. The stamp is the object's own stamp plus its entry's
  `payload_stamp_shift`. A stamped object therefore stays where it was captured in the world
  even when the fixed frame moves (a body frame such as `base_link`): a map-frame plan or
  voxel map stays put and a scan stays where it was taken, both moving in the view as the
  body moves. This holds for an object in the fixed frame itself too. With the root as the
  fixed frame the rule is the header-clock lookup at the stamp.
- **Fallback to the cursor** (`fixed <- source` in the entry clock): an unstamped object
  (stamp 0), a stamp before the first TF sample on the chain, or a stamp more than
  `kMaxStampPastTf` (1 s) past the newest header-clock sample on the chain. Live TF lags the
  data a little, so a stamp just past it holds the last sample; data stamped in wall time
  against TF in simulated time would otherwise freeze at one pose.
- **Placement hold:** each layer keeps the last `root <- source` it placed its object with
  (`PlacementHold`, keyed by root, source frame and stamp). In live streaming the object store
  can keep a stopped sensor's last object longer than the TF window keeps the transform at
  its stamp; once that sample is evicted, the same stamp's lookup fails and the hold is
  reused, still composed with `fixed <- root` at the cursor, so the object does not jump to
  the cursor. The view, the dock's scene bounds and the layer's render key all place a layer
  with its hold, so they agree.
- **Known limits.** (1) A same-stamp replace shows the new value from that stamp's earliest
  entry time: header 100 logged at 110 as v1 and at 300 as v2 reads v2 at 200, before v2
  arrived, because the buffer keeps one value per stamp. The arrival-order reference test
  pins this. (2) A static transform stamped before its log time resolves in the entry clock
  only from its log time, so TF triads and cursor-placed layers can be missing at the start
  of a file. Revisit (with R7) if users report an empty first frame.

Every consumer and its clock:

| Consumer | Clock | Why |
|---|---|---|
| 3D view: TF triads, connection lines, hover label, frame picking, follow camera, TF render key | Entry, at the cursor | They show where each frame is at the cursor. |
| Point cloud, depth cloud, pose array and its trail, voxel grid, grid map | Header at the object's own stamp for `root <- source`; entry at the cursor for `fixed <- root` (a pose-trail point: at its own store timestamp); entry at the cursor as the fallback | An object sits where its frame was when it was measured, as in RViz, and stays there in the world when the fixed frame moves. |
| Occupancy grid | Entry, at the cursor | The drawn grid is a keyframe plus later patches, with no single stamp. |
| Scene entities (markers and their models) | Entry, at the cursor | Entities carry their own stamps and `frame_locked`; honouring both is a follow-up. |
| Robot model links | Entry, at the cursor | The links are TF frames. |
| TF frame trail | Entry (`chainSampleTimes` and its lookups) | The trail is split at the cursor and its head meets the frame's triad. |
| Camera scene bounds, layer render keys, point-cloud axis range | The layer's own placement, with its placement hold | They must match what is drawn. |
| Frame list, fixed-frame combo, orphan check | None | `getFrameHierarchy` and `areConnected` take no time. |
| Scripts (`pj.tf`, slice 5c-1) | Entry by default (no time, or a time); header for `pj.tf.lookup(target, source, object)`, at the object's own stamp | An object input's stamp enters the script on the dataset clock (its entry's `payload_stamp_shift` added, as TF ingest adds it to header stamps). The fixed-joint edges a dock's robot model writes into the buffer may appear in the snapshot: they only add frames. |

With header-stamp placement a stamped object in a moving frame stays where it was measured
until its next sample, instead of moving with the frame at the cursor. This holds even when
the two clocks coincide, and it replaces the "all layers are frame-locked at the tracker
time" rule of `pj_scene3D/docs/REQUIREMENTS.md` §4 for the layers in the second row.

## 13. Life cycle of recipes and results

**Status: Decided** (one protocol). Layout persistence in `pj_runtime` is built (slice L1,
#719), and so are the protocol behaviours: unchanged generators keep their topics,
previews end with their owner, one host assembly (slice L2, #720). On-demand joins
the protocol in slice 6b. The rest is **Built**.

- **One protocol, three engines.** Each engine implements `unchanged(wanted)`,
  `clear(intent, keep)`, `install(recipe)`, `finish()`, `removeEphemerals(previews)`,
  `hasHistoryExempt()` and `exemptDependentsOf()`. The engines keep their existing method
  names; the verbs map onto them as in the table below (no shared base class: the recipe
  types differ).
  - One coordinator in `pj_runtime`, the stateless `DerivedObjects(SessionManager&,
    CatalogModel&)` built where it is needed, runs one restore ordered by dependency
    (built, slice L2): the unchanged sets upstream to downstream (filters, transforms,
    then generators), the clears downstream to upstream (generators, filters,
    transforms), the installs upstream to downstream (filters and transforms in rounds,
    a catalog rebuild so the generators resolve the outputs just created, then the
    generators), then one `markersChanged`. On-demand slots in after the generators
    (slice 6b).
  - Layout persistence lives in `pj_runtime` (`DerivedObjects`; built, slice L1):
    `save(doc, scope)` writes the `<data_processors>` block, `parse(root, intent)` reads it
    into a `DerivedSnapshot` without touching the session (the snapshot carries the
    intent, so a history parse cannot drive a layout restore), `refusal(snapshot)` is
    the history preflight and `restore(snapshot)` applies it. Diagnostics come back as
    data, the parse's first in the restore report; `MainWindow` reports them and keeps
    thin wrappers, and `applyWorkspace` parses once for the preflight and the restore. The layout format is unchanged. That is
    verified by loading layouts saved by a `release-4.1` build
    (`pj_app/tests/fixtures/layouts/release-4.1/`, `MainWindowRelease41LayoutTest`) and,
    in slice 6b, by the prototype, not only layouts generated by tests.
  - Unchanged marker generators keep their topics on restore, as transforms and on-demand
    nodes do (built, slice L2). `BatchEngine::unchangedGeneratorIds` decides per id:
    every persistent binding equals the snapshot's binding on the same dataset (kind,
    language, resolved dataset, scope, inputs, declared inputs, outputs, script and
    params ignoring line endings, window, exemption). Such a generator is not cleared:
    it keeps its object topic, its published entry (no tombstone, no republish), its run
    state (stale, pending, running, error) and its coverage. Excluded, so they run
    again: an exempt one under `kHistory` (history never touches it anyway); one reading
    a series rooted at an output the restore removes (`removalImpact`, cascade
    included) or creates, which every transform it installs does: a key it reinstalls
    under other outputs creates the new names, so a rule reading one does not keep
    markers drawn from an earlier output of that name; an all-datasets one whose last
    published run used a contributor's display offset that has moved since (its set is
    stored in the display frame, invariant 3), so a redo after a Source Timeline drag
    redraws it; an all-datasets one with a run in flight, which staged under offsets the
    restore may have moved again (re-creating it cancels the run, so a run staged at a
    dragged offset never lands after the undo that moved it back), or that never
    published (no frame to compare).
  - A snapshot entry the parse could not resolve (a dataset not loaded, an ambiguous
    one) is reported before the keep check, so a kept id does not swallow its other
    binding's diagnostic.
  - A generator that names no dataset (a generic layout strips its `dataset_id`,
    `dataset_source` and `dataset_path`, as it does a curve's) is not bound by the parse:
    its inputs may be outputs of transforms the same restore installs. The parse flags it
    (`WantedGenerator::bind_by_inputs`) and the restore binds it after the catalog
    rebuild to the one loaded dataset whose catalog carries every input literally
    (`markerSeriesField`, no qualifier parsing); none or several leave it unbound and
    reported. With no dataset to compare, its rule is never kept. Of two bindings of one
    id that resolve to the same dataset the first is created; a second with different
    persisted settings is reported, an identical one coalesces.
  - A processor output that changes marks the marker rules reading it stale (built,
    slice L2): `DataProcessorService::setOutputsChangedSink` hands the
    topic names and their dataset to `SessionManager`, which calls
    `BatchEngine::recomputeForChangedInputs(names, dataset)` (stale, then the brake in
    automatic mode; manual mode shows them stale; an all-datasets rule matches by name
    whatever the dataset). A filter output is a catalog topic like a transform's, so a
    rule can read it too. It fires on a transform upsert or in-place edit once the
    output is complete, on a filter apply or in-place edit (`updateFilter`), when a
    time-sliced replay ends (the tick or the read barrier that completes it, not the
    partial output, so a reader never forces the replay early), and before a removal
    (a transform's, a filter's own, or a cascade's), the processors chained on the
    output included. A reload's rebind (`rebindAndRecomputeForReplacedSources`) does
    not fire it: `RefillGuard::recomputeProcessors` recomputes the processors first
    and then re-runs the dataset's rules once, over the recomputed outputs. Inside a
    restore it names only outputs the restore removes or creates, which no kept rule
    reads, so nothing is suppressed: it only reaches the rules the restore does not
    decide (a preview, an exempt rule under history).

  | Verb | Filters (`DataProcessorService`) | Transforms (`DataProcessorService`) | Generators (`BatchEngine`) | Coordinator (`DerivedObjects`) |
  |---|---|---|---|---|
  | `unchanged(wanted)` | `unchangedFilterOutputs(wanted)` | `unchangedTransformKeys(wanted, intent)` | `unchangedGeneratorIds(wanted, intent, removed_upstream)` | `restore` computes the keep sets |
  | `clear(intent, keep)` | `clearAllFilters(keep_outputs)` | `clearTransforms(intent, keep_keys)` | `clearGenerators(intent, keep)` | `restore` |
  | `install(recipe)` | `makeRestoredProcessor` + `applyFilter` | `restoreTransform(recipe)` | `upsertGenerator(recipe, CreateWait)` | `restore`, in rounds for filters and transforms |
  | `finish()` | — | — | — | `CatalogModel::rebuildIfChanged` (between the transforms and the generators), then `SessionManager::notifyMarkersChanged` once |
  | `removeEphemerals(previews)` | — | `removeEphemerals(keys)` | `removeEphemerals(ids)` | `SessionManager::removeEphemerals`, from `~DerivedRuntimeHosts` with its bridges' `previews()` |
  | `hasHistoryExempt()` | — (never exempt) | `hasHistoryExemptTransforms()` | `hasHistoryExemptGenerators()` | `refusal` |
  | `exemptDependentsOf()` | `removalImpact(intent, keep_keys, keep_filter_outputs)` | same call (`exempt_transform_names`, `removed_output_names`) | `exemptDependentsOf(removed_inputs)` | `refusal` |
- **Marker topics are described by metadata** (decided 2026-10-13; built, slice M).
  - A generator's topic is registered with the plot-markers type, `pj_derived: "markers"`,
    a scope (`series`, `dataset` or `all`) and, for the series scope, the target series
    (`pj_marker_scope`, `pj_marker_target`; host constants until the host-contract minor
    gives them to the SDK). It also carries its generator's id (`pj_marker_owner`), so
    retiring a generator finds its topics without reading names.
  - The scope follows the output key (`__global__`, `__all__` or a series key), not the
    place the set is published: an all-datasets rule with a series key is a `series` topic
    on the shared home.
  - Everything that recognizes or routes marker topics reads the metadata
    (`isMarkerTopic`, `scanMarkerTopics`, `markerTopicsOf`, `publishedMarkerScopes`).
    Nothing parses names. A family lookup scans the dataset's descriptors once
    (`scanMarkerTopics`; a substring check rejects every topic without `pj_derived`
    before any parsing) and matches the scope and target in memory, not a separate
    index: an index would have to follow every removal, merge and reload of the store.
  - Names keep today's scheme, `__markers__/<key>#<generator id>`, as opaque ids, and the
    host still builds them. A direct write to a 0.35 name (`markerObjectTopicName`) gets
    its metadata stamped by `ObjectStore` on registration, the one place that reads a
    marker name: scope and target come from the name once, as for a write with no owner.
    A name whose metadata already says what it is (another builtin type, any `pj_derived`)
    is left alone. A `__markers__/` topic registered with partial marker metadata (a
    `pj_derived` value but no scope or target) is therefore neither stamped nor
    recognized; the bridge should validate the full description once the host-contract
    minor ships the keys.
  - On-demand-only logic (refill, the declared range) tests `pj_derived == "on_demand"`,
    not the key's presence, so it never touches marker topics.
  - The runtime hosts are assembled once, for the GUI and headless sessions (built,
    slice L2): `DerivedRuntimeHosts` holds the markers bridge, the transform bridge and
    the kind router over both, and both `MainWindow`'s toolbox session and
    `HeadlessDescriptorProviderSession` build theirs there, so a headless provider can
    create `kind=markers` rules too.
- **Live output.** A derived `SceneEntities` or `ImageAnnotations` topic publishes complete
  snapshots and is marked as a snapshot stream (D27).
- **Findings.** A finding is a pinned, history-exempt recipe. Its store entry sits at a
  fixed early timestamp (§9). It is re-evaluated on layout restore (D23). `dismiss` and
  `dismissAll(owner)` remove findings. A repeated question replaces its previous finding.
- **Previews** belong to the plugin instance that created them. Closing its panel, wiping
  the data or ending a headless session removes them. Undo and layout loads do not.
  A plugin session ends its previews in both engines when it ends (built, slice L2):
  closing a toolbox, a headless provider session ending, a bind failure. Each bridge
  of a `DerivedRuntimeHosts` records the previews it created and forgets one its
  plugin removes; `~DerivedRuntimeHosts` passes the rest to
  `SessionManager::removeEphemerals`, which removes those still present and still
  ephemeral and repaints once. So a headless binding of a plugin ending (a layout
  import releasing its provider session) leaves the previews of that plugin's open
  panel. Its persistent recipes are the user's data and outlive it, and so do they
  when the plugin is uninstalled (there is no per-plugin clear).
  - A persistent recipe may not read a preview's output (built, slice L2): a
    preview's removal cascades into its dependents, which would take a persistent
    transform or filter, and its recipe, along. `DataProcessorService` refuses a
    persistent transform (create, edit or restore) or a filter whose input is an
    ephemeral transform's output; another preview may read it. Marker rules need no
    rule: they read through the catalog, which lists no preview output, and a
    removal never cascades into them (a rule reading a removed series is marked
    stale and keeps its last markers).
- **History exemption.** Undo and redo have no authority over a history-exempt node.
- **Batch cancellation** (built, slice 3). A batch run in flight is cancelled, and never
  published, when its generator is upserted again, rescoped or removed, or when its
  dataset is reloaded (at the start of the refill), merged (as the anchor or merged
  away) or removed; an all-datasets run is cancelled whenever one of those happens to any
  dataset. After a merge, the anchor's bindings and every all-datasets rule are marked
  stale and re-run over the merged data.
- **Creating a marker rule** (built, slice 3; decided 2026-10-13, narrowed to layout
  loads the same day and kept 2026-10-17 after slice L2). The create compiles the script
  and stages its inputs on the GUI thread.
  The caller chooses how long the first run holds it (`BatchEngine::CreateWait`):
  - **To completion** (the default): the run computes on the GUI thread and is drawn,
    or its failure returned, when the create returns. Plugin creates through
    `MarkersRuntimeHost`, ephemeral or not, use it: released plugins read the markers
    right after `create_data_processor` returns (the anomaly detector's preview caches
    them until its input changes; the assistant reads them back), and the 0.35 contract
    says the host "compiles script, runs it over inputs". An undo or redo
    (`DerivedObjects::restore` under `kHistory`) uses it too: the user expects the
    previous markers back, not a blank set while the run finishes. With slice L2 an
    undo creates only the rules it changed, the unchanged ones stay drawn, so the
    blocking is rare.
  - **Bounded**: a layout load (`DerivedObjects::restore` under `kReplace`), including
    the progressive drain. The run computes on the pool while the GUI thread waits at
    most `BatchEngine::kCreateBudget`, 8 ms. The precedent is a transform's first
    replay slice (`kMaterializationSliceBudget`, §11): the create blocks the GUI about
    half a frame on top of staging, however long the rule runs. Only a changed or new
    rule is created at all; nothing reads its markers right away, so
    a slow one draws nothing until its run publishes. Its markers and marker-scope rows
    arrive with the later `markersPublished`, and a hidden scope saved in the layout
    stays hidden.
  - Finished within the budget: a failure is returned to the caller and nothing changes
    (the previous revision and its markers stay); a success is drawn when the create
    returns.
  - Still running: the rule is installed and the create returns. The run publishes
    through the pool like a scheduled one. Meanwhile it is visible: `isRunning`, the
    config key `computing`, and a status-bar line after 300 ms (the interim for slice 7's
    plot indicator). A failure after the budget keeps the previous markers and is
    reported, not returned: `errorOf`, the config key `error`, and once per distinct
    error a notification naming the rule and the next step ("Any previous markers are
    kept; fix the rule and apply it again.").
  - The status-bar line, the notification and the config keys apply to every
    asynchronous run: a brake or refresh re-run and a bounded layout-load create.
  - The whole-dataset recompute (reload, rollback, replace) stays synchronous: its
    callers repaint from the topics it returns, and a rollback must leave no marker
    computed from the rolled-back data. A rule it breaks is recorded like a failed
    scheduled run (`errorOf`, `error`, one notification), and stays stale.
  - Later: a non-blocking create for plugins is an opt-in flag on the v2 create (slice
    8).

## 14. Scripting languages

**Status: Built.** Python's compile-once and read-only modules are decided; Luau VM reuse is
proposed (N1, N2).

- **Luau** gets a fresh sandboxed VM per call, from bytecode compiled once per recipe
  (built, slice 5c-1: about 43 µs per call on this stack, §19). **Proposed:** one sandboxed state per pool worker plus a fresh sandboxed thread per call.
  That cuts about 86 µs of setup to about 2 µs. It needs the memory cap and instruction
  budget re-armed per call, and the random seed reset per call. Trigger: decide when the
  series slice measures per-call overhead over a long history. Before adopting it, a test
  tries to leave state behind (a global table, the string metatable) and checks that the
  next call cannot see it.
- **Python** runs on its own lane in a fresh namespace per call.
  - **Decided:** run the safety pre-check and compile once per recipe revision. That cuts
    73 µs to 1.2 µs, or 1.2 ms to 63 µs for a 43-line script.
  - **Decided:** give scripts read-only copies of the allowed modules, so a statement such
    as `math.pi = 3` cannot leak into the next call. This is a D2 fix regardless of
    performance, test-first. For scale: a one-hour series at 10 Hz is 36,000 calls, about
    45 s of rebuild overhead for the 43-line script today and about 2 s after.
  - **Open:** the trust model. The sandbox layers narrow escape hatches but are not a
    security boundary.
- **Python in the browser** is a goal (D18 with D29), gated on a spike: CPython 3.14's
  Emscripten target, statically linked, with pthreads. Until then, desktop only.

## 15. Series

**Status: Decided** (D10, N8).

- A numeric output of a function with object inputs can be a plottable series, sampled at
  the union of its object inputs' entry timestamps. An explicit clock input is the upgrade
  path, only if a strong case appears.
- It is computed on the on-demand engine, which has the right semantics: a stateless value
  per instant.
- It runs through `GridRunner` with a column sink, the same loop as window evaluations.
- It is demand-gated: it replays only while the series is displayed or explicitly
  requested, and stops otherwise. Computed rows are kept, and replay resumes.
- Live data appends. An edit or a reload replays.
- A series dropped on a plot computes the visible range first, then the rest in the
  background, with the progress bar and the stop button.
- A series not computed yet says so ("not computed", "computing 34%"). An empty series
  never appears without a reason.
- An assistant query for a value, the series or its statistics is an explicit request
  (D1), even if nothing is plotted.
- Decoding happens in the worker, with several entries batched per task.

## 16. Streaming

**Status: Built (slice 4)**, except the on-demand live edge (slice 6a).

- **Retention** is per source.
- **A poll tick runs the consumers in the streaming correctness order** (built, slice 4):
  eager processors advance, then retention trims, then the ingest is announced.
  - Processors advance over **every topic of the dataset**, not over the topics the write
    hosts committed to this tick. A changed-topic feed was built and dropped (decided with
    the slice 4 reviews): a queued tick that advanced only its own topics still trimmed every
    topic, so rows a later poll committed to another topic could be evicted unconsumed;
    rows written outside the poll's write hosts (a toolbox commits straight to the engine,
    the terminal `flushAll()` runs after the last tick) never reached it; and what it saved
    was small at the default buffer.
  - **The idle-node gate** (built) is what keeps that advance cheap. An idle node used to
    scan its inputs' retained chunks on every tick, so an advance with nothing new cost, at
    500 topics, 20 filters and 3 two-input transforms (§19), 76 µs at a 60 s buffer and
    2.7 ms at 10 min. `IncrementalEngine` now skips a node, in time linear in its inputs,
    unless an input's newest chunk id (evicted chunks included) is above the node's chunk
    watermark; a MIMO node compares each input with that input's own newest id, since its
    node-wide watermark is a maximum and a chunk sealed earlier than another input's can be
    committed after it. It is safe because input becomes new to a node only as a committed
    chunk, and every such chunk (an append, a late or out-of-order write, a cleared and
    refilled topic) is numbered from one global counter, above the watermark; retention
    raises the evicted-through id, so a gap is still seen. A node with a failure to report,
    a replay pending or a time-sliced replay unfinished is never skipped. An advance with
    nothing new now costs about 1.5 µs per tick at every buffer length.
  - The marker recompute receives every topic too: the per-topic stamp filter inside it
    (newest timestamp, row count, retention floor, series generation) is what sees a
    retention-only change on a quiet topic. That filter, not a changed set, is what ended
    the poll passing every topic to the marker recompute (slice 3).
  - The ingest announcement (`SessionManager::notifyIngest`) names every topic: the
    processors write their outputs through the engine, so no write host reports them, and a
    retention trim moves topics that received no row. It is never skipped when nothing
    changed, because its `live` flag alone drives the object-only docks.
  - While paused, writes land in the secondary engine and no processor advances. Resuming
    catches up every topic over the whole paused tail before the re-trim.
- **On-demand at the live edge** (slice 6a) needs to know which object topics changed per
  ingest tick (`onLiveIngest`). Its object-topic tracker (`takeChangedObjectTopics`) is
  carved with it, designed for that consumer.
- **The incremental engine** records a gap when retention takes input rows it never
  consumed. `TopicStorage::evictBefore` records the highest chunk id it dropped or cut rows
  from (`evictedThroughChunkId`), and a node has a gap iff that id is above its chunk
  watermark; timestamps are not compared, so an all-null column, an input without rows or
  inputs that never share a timestamp are no gap. The node then resets and replays only what
  is still retained, instead of folding rows that are gone, and keeps the last 16 gaps as
  `[feed watermark, first retained timestamp)`. They show as `gaps: [{from_ns, to_ns}]` in
  the transform's config (`from_ns` is null for a node that had fed nothing). Evicting only
  chunks the node already consumed is no gap, and a node over a file dataset never gaps,
  since nothing evicts a file topic.
  - A time-sliced node stopped inside a chunk has consumed that chunk up to its resume row:
    the chunk dropped, or the floor reaching the first unfed row, is a gap; the floor cutting
    only the fed prefix is not.
  - A node whose op failed consumes nothing, but input that arrives for it counts as
    consumed, as for a failed single-input node, or retention would pass its frozen
    watermark and read as a gap, replaying a node that fails again, on every tick.
  - A pause longer than the window is the same case: `DataEngine::flushTo` hands the paused
    tail's retention floor and evicted-through id to the primary, so a node that had not seen
    the trimmed rows records one gap on resume, replays the retained window (an integral
    restarts) and a straddling chunk's sub-floor rows stay hidden.
- **Batch** (built, slice 3) recomputes behind the reload brake, off the GUI thread, only
  for a dataset something displays (a demand-capable stream counts while a widget shows
  one of its topics). Every completed run is published, so markers keep updating while
  the stream outruns the rule. Resuming a paused stream marks its generators stale.
- **Coverage gaps** are reported in evaluation reports.

## 17. Scene integration

**Status: Built.** It changes with D30 and with findings (§9).

- **3D and 2D layers** render derived topics.
  - They read a derived topic from the store like any other topic: the newest entry at or
    before the cursor.
  - A layer's readiness gate applies to missing input and error only (D6). A pending node
    keeps showing its newest result, with the lag (D30).
  - Scene2D core stays runtime-free: a derived topic reaches it as a store topic.
  - The dock's time-range clamp and the live-edge fold skip findings (§9).
- **The lag indicator (D30)** appears on the layer row, with the number (for example
  "behind 0.4 s"), and in the scene dock. It is what makes showing an older result
  acceptable. It also tells the user that the function needs attention. "Why is it empty"
  warnings come from the node's state.
- **A dock shows one dataset.** A topic of another dataset is refused with a visible
  reason. A saved layout that mixed datasets restores the same way whatever the load order:
  the dataset of the first saved element wins. No element of a later dataset binds before
  it resolves, and the restore's last step (`IDataWidget::finishRestore`: the shell after a
  layout load, the dock after a paste) picks the winner when the first never arrived: the
  dataset the dock already shows, else the first saved dataset that is loaded. The winner's
  own pending elements stay pending (their topics may still arrive); the pending elements
  of the other saved datasets are dropped with the reason. With nothing loaded at settle,
  the first dataset to resolve afterwards wins. Elements that name no dataset (a Scene2D
  generic layer, whose live dataset the dock fills in before the shared restore) join no
  order, and a rejected paste hands the choice back with the pending queue. Known limits:
  - Two loaded datasets with the same source and path (one file loaded twice, or a single
    dataset reopened as a fan-out) are told apart only by a topic-name tiebreak, and the
    saved ids no longer match. A saved dataset whose recorded topic is in both, or in
    neither, resolves ambiguously, so the load order can still choose.
  - A dock state that names one live dataset with two different attribute sets (for
    example a stale pending element without `dataset_path` next to a live layer with it)
    is read as two saved datasets, that is, as mixed.
  - The payload sources of robot-model and trail layers (the robot-description selector,
    a pose trail's topic) stay outside the gate (GAP-47).
- **The Scene2D pairing check** (annotation `image_topic` plus cadence) applies to derived
  annotations too.
- **Scene tabs** are kinds of `pj.plot_tabs.v1` tabs. **Dialog-embedded scene views** give a
  plugin panel a live 3D or 2D view.

## 18. Plugin surfaces (SDK host contract)

**Status: Built in the SDK (#202).** It ships in the minor after the header-only 0.36.0. Its
contract decisions were settled on 2026-10-13 (§5 and below).

- Catalog snapshot v2: object topics with dataset, type, entry count and time range. The
  plugin sets `struct_size`, under the read-prefix rule. The host writes back how much it
  filled, and rejects a size below the v2 minimum. A flag to skip the scalar half is added
  only if measuring the editor's once-a-second refresh shows that it matters.
- Data processors v2: a typed request with typed outputs, a label, and INSTANT or WINDOW.
- Asynchronous evaluations: submit, poll and release, returning a JSON report.
- `pj.plot_tabs.v1` tail slots: `create_tab_v2`, `attach_topic`, `detach_topic`,
  `focus_tab`. Each slot is filled when it is implemented (not all-or-nothing), and a kind
  the host cannot serve is rejected with an error naming it. `hasSceneTabs()` is removed;
  it was never released.
- Dialog scene views: one `scene_view` key holding the kind and the topics, gated by
  `PJ_DIALOG_HOST_EMBEDS_SCENE_VIEWS`. A topic the host cannot attach is reported to the
  plugin, not only logged.
- Manifest keys: `badge`. `custom_topics_editor` is a tag in the existing `capabilities`
  array: declarative tags read without loading the plugin, distinct from the
  `capabilities()` bits in code.
- Metadata keys: `pj_snapshot`; `pj_derived`, an open value set (the key means "produced
  by a host data processor", the value names the kind); the marker keys of §13.
- Marker generators on the v2 request take a typed `scope`, instead of `"__global__"` and
  `{"scope":"all"}`. v1 keeps accepting those.
- The capability-detection rule is stated once, in `plugin_data_api.h`.

Under the ring, a derived topic exposes the results in its window to object reads at any
`t`. A finding reports its fixed store timestamp in `out_timestamp`; the real stamp is in the
payload (§9).

## 19. Measurements

These come from the prototype, whose design document describes the machine and method,
except the slice 2, slice 3, slice 5a, slice 5n, slice 5c-1, slice 6a-2 and slice 6a-3 rows. The slice 2 and 3 rows are measured on this stack by
`EngineFreezeBench` (`pj_runtime/tests/engine_freeze_bench_test.cpp`, `PJ_BENCH=1`), from
the call until the result is complete, as two different metrics:
- **GUI-thread CPU time**, as the longest single stretch (the call or one event-loop
  pass) and the total. It cannot see a GUI thread waiting on a lock or preempted.
- **Worst wall-clock stall**: the greatest lateness, beyond its 1 ms period, of a heartbeat
  timer on the GUI thread, i.e. the longest the GUI could not run a timer. For the transform
  it exceeds the CPU slice because the first event-loop pass runs a replay slice (8 ms)
  before the timer can fire, right after the 8.3 ms call.

Each value is the median of three runs, each the median of five applications; i7-13700H,
Linux, RelWithDebInfo.

| Measurement | Value |
|---|---|
| GUI freeze before slices 3-4 (slice 2): transform over 500k rows | GUI-thread CPU time: 8.3 ms max slice / 494 ms total. Worst wall-clock stall: 15.0 ms |
| GUI freeze before slices 3-4 (slice 2): marker rule over 500k rows | GUI-thread CPU time: 193 ms max slice / 193 ms total. Worst wall-clock stall: 192 ms |
| GUI freeze after slice 3: marker rule re-run after an input change (the brake, through the production resolver; `RerunMarkerRuleOverLongSeries`), 500k rows | 193 ms (synchronous before) → GUI-thread CPU time: 14.8 ms max slice / 26.1 ms total. Worst wall-clock stall: 13.8 ms |
| After slice 3, a layout load creating a marker rule over 500k rows (`upsertGenerator` with `CreateWait::kBounded`: compile and stage, then wait at most `kCreateBudget` = 8 ms; a longer run publishes from a drain, included; an undo's or redo's create and a plugin's run to completion, at the synchronous cost): the heavy rule, 10 `math.sin` per sample (`ApplyMarkerRuleOverLongSeries`, in-memory resolver, so staging is a vector copy) | Synchronous create: 197 ms max slice / 197 ms total, stall 196 ms → bounded: GUI-thread CPU time: 8.0 ms max slice / 13.8 ms total. Worst wall-clock stall: 8.6 ms |
| … the trivial threshold rule (`ApplyTrivialMarkerRuleOverLongSeries`, production resolver): the fixed part every rule pays, staging plus reading each sample once | Synchronous create: 102 ms / 102 ms, stall 101 ms → bounded: GUI-thread CPU time: 15.5 ms max slice / 25.0 ms total (the drain's publication of its ~22k markers included). Worst wall-clock stall: 22.6 ms (staging plus the budget) |
| … staging alone (`StageMarkerInputOverLongSeries`: the production resolver copies 500k rows, the script only looks the series up) | GUI-thread CPU time: 15.7 ms max slice / 15.7 ms total. Worst wall-clock stall: 14.9 ms (13.3 / 13.3 / 12.3 ms in an earlier series of runs) |
| The marker-input copy before slice 3's range cursor (`sampleAt` per row rescans from row 0) | 25k / 50k / 100k rows: 4.5 s / 17.8 s / 71 s (quadratic; about 30 min at 500k) → the cursor: 0.7 / 1.4 / 2.6 ms |
| GUI freeze: new transform over 500k rows | 1,613 ms → 8.3 ms per slice |
| On-demand evaluation, 260k-point cloud | 4-5 ms cold; under 0.01 ms warm |
| Input staging after slice 6a-3 (`InputStagerBench`, `pj_runtime/tests/input_stager_test.cpp`, `PJ_BENCH=1`; a lazily stored cloud whose fetch returns a view, median of 101 calls, three runs) | `stage` on the GUI thread: 0.10-0.23 µs for 260k points, 0.10-0.24 µs for 1 point: independent of the input's size. `decode` of the 260k cloud (4.2 MB of canonical bytes): 0.3 µs, a view over the store's bytes (170-243 µs with a copying decode), on a worker in production, where a real lazy entry's chunk decode joins it (2.2 ms for 3.2 MiB of LZ4, measured on the prototype). `stage` among 10k object topics and 10k scalar names, the node's registered last (`StageAmongTenThousandTopics`; both lookups scan): 35-36 µs, so no per-node lookup cache |
| Streaming: 5 min of 10 Hz, 2048-point clouds | Bounded plateau of entries and bytes |
| Per-call setup: Luau | about 86 µs per call (fresh VM) |
| Object scripts after slice 5c-1 (`ObjectScriptBench`, `pj_scripting/tests/object_script_test.cpp`, `PJ_BENCH=1`; three runs) | Per-call setup (fresh VM over compiled bytecode, a one-line script): 40-47 µs. Iterating a 100,000-element list field (`FrameTransforms.transforms`, reading `translation.x`): 11.0-11.5 ms (about 112 ns per element) holding the list in a local, 15.7-16.3 ms re-reading the field each iteration. The same loop with the list materialized on every element read (a one-line mutation of the binder): 18.6 s |
| Per-call setup: Python | 73 µs (4-line script) to 1.2 ms (43-line script), re-parsing every call |
| TF snapshot copy (prototype: a copy per snapshot) | 0.26 ms (5 s window) to 248 ms and 691 MB (1 h file) |
| TF snapshot after slice 5a (copy-on-write; measured on this stack by `TransformSnapshotBench`, `pj_tf/tests/tf_buffer_test.cpp`, `PJ_BENCH=1`; 25 edges at 100 Hz, every sample distinct, median of 5) | `snapshot()`: 0.02 µs and 0 bytes at any size. The next write while it is held copies the state once, on the writing thread under the buffer's write lock: live 5 s window 0.15 ms / 0.9 MB of samples, 60 s window 2.9 ms / 10.8 MB, complete 1 h file 72 ms / 648 MB (only when something writes the file buffer after a snapshot, e.g. a robot model's fixed-joint bridges); a buffer nobody writes is never copied. Consumer rule: during a dataset's TF ingest (a first file load writes the active buffer, streaming never ends) a fresh snapshot per revision makes every next write copy the growing state, quadratic over a load; take a new snapshot no faster than the brake rate there, or wait for the ingest to end |
| Native point-cloud operations (slice 5n; `PointCloudOpsBench`, `pj_scripting/tests/native_ops_test.cpp`, `PJ_BENCH=1`; median of 20 calls on a 260k-point XYZI float32 scan, flat and as 64 x 4096 with padded rows, which cost the same per point) | `filterByField` (50% kept) 3.0 ns per point, `cropBox` (30% kept) 9.5 ns, `bounds` 3.2 ns, `extremePoint` 0.9 ns, `transformXyz` 6.0 ns, a full `nextFinitePoint` iteration 6 ns. The same loops with a datatype and comparison switch per point, a filter predicate evaluated twice and a division per record run 1.25x to 5.3x slower |
| `ResultHistory` after slice 6a-2 (`ResultHistoryBench`, `pj_runtime/tests/result_history_test.cpp`, `PJ_BENCH=1`; three runs) | `lookup`, median of 1001 calls, the store's `at` per output included: 0.10-0.20 µs for a hit or a miss at 1, 100 and 10,000 results per node. 600 results of a 260k-point XYZI cloud at 10 Hz under the default limits: the ring holds 253.9 MiB (64 results); the compute cache 254 MiB with a decoded copy of each ring-held result → 11 KB without. Publication on the GUI thread (`PublishCost`, serialized there, 50 to 100 entries per topic): median 4-8 µs with a 64-byte cloud, 177-183 µs with a 1.5 MB one. One that removes nothing, at 600 to 800 retained results (`PublishCostAt600RetainedResults`): 11.8-12.7 µs with a drop predicate the store scans every entry for → 2.3 µs without one |
| Streaming poll, processors advancing over every topic with nothing new (slice 4): 500 topics, 20 single-input filters + 3 two-input transforms, one 5-row chunk per 50 ms tick on the 23 inputs; median of 400 calls, from a throwaway benchmark that is not kept | Buffer 5 s (100 chunks per input): 6.2 µs (max 11). 15 s: 12.5 µs. 60 s: 76 µs (max 89). 150 s: 215 µs. 5 min: 707 µs. 10 min (12,000 chunks): 2.7 ms (max 3.7). With the idle-node gate (§16): 1.5 µs at 100, 300, 1,200, 3,000 and 6,000 chunks (max 2.3 to 6.2) and 1.6 µs at 12,000 (max 7.2), flat |

## 20. Open items and spikes

- **Python in the browser** (§14), after the Python slice.
- **TF:** verify static frames on a real bag (R7). R6 is decided.
- **SDK:** decided to split.
  - 0.36.0, header-only, was released first (2026-10-17). `unprojectPixel` uses the full
    inverse of K (no exactness checks; it rejects only non-finite values, a singular K or a
    ray with z <= 0), and `pj_derived` is an open value set.
  - The host contract (#202) is tagged just before slice 8. Its decisions are settled (§5,
    §18).
- **Later:** move the incremental engine onto the pool; split TF ingest; an explicit series
  clock input if a strong case appears.
