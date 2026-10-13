# Derived objects: target architecture

**What this is.** This is the design the derived-objects work is being delivered toward,
on `release-4.1`, as a stack of small pull requests (see [CARVING.md](CARVING.md)). It
folds in the decisions taken on 2026-10-10, and the prototype author's review of them on
2026-10-13. A functional prototype exists: pull requests
#697, #698 and #699, the reference implementation, at branch
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
  - **Pending SDK:** depends on the SDK 0.36.0 contract decisions.

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

**Status: Built**, with one fix decided.

- **SDK field tables** describe each builtin struct's members: name, kind, accessors. They
  drive generic binders in Luau and Python, so a script and an agent see the same fields.
- **Heavy operations are native C++ per type,** in `pj_scripting/native/`: filter, crop,
  transform, bounds, builders, presets. They serve both languages.
- **Decided fix:** binders read list elements lazily through `list_at`, instead of
  materializing whole lists on every field access. Today iterating a list is O(N²).
- **Decided fix:** an output declaration accepts only the object types the binders know:
  point cloud, scene entities, image annotations, image, depth image, camera info, video
  frame and frame transforms. Today `parseOutputSpec` accepts any builtin type, so a
  script can declare an output it can never return.

## 7. Time and alignment

**Status: Built** (D5, D17).

- **Inputs** are resolved independently at their closest sample at or before the
  evaluation instant. There is no primary clock.
- **The result's stamp** is the newest resolved input timestamp.
- **The query clock** (`pj.tf.now` and the default lookup instant) enters a result's
  identity only when the run observed it.
- **TF lookups are a zero-order hold:** the newest sample at or before `t`, with no
  interpolation.

The TF clock question is in §12.

## 8. The on-demand pipeline

**Status: Decided.** The structure is decided. Pull (§9) awaits its spike.

`OnDemandService` is a thin facade over concrete classes, with no single-implementation
interfaces. It starts with the public API that `SessionManager` and the plugin hosts use
today, and is transitional: each consumer moves to the component it needs (a layer to
`ResultHistory` through `frameAt`, the Custom Topics panel to `RecipeGraph`). The facade
keeps only the orchestration that spans components: install, remove and restore.

| Component | Responsibility |
|---|---|
| `RecipeGraph` | Recipes, bindings, the DAG and cycle rejection, validation, ownership, previews, findings, dependents. Pure logic, no Qt. |
| `InputStager` | Resolves inputs at `t` on the GUI thread and captures payload views (shared-pointer copies). It does not decode. |
| `ResultHistory` | Per node: ordered results, the cache identity, byte bounds, and the read used by consumers (§9). |
| `NodeScheduler` | Per node: one running run plus one pending request, demand, the in-flight budget, submission to `EnginePool`, supersede on meaning change (invariant 1). |
| `GridRunner` | The per-instant loop shared by window evaluations and series, with a report sink and a column sink (§15). |

Data flow for one cursor request:

```
cursor / live edge ──► NodeScheduler ──► InputStager (GUI: resolve, payload views)
                                              │
                                   EnginePool worker: decode, run script, stamp
                                              │
                                   GUI drain ──► ResultHistory.insert ──► consumers read
```

**Chains.** A downstream node takes its upstream's decoded result inline, with no round
trip through the store. **Proposed:** compute a whole chain in one worker task, instead of
staging upstream nodes synchronously on the GUI thread. If adopted, two conditions hold:
- Each intermediate result lands in its node's history, so an upstream node that is also
  displayed, or read by another node, does not run twice.
- The budget and cancellation cover the whole chain, and the report names the node where
  it stopped.

## 9. Where results live

**Status: Spike.** The target is pull with one lazy store entry. The fallback is a ring
window.

**Target (pull).**
- **History.** `ResultHistory` keeps each node's results ordered by stamp.
- **One global byte budget covers every node's results.** Today the compute cache has a
  global budget, but the store's ceilings are per topic: 55 derived topics could hold about
  7 GB. When the budget is full, the result farthest from its dataset's cursor is evicted,
  across all nodes. Nothing is refused. Each node's displayed result (the newest at or
  before the cursor) is never evicted; that is the per-node floor.
- **A pinned node holds one result.** It counts against the budget, is exempt from
  eviction, and is bounded by the per-owner pin cap.
- **One read, `frameAt(topic, t)`.**
  - It is const, runs on the GUI thread, and never starts work. Requests come from the
    follow loop and from live ingest.
  - It returns the state, the newest result at or before `t` (a shared, immutable decoded
    object), its stamp, its identity (used as render key), whether it is an exact hit (lag
    0), and the error detail.
  - Readiness, the lag indicator and warnings all come from this one call.
- **Pinned findings (D26)** are returned at any `t`, so they stay visible while scrubbing.
- **The store keeps one lazy entry per derived output topic** (`pushLazy`). The bytes are
  serialized only when a plugin reads them, then memoized.
  - Plugin reads, the catalog, drag and drop, layouts and the Custom Topics panel work
    unchanged.
  - A derived topic exposes only its current result through the object-read service.
    Other instants go through evaluations.
- **Objects are stamped once, in the worker.** Nothing is serialized on the GUI thread
  unless a plugin reads.

Both options need the **declared range:** a derived layer's time range comes from its
inputs' range, not from what has been computed.

**Fallback (ring).** Keep writing results into the store, with the window as a ring that
evicts the oldest entry instead of refusing publication, under the same global budget. This keeps the bundle
transaction, the generation, the second readiness channel and the D26 gap.

**The spike.**
- Step 0: measure today's GUI-thread publication cost.
- Then build pull in a throwaway branch: the point-cloud and scene-entities layers, one
  Scene2D source (to validate the injected reader), D26, chains.
- List every reader of the output types a script can produce (§6): the 3D and 2D layers,
  and whatever consumes a derived frame-transforms output, which the prototype publishes
  into the store.
- Pass when: the demo recipes render the same; the paint rate stays at or below 60 Hz;
  D26 holds; the GUI time per publication drops; the backward-seek hit rate is no worse; the
  net line count is negative.
- Kill when: the layer integration needs invasive changes; or the cost is negligible
  **and** D26 is accepted another way.
- **Order.** The spike is the first build-heavy work, because slice 6a builds
  `ResultHistory` one way or the other.

## 10. Cache identity

**Status: Built**, with a change proposed.

- A result is cached under everything that determined it: the script and parameter
  revision; each resolved input's identity (UID and source generation); upstream
  revisions; the representation; and the services the run observed.
- **Proposed:** one observed-dependency list per result, with clock, TF and video as
  entries, instead of key variants. The TF entry records the edges walked and the sample
  chosen on each (§12, R5), so unrelated live TF traffic no longer invalidates results.

## 11. Scheduling and execution

**Status: Built (slice 2)** (the pool) and **Decided** (the queue, worker decoding, GIL
release). The chain-in-one-task item is proposed.

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
- **The in-flight memory budget** reserves the staged inputs plus one VM ceiling per
  evaluation. A request that does not fit waits.
- **GUI-thread work still to move:**
  - input decoding moves to the worker (decided);
  - a chain runs in one worker task (proposed; decided while designing slice 6a);
  - publication serialization disappears with pull;
  - the batch staging copy, and TF snapshot copies (§12), get measured first.
- **Incremental engine.** Its per-ingest cycle runs on the GUI thread under `lockEngine`,
  and the first replay runs in 8 ms slices. Moving it to the pool is later work, after
  measurement.
- **Native operations release the GIL** while they work on C++ data (decided). Without
  this, a native call on the Python lane blocks any GUI-thread Python work.

## 12. TF

**Status: Built** (the per-dataset service). R1-R4 and R6 are decided and R10 is built
(slice T); the rest are proposed or conditional.

- **`pj_tf`** (Built, slice 1t) is a Qt-free leaf library. `TransformBuffer` keeps a forest of edges with a
  timestamped sample history per edge and zero-order-hold lookups.
- **`TransformService`** (`pj_runtime`) owns one buffer per dataset and the ingest tap,
  which is active from startup. `SessionManager`'s remove, merge and reload paths call its
  life-cycle hooks.
- **Scripts read an immutable per-dataset snapshot** through `ITransformLookupProvider`.

| # | Change | Status |
|---|---|---|
| R1 | Move the immutable snapshot type into `pj_tf`; drop the scripting-side interface and adapter | Decided |
| R2 | Move the fixed-frame memory (a 3D-view preference) out of `TransformService` to the scene side (`FixedFrameMemory` in `pj_scene3d_widgets`) | Built (slice 1t) |
| R3 | A 3D dock refuses a topic from another dataset, instead of warning | Built (slice 1t; follows D7) |
| R4 | Replace a buffer as a whole object instead of rewriting it in place; seal complete file buffers, so snapshots share them without copying | Decided |
| R5 | Report which edges and samples a lookup used, for the cache dependency | Proposed; measure after R10, which already removes most spurious revision bumps |
| R6 | TF clock contract. The buffer is keyed by header stamps, while lookups use the dataset's entry clock. Fix: two timestamps per sample (header and entry). The cursor and the query time use the entry clock; placing an object uses its own stamp in the header clock. On the nuScenes recording (log time), log time minus header stamp is 0 ms on the lidar, 0-40 ms on the front camera and -37 to +48 ms on `/tf`: the lidar hid the mismatch. | Decided 2026-10-13 (slice 1b). Test-first with a fixture whose log time differs from its header stamps; confirm visually on the nuScenes cameras. |
| R7 | Frames published once are valid before their first stamp | Verify first |
| R8 | Chunked copy-on-write edge storage | Only if measured |
| R9 | Split the ingest pipeline out of `TransformService` | Later |
| R10 | Do not store a sample equal to its predecessor. Exact under the zero-order hold; skips the revision bump unless the repeat evicts something. Equal means exactly equal, component by component (no tolerance): a tolerance would change the user's data. Each edge still records the newest stamp it received, so the latest sample, `latestCommonTime` and the eviction cutoff are unchanged. Eviction keeps the newest sample at or before the cutoff, which holds the value valid there, so a run of repeats longer than the window never loses its value. | Built (slice T) |

## 13. Life cycle of recipes and results

**Status: Decided** (one protocol). The rest is **Built**.

- **One protocol, three engines.** Each engine implements `unchanged(wanted)`,
  `clear(intent, keep)`, `install(recipe)`, `finish()`, `removeEphemeralsOf(owner)`,
  `hasHistoryExempt()` and `exemptDependentsOf()`.
  - One coordinator in `pj_runtime` (`SessionManager::derived()`) runs one restore ordered
    by dependency: transforms and filters, then generators, then on-demand.
  - Layout persistence moves out of `MainWindow` into `pj_runtime`. The layout format is
    unchanged. That is verified by loading layouts saved by a `release-4.1` build and by
    the prototype, not only layouts generated by tests.
  - Unchanged marker generators keep their topics on restore, as transforms and on-demand
    nodes already do. Their names are already stable; the churn comes from the
    clear-and-replay restore, and the protocol removes it.
- **Marker topics are described by metadata** (decided 2026-10-13, slice M).
  - A generator's topic is registered with the plot-markers type, `pj_derived: "markers"`,
    a scope (`series`, `dataset` or `all`) and, for the series scope, the target series.
  - Everything that recognizes or routes marker topics reads the metadata. Nothing parses
    names.
  - Names keep today's scheme, `__markers__/<key>#<generator id>`, as opaque ids. A direct
    write to a 0.35 name (`markerObjectTopicName`) gets its metadata stamped at
    registration, the one place that reads a marker name.
  - On-demand-only logic (refill, the declared range) tests `pj_derived == "on_demand"`,
    not the key's presence, so it never touches marker topics.
  - The runtime hosts are assembled once, for the GUI and headless sessions.
- **Live output.** A derived `SceneEntities` or `ImageAnnotations` topic publishes complete
  snapshots and is marked as a snapshot stream (D27).
- **Findings.** A finding is a pinned, history-exempt recipe. It is re-evaluated on layout
  restore (D23). `dismiss` and `dismissAll(owner)` remove findings. A repeated question
  replaces its previous finding.
- **Previews** belong to the plugin instance that created them. Closing its panel, wiping
  the data or ending a headless session removes them. Undo and layout loads do not.
- **History exemption.** Undo and redo have no authority over a history-exempt node.

## 14. Scripting languages

**Status: Built.** Python's compile-once and read-only modules are decided; Luau VM reuse is
proposed (N1, N2).

- **Luau** gets a fresh sandboxed VM per call, from bytecode compiled once per recipe.
  **Proposed:** one sandboxed state per pool worker plus a fresh sandboxed thread per call.
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

**Status: Built.**

- **Retention** is per source.
- **On-demand at the live edge** requests the newest changed-topic timestamp once per
  ingest tick (`onLiveIngest`).
- **The incremental engine** skips to the live edge when it falls behind retention, and
  records the gap.
- **Batch** recomputes behind the reload brake.
- **Coverage gaps** are reported in evaluation reports.

## 17. Scene integration

**Status: Built.** It changes with §9 and D30.

- **3D and 2D layers** render derived topics.
  - With pull, the point-cloud and scene-entities layers, the Scene2D pipeline sources and
    every other layer for a type a script can produce (§6) read through `frameAt` (one
    branch each). The separate readiness gates disappear.
  - Scene2D core stays runtime-free, through an injected reader.
- **The lag indicator (D30)** appears on the layer row, with the number (for example
  "behind 0.4 s"), and in the scene dock. It is what makes showing an older result
  acceptable. It also tells
  the user that the function needs attention. "Why is it empty" warnings come from the same
  state.
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

Under pull, a derived topic exposes only its current result through object reads (§9).

## 19. Measurements

These come from the prototype, whose design document describes the machine and method,
except the two slice 2 rows. Those are measured on this stack, before slices 3-4, by
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
| GUI freeze: new transform over 500k rows | 1,613 ms → 8.3 ms per slice |
| On-demand evaluation, 260k-point cloud | 4-5 ms cold; under 0.01 ms warm |
| Streaming: 5 min of 10 Hz, 2048-point clouds | Bounded plateau of entries and bytes |
| Per-call setup: Luau | about 86 µs per call (fresh VM) |
| Per-call setup: Python | 73 µs (4-line script) to 1.2 ms (43-line script), re-parsing every call |
| TF snapshot copy | 0.26 ms (5 s window) to 248 ms and 691 MB (1 h file) |

## 20. Open items and spikes

- **The pull spike** (§9), before slice 6a.
- **Python in the browser** (§14), after the Python slice.
- **TF:** verify static frames on a real bag (R7). R6 is decided.
- **SDK:** decided to split.
  - 0.36.0 is header-only and released first. Before its tag: `unprojectPixel` uses the
    full inverse of K (no exactness checks; it rejects only non-finite values, a singular K
    or a ray with z <= 0); `pj_derived` becomes an open value set; the header-only part
    builds without the host-contract declarations.
  - The host contract (#202) is tagged just before slice 8. Its decisions are settled (§5,
    §18).
- **Later:** move the incremental engine onto the pool; split TF ingest; an explicit series
  clock input if a strong case appears.
