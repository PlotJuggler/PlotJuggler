# Derived objects: architecture

**What this is.** The design of derived objects as built: scripts, the derived series and objects
they produce, and the plots and scene layers that show them. It spans `pj_datastore`,
`pj_runtime`, `pj_scripting`, `pj_scene_common`, `pj_scene2D`, `pj_scene3D` and `pj_app`.

**How to read it.**
- Start with [README.md](README.md) (the engines and the path from a script to a layer), then
  §1-§4, then the section for the area you change, then its header: the class comments own the
  exact rules, and this document points to them instead of restating them.
- Requirements and decision IDs (D1-D30) are in [REQUIREMENTS.md](REQUIREMENTS.md). Row IDs
  such as GAP-02 or STR-05, cited by tests, are in the archived
  [scenario catalog](../archive/derived_objects/SCENARIOS.md).
- What the code does not do, with the symptom and the upgrade path, is §20. How to change the
  common things is §21. Numbers that bound behaviour are §19.
- Section numbers are stable: code and module docs cite them as "ARCHITECTURE.md §N".

## 1. Goal

PlotJuggler derives data from scalar series (a Luau transform yields a series, a marker rule
yields a `PlotMarkers` set). Derived objects let any client of the plugin ABI declare a function
over any mix of scalar and object inputs (point clouds, transforms, scene entities, images,
annotations) and get typed results. Object outputs appear as ordinary topics, which viewers
render without knowing they are derived. The minimum walkthrough is US1 (crop a cloud, preserving
its fields) plus US5 (a measurement with a visible witness from one evaluation) (D28).

## 2. Vocabulary

- **Topic:** a named stream inside a dataset. **Entry:** one timestamped item of a topic.
- **Recipe:** a stored declaration: engine kind, language, inputs, typed outputs, parameters,
  script. **Node:** a live recipe bound to concrete inputs and outputs.
- **Bundle:** the result of one evaluation of a node: one value per declared output, produced
  together from the same resolved inputs, plus provenance.
- **Result:** a bundle together with the stamp it is shown at (the newest resolved input
  timestamp).
- **Finding:** a result the user or an agent chose to keep, pinned to one data time (D26).
- **Preview:** an ephemeral recipe owned by one plugin instance. It is never persisted.
- **Demand:** a consumer of a node's output: a displayed layer, a plotted series, an explicit
  request.

## 3. Three engines, named by trigger

The plugin-facing kinds are `transform`, `markers` and `on_demand`, with no aliases.

| Engine | Module | Kind | Triggered by | Runs over | Produces | State |
|---|---|---|---|---|---|---|
| `IncrementalEngine` | `pj_datastore` | `transform` | an entry arriving | that entry, in ascending order | one output per input entry (a history) | carried between entries |
| `BatchEngine` | `pj_runtime` | `markers` | a commit, a refresh, an explicit request | all entries of its inputs, or a window | one bundle | none (one run) |
| `OnDemandService` | `pj_runtime` | `on_demand` | a consumer asking for time `t` | the inputs resolved at `t` | one bundle for `t`, cached by dependency identity | none; required (D2) |

None can replace another:
- on-demand never builds a history;
- batch answers for a whole series;
- only on-demand evaluates at an arbitrary `t`, with no scene open (D19).

## 4. Invariants

A change to one of these rules is a decision, recorded in REQUIREMENTS.md or here.

1. **Result policy (D30): the newest completed result wins.**
   - It applies to on-demand requests from the cursor and the live edge, and to batch runs.
   - A completed valid result is published whenever it is newer than what is shown. It is
     never thrown away because a newer request exists.
   - Work is superseded only when its meaning changes: recipe, parameters, dataset epoch or
     removal. Rejected: cancelling on every new request, because a script slower than the
     request rate would then never publish.
   - Each node has one running run and at most one pending request, which keeps only the
     latest time.
   - The display only moves forward: never older than the result already shown for the
     current request. State comes from the newest request. The scene shows the newest result
     at or before the cursor, plus how far it lags behind the result the current request
     produces (D30).
   - Explicit evaluations (submit/poll) are outside this rule: each handle is its own consumer.
2. **Statelessness by isolation (D2, D29).** A function's result depends only on its resolved
   inputs, parameters and observed host services. It is enforced by isolating what a script can
   reach, not by rebuilding the interpreter for every call (§14).
3. **One dataset per node (D7).** A recipe whose inputs span datasets is rejected. Each dataset
   has its own TF buffer. A 3D dock binds to one dataset.
4. **Nothing is computed without demand (D1).**
   - On-demand evaluation runs for a displayed or requested node.
   - Batch runs for a consumed output, behind a reload brake.
   - Series replay runs while the series is displayed or explicitly requested.
5. **Missing input and errors clear the display (D6).** An unavailable output is reported per
   output (D25), separately from an empty result, a missing input and an execution error.
6. **Threading.**
   - On-demand runs, series runs and scheduled marker runs never run on the GUI thread: they
     compute on an `EnginePool` lane, over inputs the GUI thread resolved.
   - The exceptions run a script on the GUI thread:
     - filters and transforms: `IncrementalEngine` runs each node's `calculate` on the GUI
       thread, under the `DataEngine` lock (`runNodeLocked`), for Luau and Python alike; a
       Python filter takes the GIL there, unbounded (`python_engine.cpp`);
     - a marker rule's create with `BatchEngine::CreateWait::kToCompletion` (a plugin's create,
       an undo, a redo) and the whole-dataset recomputes (reload, rollback, removal), which run
       on the calling thread;
     - marker runs of a session built without a pool, which run inline.
   - The GUI thread resolves inputs and applies results, in bounded passes.
   - No on-demand, series or marker script runs while a store lock is held.
   - Python on-demand and series runs use their own single-thread lane.
7. **The cursor stays independent.** Evaluating or showing a result never moves the cursor
   (D22). An explicit-time evaluation is independent of the cursor (D19).
8. **Domain-neutral host.** Mechanisms are generic; plugins supply the domain meaning.

## 5. Recipe and bundle

The contract is part of the pinned SDK; the host serves it through the on-demand bridge (§18).

- Outputs are declared with fixed types (D8, D9): number, string, or a canonical object type.
- A script returns a table keyed by output name. Omitting a declared output is an error.
  `pj.unavailable(reason)` marks one output unavailable (D25).
- A bundle is atomic and carries provenance: the requested time, each resolved input's real
  timestamp, the output frame, and the script and parameter revisions.
- A preview evaluation that only wants values returns them as a JSON report. Nothing is
  published.

**The host contract** in the pinned SDK:
- **Poll report:** only what clients act on: `coverage.{candidates, evaluated, stopped, error,
  gaps}`, and per bundle the requested time, the resolved input times as a map, and the outputs.
  The SDK's `parseEvaluationReport()` also derives the bundle stamp (the latest resolved input
  time), so the editor and the assistant parse it one way.
- **One error shape:** `FAILED` carries the report with `stopped:"error"`. `CANCELLED` is
  reserved and never emitted; a client treats it as not-success. The host reports `FAILED` when
  the error came before any bundle, and `COMPLETED` with `stopped:"error"` after some.
- **Typed creation only:** an `on_demand` recipe is created through `create_data_processor_v2`;
  the v1 create refuses the kind.
- **Output inference:** a scripted submit with no declared outputs infers them, and lenient
  binding is the only on-demand binding. It still rejects an undeclared key and a missing
  declared output. A layout's `infer_outputs` attribute is read past.
- **A nonzero evaluation-budget `reserved`** is rejected.
- **WINDOW and INSTANT** are mutually exclusive.
- **An optional typed dataset id on the v2 request** (D7). Empty means a unique name, or an
  error. A plugin copies the id from catalog v2; layouts map dataset ids on load
  (`SessionManager::resolveDatasetIdentity`). The per-input `source:topic` qualifier stays.

## 6. Types and bindings

The Luau binder is `pj_scripting/src/object_binder.cpp`, the Python binder
`pj_scripting/src/python_object_binder.cpp`; both read and build objects through the same native
operations, and `pj_scripting/src/script_contract.h` holds what they share (which objects enter
a script and at what stamp, the output binding, the wording of errors).

- **SDK field tables** describe each builtin struct's members: name, kind, accessors. They drive
  generic binders in Luau and Python, so a script and an agent see the same fields.
- **Heavy operations are native C++ per type,** in `pj_scripting/src/native/`: filter, crop,
  transform, bounds, builders, presets, image and depth operations. They serve both languages.
- **Lazy lists:** binders read list elements one at a time through `list_at`, never
  materializing a whole list on a field access (doing so makes a loop over a list O(N²)).
- **Producible outputs:** an output declaration accepts only the object types a script can
  produce: `producible()` in `pj_scripting/src/script_contract.cpp` accepts every type with an
  SDK field table (`sdk::describe`) except `kVideoFrame` (an input only) and `kFrameTransforms`
  (§20). Today that is point cloud, scene entities, image annotations, image, depth image, camera
  info and voxel grid.
- **Cloud presets and the voxel builder (D14, D16):** `pj.cloud.new{preset, frame_id}` builds a
  cloud of PCL's point types (xyz, xyzi, xyzrgb) by name and type, in 16-byte records whose colour
  is PJ4's canonical `rgb` (R, G, B, 255 by address); `cloud:downsample(voxel_size)` keeps one
  centroid per occupied cube as an xyz cloud. `pj.voxels.new{frame_id, origin, cell_size, dims}`
  builds a dense grid of one float32 `value` channel, at most 2^24 cells, checked before anything
  is charged or allocated (`native/point_cloud_builder.h`, `native/voxel_grid_ops.h`).
- **Images and depth:** an image or depth image decodes, on the run's worker, through a headless
  codec the shell injects (`pj_scripting/include/pj_scripting/image_decoder.h`; `pj_app`'s
  `SceneImageDecoder` over Scene2D's JPEG/PNG codecs and the raw encodings, handed to
  `SessionManager::setImageDecoder`, so `pj_runtime` links no codec), into its original samples
  as floats: a uint16 depth keeps its millimetres. Without the codec, `decode` reports "no image
  decoder". The decoder decodes only a payload whose container matches its encoding, and charges
  its output, the input copy and a bound on its workspace before a codec runs. Decoded pixels are
  charged to the run's native budget; a run decodes each input once (a per-run memo in
  `ScriptMedia`, no cache across nodes). The native operations (`native/image_ops.h`,
  `native/depth_ops.h`) crop, threshold, convert to gray, measure and unproject a depth image
  through a `CameraInfo` into an organized metric cloud. Annotations made from an image input pair
  with it: its bare store topic and its own stamp on the dataset clock (0 is a stamp, which a
  merge's shift moves); annotations for no image input take the result's stamp
  (`RunOptions::result_stamp`).
- **Video:** a video input is the packet at or before the requested time (decode order);
  `video:frame_at(t)` returns the frame presented at `t` (B-frames reordered) as an rgb8 image
  charged to the native budget, through a source the shell injects
  (`pj_scripting/include/pj_scripting/video_frame_source.h`; `pj_app`'s `SceneVideoFrameSource`,
  one Scene2D streaming decoder per topic, handed to `SessionManager::setVideoFrameSource`). The
  decoder addresses packets by UID (`ObjectStore::indexOf`), so equal and late DTS never alias;
  refreshes its index from the store's revisions (`ObjectStore::contentIdentity`); indexes each
  packet at its PTS plus its entry's `payload_stamp_shift`, so `t` and the presentation time are on
  the dataset clock; honours the run's cancel check inside a GOP walk; and converts to rgb24
  through swscale outside the topic's lock. A topic's decoder is made again when its parser
  changes, and the session retires a dataset's decoders before its storage changes
  (`suspendOnDemand`); a call in flight then fails "video source generation changed".

## 7. Time and alignment

- **Inputs** are resolved independently at their closest sample at or before the evaluation
  instant (D5, D17). There is no primary clock. A string series (D4) holds its newest row, and a
  null cell there is a missing input (D6), whose time still moves the live edge; a numeric series
  skips null rows (`InputStager::sampleAt`).
- **The result's stamp** is the newest resolved input timestamp.
- **The query clock** (`pj.tf.now` and the default lookup instant) enters a result's identity
  only when the run observed it.
- **TF lookups are a zero-order hold** in the lookup's clock (entry or header, §12): the newest
  sample at or before `t`, with no interpolation.

## 8. The on-demand pipeline

`OnDemandService` (`pj_runtime/include/pj_runtime/OnDemandService.h`) is a thin facade over the
concrete classes below, with no single-implementation interfaces. It owns them and keeps only the
orchestration that spans them (install, remove, forwarding requests and state queries to
`NodeScheduler`); the pool, the in-flight budget, the TF snapshot provider, the dataset epoch, the
scalar sampler and the parser lookup are injected, and it reports through callbacks, not Qt
signals. Restore lives in `DerivedObjects` (§13).

The session owns it. `SessionManager` builds it over its pool and budget and wires the resolver
(an object topic by name in each dataset, with the builtin type from its metadata; a scalar
series, a number or a string, through `CatalogModel::findScalarForMarkerKey`, which `AppSession`
injects and the catalog memoizes until its items change; a `source:` qualifier stripped; never an
on-demand output, which the graph binds itself), the qualifier splitter (how the graph reads a
qualified upstream output: `RecipeGraph::QualifierSplitter`), the parser lookup, the dataset epochs and the TF
providers (one snapshot per `kTfSnapshotPeriod` while a dataset ingests). It turns the callbacks
into its signals (`derivedEntriesPublished`, `derivedStateChanged`, `onDemandRecipeInstalled`,
`objectTopicsChanged`).

`AppSession` requests every demanded recipe at the playhead, in its dataset's raw time. A recipe
that is not a finding is demanded while one of its object output topics is in its dataset's
active set (`TopicDemandTracker`); there is no DAG walk, since a reader's request requests its
upstream. A recipe installed or newly demanded is requested where the cursor is parked (also when
a display offset changes there), a finding at its pin once per install, and demand leaving
withdraws the node and, recursively, the upstreams nothing else needs (`OnDemandService::withdraw`,
`NodeScheduler::withdraw`).

| Component | Responsibility |
|---|---|
| `RecipeGraph` | Recipes, bindings, the DAG and cycle rejection, validation, ownership, previews, findings, dependents. Pure logic, no Qt. A node holds its script compiled in its language (`CompiledOnDemandScript`, §14). |
| `InputStager` | Resolves inputs at `t` on the GUI thread by identity (an object entry's topic, UID and parser; a scalar sample by its row, the sealed chunk's id and the row's index, so equal-time rows never alias and no value is hashed; an upstream's result) without fetching a payload, and builds the request's cache key. `decode` fetches and decodes on any thread, a parser-backed entry under its per-topic parser lock and a canonical point cloud as a view over the store's bytes, and returns the parser keepalives its objects need. |
| `ResultHistory` | Per node: the compute cache and the ring (§9). Results keyed by cache identity (§10), ordered by stamp and sized by time span: a window around the node's cursor, then one global byte budget each that evicts the result farthest from its node's cursor. |
| `NodeScheduler` | Per node: one running run plus one pending request, submission to `EnginePool`, supersede on meaning change (invariant 1), and the node state layers read (`NodeState`: idle, pending, ready, missing input, error, with the lag). Its class comment is the result-policy spec. |
| `GridRunner` | The per-instant loop shared by explicit evaluations and series (§15): a window's grid (its start and every raw input change after it, through the whole chain; a finding adds its real stamp; an instant is a window of one), the chain evaluated at each instant with the job's own upstream results, one in-flight reservation per job, budgets clamped to `kHostCaps`, and a report in the SDK's shape (`GridRunner::toJson`). Nothing published, no cursor moved. `submitRows` is the series' column sink. |

**Custom Topics.** `MainWindow::syncCustomTopicsPanel` lists the persistent recipes of every
dataset (`RecipeGraph::keysOf`), one flat row per object output (`ResultHistory::topicsOf`,
resolved to its catalog row by `CatalogModel::objectTopicKey`), with the node's state
(`NodeScheduler::NodeState`) and a finding's pin; transform outputs keep their rows. A preview is
in no tree, yet stays in the catalog (`CatalogModel::ephemeralPreviewKeys`). A state change or an
offset only refreshes, once per event-loop turn (`refreshCustomTopicsPanel`), at a cost per derived
row. Only the Custom Topics editor's rows (§18) can be edited or deleted; who may delete what is
`pj_runtime/include/pj_runtime/CustomTopicsPolicy.h`, asked again once a prompt is answered. A
derived or marker topic is no part of the data universe history is checked against on a file load
(`isOffClockTopic`), so a delete keeps its undo step across the next load.

Data flow for one cursor request:

```
cursor / live edge ──► NodeScheduler ──► InputStager (GUI: resolve by identity)
                                              │
                                   EnginePool worker: decode, run script, serialize
                                              │
                                   GUI drain ──► ResultHistory + store write ──► consumers read the store
```

**Chains.** One task per node. A request for a reader first requests its upstream at the same
time; the reader's request waits as its pending request until the upstream publishes or settles,
then stages and reads the upstream's object output from the ring (`ResultHistory::resultAt`),
decoded like any input. Each publication of an upstream serves its readers again, and they take
the result it shows rather than wait for its newest request, so a reader keeps updating while its
upstream lags (D30). An upstream that misses an input or fails is its reader's missing input, with
the reason (D25). A whole chain in one worker task is not built (§20).

## 9. Where results live

Results are written into the `ObjectStore`, and each derived topic's window is a ring.

- **Publication is never refused.** A derived topic evicts instead, so a node never goes to error
  or hides its layer because of a ceiling.
- **One global byte budget covers every derived topic** (`ResultHistory::Limits::ring_bytes`).
  When it is full, the result farthest from its node's cursor (the node's last requested time) is
  evicted, across all nodes. Each node's displayed result (the newest at or before the cursor) is
  never evicted; that is the per-node floor. The results never evicted may exceed the budget,
  reported separately.
- **The compute cache holds no decoded copy of what the ring holds,** so a large result is held
  once. A cached result keeps its number and string outputs; it keeps its decoded object outputs
  only while the ring does not hold it at its stamp (a late result, cached only).
- **The ring evicts all of a node's outputs at a stamp in one `pushOwnedBundle`** (`append =
  false` plus a `drop` predicate), so a node's outputs never disagree.
- **Five rules keep the GUI thread out of the result path** (code cites them by number):
  1. Byte estimates never serialize: inputs and outputs are sized from the decoded object
     (`estimateBytes`, for example point count times stride), and outputs are serialized on the
     worker (`ResultHistory::serializeOutputs`).
  2. A lookup checks the ring (the store) as well as the compute cache (`ResultHistory::lookup`),
     so a result already stored is never recomputed.
  3. The per-node compute cache is sized by time span, not entry count (an entry count gives no
     hits on a short jog).
  4. Publication is never refused (above).
  5. Staging resolves a lazy input entry by identity; its fetch and chunk decode run on the
     worker with the rest of the decoding (`InputStager::decode`).
- **Readers see a derived topic like any store topic:** the newest entry at or before `t`. Plugin
  reads, the catalog, drag and drop, layouts and the Custom Topics panel work unchanged.
- **The state of a node** (computing, missing input, error) comes from the node, not from the
  store. The lag is how far the entry read is behind the result the current request produces
  (D30), not the cursor minus its stamp.
- **Off the data clock:** the time-bounds scan and the refill prune skip derived topics
  (`isOffClockTopic`), or a finding at the minimum timestamp would stretch the playback range.
- **Findings (D26).**
  - A pinned node writes its single entry at a fixed early store timestamp
    (`kFindingTimestamp`). The payload keeps its real stamp, and scene placement uses it.
  - Every at-or-before read then returns the finding at any `t`, with no reader change. Plot
    markers work the same way (`Timestamp{0}`, one entry).
  - It counts against the budget and is exempt from eviction.
  - The computation-time label comes from the recipe's pinned time, not from the store.
  - A computation reads a finding only from its real stamp: an input resolved at `t` before the
    finding was computed is missing, while the display shows the finding at any instant.
  - A finding ignores a model entity's finite lifetime, and the dock's time-range clamp and the
    live-edge fold skip findings (`SceneDockWidget::clockRangeOf`). In a moving frame it stays
    where it was measured (§12, `placeObject`).
- **The declared range:** a derived layer's time range comes from its inputs' range, not from
  what has been computed (`SessionManager::derivedTopicInputRange`).

**Rejected: pull** (layers read from the evaluator, one lazy store entry per derived topic). It
needs every layer on an injected reader and the same fixed timestamp for plugin reads; with the
publication cost measured negligible (well under 1 % of GUI time at 10 Hz), it buys nothing.

## 10. Cache identity

- A result is cached under everything that determined it: the script and parameter revision; each
  resolved input's identity (UID and source generation); upstream revisions; the representation;
  and the services the run observed.
- A stored key serves a request when the base fields are equal and what its run observed (the
  query clock, the TF revision) is the same now (`CacheKey::serves`). A lookup reads only the
  request's stamp, so its cost does not grow with the results held (§19).
- **A video input** joins with its topic's content revision as well as its entry
  (`CacheKey::video_content`): what `frame_at` reads is a function of the key and of the topic's
  content, so any change of the packets is a miss for a new request and nothing is fetched or
  parsed on the GUI thread. A run's completion ignores a moved revision alone
  (`CacheKey::completes`, D30): under a live stream a paused request's run is shown instead of
  cached and run again forever.
- **The dataset epoch** moves before every change of a dataset's storage that changes what its
  inputs mean (§13), so a run staged before is never published.
- TF identity is the whole snapshot's revision, not the edges a run walked (TF R5, §12, §20).

## 11. Scheduling and execution

- **`EnginePool`** (`pj_runtime/include/pj_runtime/EnginePool.h`) runs ready tasks. It keeps no
  per-node state: no mailboxes, no coalescing, no supersede by generation.
  - A task is a `compute` (worker thread, its captured inputs only) and a `publish` (GUI thread).
  - The shared lane is a dedicated pool bounded to `max(1, ideal/2)` threads. The Python lane is
    one separate thread (`EnginePool::Lane::kPython`), since the GIL serializes Python anyway. In
    the browser both run on Web Workers.
  - A node's language picks the lane of every run of it, a cursor request's, an evaluation's
    instant and a series chunk's instant alike (`laneOf`, a free function in
    `pj_runtime/src/OnDemandRun.h`).
  - Both lanes request `QThread::LowPriority`. Qt ignores thread priorities on Linux, so there
    the bound on the shared lane, half the cores, is what keeps the GUI thread responsive.
  - `submit` returns a cancel token, which the consumer keeps to cancel the run when its meaning
    changes. Cancellation is cooperative (`compute` polls the token), and a cancelled run still
    publishes, even when its compute had finished.
  - Every run publishes exactly once with an `Outcome`: `kCompleted`, `kCancelled` (wins over
    `kFailed`) or `kFailed` (`compute` threw; the worker and the pool survive). `publish` runs some
    time after `submit`, also after a cancel, so it must not capture a raw pointer to an object
    that may be gone (`QPointer` or a weak guard).
  - Publication happens only in a GUI-thread drain, at most `kMaxPublishesPerDrain` per pass,
    then the event loop turns. A drain pops one result at a time, so a `publish` that pumps events
    sees the rest as pending, in order, and one that destroys the pool ends the drain.
  - Destroying the pool cancels every token, waits for the running computes and calls no
    `publish`.
- **The queue per node** follows invariant 1 and belongs to the consumer, not to the pool. The
  pending slot holds a request, not a task: inputs are captured on the GUI thread only when a run
  can start, so a burst of refreshes costs one capture.
  - **Batch:** per generator binding, one run in flight and at most one pending `refresh`; the
    full policy (staleness, the brake, demand, failures) is the `BatchEngine.h` class comment. The
    brake is one single-shot timer at the reload rate (`kDefaultReloadHz`; Manual mode stops it).
  - **On-demand:** per node, one run in flight and one pending request holding the latest time;
    the full policy (cancellation, history hits, chains, the order of the waiters for room) is the
    `NodeScheduler.h` class comment.
- **The in-flight memory budget** (`InFlightBudget.h`) bounds what the runs of both engines on the
  pool hold at once. A run holds a reservation from its submission to its publication:
  - an on-demand run, its VM cap plus its native budget (`InFlightBudget::kRunBytes`); its
    decoded inputs are views on the store's bytes, so they are not charged again;
  - a marker run, `kMarkerRunBytes` (its VM cap plus one output ceiling for the whole run), and
    once staged the bytes of its staged series copy. A copy that does not fit gives the room back
    and waits at the head of the line, so at most one copy waits uncharged.

  Runs that do not fit wait in ONE line for both engines (`InFlightBudget::whenRoom`, a ticket
  that leaves the line when destroyed), first in, first out: a small request never passes an
  earlier large one, and a continuously demanded node cannot starve a marker run. One larger than
  the whole budget runs alone rather than never. A release never serves the line inside the call
  that released: it posts a drain, and a grant may re-enter either engine, so a run staged
  meanwhile is checked again before it starts. Inline runs (no pool, the creates to completion,
  the whole-dataset recomputes) reserve nothing. The session owns the budget, declared before the
  pool, which it outlives.

  An explicit evaluation (`GridRunner`) holds one reservation for its whole life, so each plugin
  instance's bridge lets at most `OnDemandRuntimeHost::kMaxRunningEvaluations` evaluation into
  the runner; the others wait PENDING in the bridge's FIFO, so the playhead waits for at most one
  evaluation per plugin instance. Series chunks (§15) are `GridRunner` jobs too: at most
  `SeriesRunner::kMaxRunningChunks` in the runner session-wide, the others waiting in the
  `SeriesRunner`'s FIFO, outside the budget's line, each chunk ending on `GridRunner::kRowsMaxMillis`
  (checked between instants). So the playhead waits for at most one chunk of at most 100 ms, plus
  one evaluation per plugin instance. Both caps exist because there is no per-run reservation
  (§20).
- **Native operations release the GIL** while they work on C++ data, so a native call on the
  Python lane never blocks GUI-thread Python work. The audit guard and the allocation cap are per
  thread, and no lock is held across a call.

## 12. TF

- **`pj_tf`** is a Qt-free leaf library. `TransformBuffer` keeps a forest of edges with a
  timestamped sample history per edge and zero-order-hold lookups.
- **`TransformService`** (`pj_runtime`) owns one buffer per dataset and the ingest tap, which is
  active from startup. `SessionManager`'s remove, merge and reload paths call its life-cycle hooks.
- **Scripts read an immutable per-dataset snapshot.** `TransformService::snapshot(dataset)`
  returns a `std::shared_ptr<const pj::tf::TransformSnapshot>` with `tryLookupTransform(target,
  source, t, clock)`, `getAllFrames()` and `revision()`. It is never null, never changes, and is
  read from any thread without a lock. Calls at one revision return one object while a caller
  holds it. While a dataset ingests, on-demand runs share one snapshot per `kTfSnapshotPeriod`:
  each new snapshot makes the next write copy the buffer's state, so taking one per revision
  during a load is quadratic.
- **Copy-on-write buffer state (R4).** A buffer's state *is* a `TransformSnapshot`:
  `TransformBuffer::snapshot()` shares it in O(1), and the buffer copies it before its next write
  while a snapshot holds it. Every replacement swaps the state behind the same buffer: `clear()`
  (invalidation, removal, the merge rebuild) swaps in an empty one, and a reload commit makes the
  active buffer adopt the staging buffer's state (`adoptStateOf`, O(1)). The buffer object never
  changes, so every holder (3D docks, their layers, the robot model's fixed-joint bridges, which
  write into it) sees each replacement without rebinding; `epoch()` counts the replacements, for a
  reader that extends a cached result. Revisions come from one process-wide counter, so a revision
  names one state across buffers (the script cache key). `setCacheWindow` bumps the revision when
  it evicts.

| # | Rule | Status |
|---|---|---|
| R1 | The immutable snapshot type lives in `pj_tf` (`pj::tf::TransformSnapshot` in `tf_buffer.h`); no scripting-side interface or adapter | |
| R2 | The fixed-frame memory (a 3D-view preference) lives on the scene side (`FixedFrameMemory` in `pj_scene3d_widgets`), not in `TransformService` | |
| R3 | A 3D dock refuses a topic from another dataset (D7) | |
| R4 | A buffer's state is replaced whole, by copy-on-write: a write copies a state a snapshot shares. No seal: the robot model's bridges write into file buffers | |
| R5 | Report which edges and samples a lookup used, for the cache dependency | Skipped (measured, §20) |
| R6 | Two timestamps per sample (header and entry); see [the two clocks](#the-two-clocks-r6) | |
| R7 | Frames published once are valid before their first stamp | Not built (§20) |
| R8 | Chunked copy-on-write edge storage | Not built (§20) |
| R9 | Split the ingest pipeline out of `TransformService` | Not built (§20) |
| R10 | A sample equal to its predecessor is not stored. Equal means exactly equal, component by component: a tolerance would change the user's data. Each edge still records the newest stamp it received, so the latest sample, `latestCommonTime` and the eviction cutoff are unchanged; eviction keeps the newest sample at or before the cutoff, so a run of repeats longer than the window never loses its value | |

### The two clocks (R6)

Each TF sample keeps its header stamp and its entry time: the store timestamp of the message that
carried it, already on the dataset clock after a merge. Every lookup names its clock
(`pj::tf::Clock`). A recording's log time and header stamps differ by tens of milliseconds on
cameras and `/tf`; a lidar alone hides the mismatch.

- **Entry clock:** per edge, the newest-stamped sample that had arrived by `t` ("what the
  recording knew at t"). The cursor and query times are in this clock.
- **Header clock:** per edge, the newest sample stamped at or before `t` ("where the frame was
  when this was measured"). An object placed at its own stamp reads this clock.
- **Equal clocks:** when header == entry for every sample (simulated-time logs, bags logged with
  header stamps), both clocks agree.
- **Storage:** one deque sorted by stamp. Each sample's entry key is the earliest entry time by
  which it or a later-stamped sample had arrived (a suffix minimum), so both keys are sorted and
  one binary search serves either clock. R10 stays exact in both clocks: a skipped repeat's entry
  time is folded into the sample it repeats.
- **Eviction** drops a sample only when neither clock's live window still needs it.
  `latestCommonTime` and `chainSampleTimes` take a clock too; `getLatestSample` reports the
  header stamp.
- **Object placement** is one rule, `placeObject()`
  (`pj_scene3D/core/include/pj_scene3d_core/tf/object_placement.h`), composed through the root of
  the tree that holds both frames: `fixed <- source = (fixed <- root, entry clock, at the cursor) *
  (root <- source, header clock, at the object's stamp)`. The stamp is the object's own stamp plus
  its entry's `payload_stamp_shift`. A stamped object stays where it was captured in the world even
  when the fixed frame moves (a body frame such as `base_link`).
- **Fallback to the cursor** (`fixed <- source` in the entry clock): an unstamped object (stamp
  0), a stamp before the first TF sample on the chain, or a stamp more than `kMaxStampPastTf` past
  the newest header-clock sample on the chain (data stamped in wall time against TF in simulated
  time would otherwise freeze at one pose).
- **Placement hold:** each layer keeps the last `root <- source` it placed its object with
  (`PlacementHold`). Live retention can keep a stopped sensor's last object longer than the TF
  window keeps the transform at its stamp; the hold then keeps the object from jumping to the
  cursor. The view, the dock's scene bounds and the layer's render key all use the hold.

Every consumer and its clock:

| Consumer | Clock | Why |
|---|---|---|
| 3D view: TF triads, connection lines, hover label, frame picking, follow camera, TF render key | Entry, at the cursor | They show where each frame is at the cursor. |
| Point cloud, depth cloud, pose array and its trail, voxel grid, grid map | Header at the object's own stamp for `root <- source`; entry at the cursor for `fixed <- root` (a pose-trail point: at its own store timestamp); entry at the cursor as the fallback | An object sits where its frame was when it was measured, as in RViz. |
| Occupancy grid | Entry, at the cursor | The drawn grid is a keyframe plus later patches, with no single stamp. |
| Scene entities (markers and their models) | Entry, at the cursor; a finding (D26) at its result's real stamp, as a stamped object | `frame_locked` is not honoured (§20). |
| Robot model links | Entry, at the cursor | The links are TF frames. |
| TF frame trail | Entry (`chainSampleTimes` and its lookups) | The trail is split at the cursor and its head meets the frame's triad. |
| Camera scene bounds, layer render keys, point-cloud axis range | The layer's own placement, with its placement hold | They must match what is drawn. |
| Frame list, fixed-frame combo, orphan check | None | `getFrameHierarchy` and `areConnected` take no time. |
| Scripts (`pj.tf`) | Entry by default (no time, or a time); header for `pj.tf.lookup(target, source, object)`, at the object's own stamp | An object input's stamp enters the script on the dataset clock (`NamedValue::stamp_shift`). The fixed-joint edges a dock's robot model writes into the buffer may appear in the snapshot: they only add frames. |

## 13. Life cycle of recipes and results

**One protocol, three engines.** Each engine implements seven verbs under its existing method
names (no shared base class: the recipe types differ). One coordinator in `pj_runtime`, the
stateless `DerivedObjects(SessionManager&, CatalogModel&)` built where it is needed, runs one
restore across them.

| Verb | Filters (`DataProcessorService`) | Transforms (`DataProcessorService`) | Generators (`BatchEngine`) | On-demand (`OnDemandService`, its `RecipeGraph`) | Coordinator (`DerivedObjects`) |
|---|---|---|---|---|---|
| `unchanged(wanted)` | `unchangedFilterOutputs(wanted)` | `unchangedTransformKeys(wanted, intent)` | `unchangedGeneratorIds(wanted, intent, removed_upstream)` | `graph().unchangedRecipeKeys(wanted, intent, replaced_outputs)` | `restore` computes the keep sets |
| `clear(intent, keep)` | `clearAllFilters(keep_outputs)` | `clearTransforms(intent, keep_keys)` | `clearGenerators(intent, keep)` | `clearExcept(intent, keep)` | `restore` |
| `install(recipe)` | `makeRestoredProcessor` + `applyFilter` | `restoreTransform(recipe)` | `upsertGenerator(recipe, CreateWait)` | `install(recipe)` | `restore`, in rounds for filters and transforms |
| `finish()` | — | — | — | `sweepUnclaimed()` | `CatalogModel::rebuildIfChanged` (between the transforms and the generators), then `SessionManager::notifyMarkersChanged` once; after the on-demand step, a rebuild and `notifyObjectTopicsChanged` once |
| `removeEphemerals(previews)` | — | `removeEphemerals(keys)` | `removeEphemerals(ids)` | `graph` `removeEphemerals(keys)` | `SessionManager::removeEphemerals`, from `~DerivedRuntimeHosts` with its bridges' `previews()` |
| `hasHistoryExempt()` | — (never exempt) | `hasHistoryExemptTransforms()` | `hasHistoryExemptGenerators()` | `graph().hasHistoryExemptRecipes()` | `refusal` |
| `exemptDependentsOf()` | `removalImpact(intent, keep_keys, keep_filter_outputs)` | same call (`exempt_transform_names`, `removed_output_names`) | `exemptDependentsOf(removed_inputs)` | `graph().exemptDependentsOf(removed)` | `refusal` |

**The ordering rule.** The unchanged sets are computed upstream to downstream (filters,
transforms, generators, on-demand); the clears run downstream to upstream (on-demand, generators,
filters, transforms); the installs run upstream to downstream (filters and transforms in rounds, a
catalog rebuild so the generators resolve the outputs just created, the generators, then the
on-demand recipes, producers first). An unchanged recipe keeps its topics, published results and
run state: nothing runs and nothing is written. A generator or on-demand recipe that reads an
output the restore removes or creates is never unchanged.

Where the details live:
- **Restore and layout persistence:** the `DerivedObjects.h` comments (`save`, `parse`,
  `refusal`, `restore`; the `<data_processors>` block and its `<on_demand>` element; generators
  that name no dataset, `WantedGenerator::bind_by_inputs`; history's refusals). Diagnostics come
  back as data; `MainWindow` reports them and `applyWorkspace` parses once. The layout format is
  checked against layouts saved by a `release-4.1` build and by the prototype
  (`pj_app/tests/fixtures/layouts/`, `MainWindowRelease41LayoutTest`,
  `MainWindowPrototypeLayoutTest`).
- **Which generators stay unchanged, and when a marker run is cancelled:** the `BatchEngine.h`
  class comment and `unchangedGeneratorIds`.
- **Which processor outputs mark marker rules stale:** `DataProcessorService::setOutputsChangedSink`
  and `BatchEngine::recomputeForChangedInputs`.
- **On-demand install, clear and sweep:** `OnDemandService.h` (`install`, `clearExcept`,
  `sweepUnclaimed`, `rerun`) and `RecipeGraph.h` (`unchangedRecipeKeys`, `upsert`, which refuses
  changing the exemption of an installed key).

**Creating a marker rule** (`BatchEngine::CreateWait`). The create compiles and stages on the GUI
thread; the caller chooses how long the first run holds it. A plugin's create and an undo or redo
run it to completion (a released plugin reads the markers right after `create_data_processor`
returns; the user expects the previous markers back). A layout load uses the bounded create: the
GUI waits at most `kCreateBudget`, then the run publishes through the pool. A failure after the
budget keeps the previous markers and is reported (`errorOf`, the config key `error`, one
notification per distinct error). The whole-dataset recompute (reload, rollback, replace) stays
synchronous: a rollback must leave no marker computed from the rolled-back data.

**Marker topics are described by metadata.** A generator's topic carries the plot-markers type,
`pj_derived: "markers"`, a scope (`series`, `dataset` or `all`), for the series scope its target,
and its generator's id (`pj_marker_scope`, `pj_marker_target`, `pj_marker_owner`; host constants
in `pj_datastore/include/pj_datastore/marker_topic_metadata.hpp`). The scope follows the output
key (`__global__`, `__all__` or a series key). Everything that recognizes or routes marker topics
reads the metadata (`isMarkerTopic`, `scanMarkerTopics`, `markerTopicsOf`,
`publishedMarkerScopes`); nothing parses names. Names keep the scheme
`__markers__/<key>#<generator id>` as opaque ids; a direct write to a bare marker name
(`markerObjectTopicName`) gets its metadata stamped by `ObjectStore` on registration, the one
place that reads a marker name. On-demand-only logic (refill, the declared range) tests
`pj_derived == "on_demand"`, never the key's presence.

**The runtime hosts** are assembled once, for the GUI and headless sessions:
`DerivedRuntimeHosts` holds the markers, transform and on-demand bridges and the kind router over
them, and both `MainWindow`'s toolbox session and `HeadlessDescriptorProviderSession` build theirs
there.

**Live output.** A derived `SceneEntities` or `ImageAnnotations` topic publishes complete
snapshots and is marked as a snapshot stream (D27, `derivedTopicMetadata`).

**Findings** are pinned, history-exempt recipes, re-evaluated on layout restore (D23). A repeated
question replaces its previous finding.

**Previews** belong to the plugin instance that created them. Closing its panel, ending a headless
session or a bind failure ends them in every engine: each bridge records the previews it created,
and `~DerivedRuntimeHosts` passes the rest to `SessionManager::removeEphemerals`. Undo and layout
loads do not. Persistent recipes are the user's data and outlive their plugin, also when it is
uninstalled. A persistent transform or filter may not read a preview's output
(`DataProcessorService` refuses it): a preview's removal cascades into its dependents.

**History exemption.** Undo and redo have no authority over a history-exempt node.

**On-demand recipes through the dataset life cycle.**
- Before a reload, a merge (the anchor and every source), an eviction of object topics and a
  clear, and again when a rollback restores the data, `SessionManager::suspendOnDemand` moves the
  dataset's epoch, drops its nodes' results (before the refill detaches the data), cancels their
  runs and holds their requests (`NodeScheduler::hold`). The hold ends from the event loop
  (`resumeOnDemandLater`), after the caller rebuilt the catalog and the TF, and each node that had
  a request is requested again at its cursor.
- A removed dataset removes its recipes and retires their topics. A raw input evicted from under a
  recipe leaves it installed, missing that input by name, and the input coming back relinks it.
- A merge installs the anchor's and the consumed datasets' recipes again from the event loop
  (`rebindOnDemandAfterMerge`), producers first, re-bound on the anchor; a consumed finding's pin
  and a consumed node's cursor move by that dataset's raw shift (saturating). Derived entries are
  cleared before the object fold, never merged: the finding timestamp is a sentinel and is never
  shifted. What a merge leaves undone is in §20.

## 14. Scripting languages

- **Luau** runs on one sandboxed state per pool worker thread, made on its first run, from
  bytecode compiled once per recipe revision (`runOnDemandScript`, `pj_scripting/src/object_script.cpp`).
  - Each run gets a fresh thread on that state (`luaL_sandboxthread`: globals of its own over the
    frozen stdlib) and its own `pj` table. The random seed is reset, and the memory cap,
    instruction budget and sticky trip are re-armed.
  - A full collection ends every run, so what the run left in the VM releases its inputs, frames
    and native charges at once, and the next run's memory cap starts from the bare state. A run
    the allocator failed inside closes the state; the next run makes a new one.
  - The leak test (GAP-38) checks that a run writing globals, the stdlib tables, the string
    metatable, `_G` and `pj` leaves nothing for the next recipe run on the same thread.
- **Python** runs on its own lane in a fresh namespace per call
  (`pj_scripting/include/pj_scripting/python_object_script.h` owns the contract). The safety
  pre-check and the compile run once per revision; a run executes the code object. Scripts import
  read-only copies of the allowed modules, so `math.pi = 3` is an error and never reaches the next
  call (D2); a copy holds the module's public names only, so `statistics.sys` and `json.codecs` do
  not exist.
- **The trust model.** Python runs in-process with cooperative limits, documented as not a
  security boundary. Isolation in a separate process waits for a request.
- **Two profiles of one sandbox** (`pj_scripting/src/python_sandbox.h`). Both run every call into
  script code (a filter's teardown included) under the same limits: the watchdog
  (`BudgetLimits::python_call_millis` per filter call, `python_watchdog_millis` for a load, a create
  or an on-demand run), the allocation cap (`mem_bytes`; past it only a block of 1 KiB or more is
  refused, because CPython mishandles a refused function allocation, and a call whose charges pass
  twice the cap is stopped by a MemoryError the watchdog raises), sticky failures (a caught
  `MemoryError` or `TimeoutError` still fails the call) and the process-wide audit hook. Filters
  use the compatible profile: their real builtins and modules behind the `math`/`cmath`/`statistics`
  allowlist, no AST check, so every existing filter runs unchanged. On-demand scripts use the
  strict profile on top: an AST check (dunders, frame internals, `str.format`/`format_map`), gated
  builtins, read-only module copies and a hook that also refuses `exec` and `compile`. A copy's
  functions share the real module's globals, so a filter can patch, say, `statistics._sum`, and
  later on-demand runs call the patched code; accepted under the trust model.
- **The Python binder** reads the objects with Luau's native operations, in Python's idiom:
  0-based indices, tuples for vectors, keyword arguments for the builders, and `(value, None)` or
  `(None, reason)` where Luau returns `value` or `nil, reason` (`pj.tf.lookup`, `decode()`,
  `frame_at`); a call whose result is an output (`to_point_cloud`, `threshold`) returns a value or
  `pj.unavailable(reason)`, as in Luau. Every operation that walks the data releases the GIL. Every
  type is a plain CPython type: pybind11's dispatch dereferences an allocation the cap refuses and
  costs a type lookup per list element. A call that needs the run's limits, media or observations
  raises outside its run, and a builder outside the run that made it. An object keeps its input's
  keepalive (`NamedValue::keepalive`, the producing parser's library), so its payload never
  outlives the code that frees it.
- **In the runtime:** a recipe's `language` picks its compiler and runner in one place
  (`pj_scripting/include/pj_scripting/on_demand_script.h`): `RecipeGraph` holds one
  `CompiledOnDemandScript` per revision (Luau bytecode compiled at the upsert, or the opaque Python
  handle its first run compiles, so the graph includes no CPython), and its runs go through
  `runOnDemand` on the lane the language picks (§11). Every path accepts `python` where
  `pythonAvailable()`; a build without it refuses it with "python is not available in this build".
  A run's cancellation reaches pure Python through the watchdog, which polls the pool token's flags
  (`RunOptions::cancel_flags`) and raises KeyboardInterrupt (past `except Exception`).
- **No GUI-thread wait on Python.** The GUI never compiles Python: `compileOnDemand` keeps the
  source in the revision's handle (`deferOnDemandScriptPython`), and the revision's first run
  checks and compiles it on the Python lane, after the run's cancellation check and before its
  armed scope (whose hook refuses compiles). The handle keeps the code object, or the compile
  error, so later runs compile nothing. Consequences:
  - A create, an evaluation's bind and a restore of N recipes return at once. A node whose first
    run waits for the GIL is `kPending`.
  - A compile error is that run's error: the node settles `kError` with the compile's message, and
    the scheduler remembers it by cache key like any failed run. So a create does not refuse
    Python that does not compile (Luau still compiles at the create), and a restore never reports
    one: the error surfaces at the node's first demanded run.
  - A validate (an editor's check button) runs a throwaway compile on the Python helper thread
    (`checkOnDemandScriptPython`), waited for at most `kPythonValidateWaitMillis`; otherwise it
    answers unchecked. `check_data_processor_script` returns the tri-state (valid, invalid,
    unchecked with why), and the bool-only `validate_data_processor_script` passes an unchecked
    script; every PJ4 host vtable fills the check (`scripting::checkOnDemand`; the kind router maps
    a backend's validate when it has no check).
  - The interpreter boots at the first compile, on the thread that makes it, so a session without
    Python pays nothing. A compiled script's last release is posted to the Python helper thread. In
    the browser `pj_app/src/main.cpp` starts the helper at startup (`prestartPythonHelper()`),
    because a thread created while the main thread waits on it never starts.
  - Rejected: compiling on the helper with a completion posted back to the GUI, which needs a
    compiling state and an install path and still defers the error; a zero-wait bound with a retry
    in the browser, which keeps "Python is busy" failures.
- **Python in the browser** (D18 with D29) is CPython's Emscripten target, linked statically with
  pthreads behind `PJ_WASM_WITH_PYTHON` (on by default, `OFF` drops it). Only the on-demand lane
  runs there; Python filters are unavailable. The build, the stdlib zip policy, the symbol audit,
  the download budget and the third-party notices are in `docs/WASM_DEPLOYMENT.md`.
  `tests/wasm/python_layout.spec.js` restores a layout's Python recipes into a 3D scene in both
  browsers, and `tests/wasm/python.spec.js` holds the main-thread gap of a compile plus a validate
  under 250 ms. Allocation-heavy code (`sum(range(10**6))` in a capped strict run, the
  `PJ_WASM_PY_ALLOC` probe) costs about 122 ns per element in Chromium and 520 in Firefox, against
  30 natively (§19); what remains is the interpreter in wasm and the JS trampoline every C call
  takes, not the cap.

## 15. Series

A numeric output of a function with object inputs can be a plottable series (D10), computed by
`SeriesRunner` (`pj_runtime/include/pj_runtime/SeriesRunner.h`, owned by `OnDemandService`,
`series()`); its header owns the rules.

- **What becomes a series:** a node that is not pinned, reads an object and declares a number
  output writes one `DataEngine` topic named like its key, one float64 column per number output
  (`DataEngine::createColumnGroupTopic`). Number outputs keep their bare names; the config's
  `series` block names the topic.
- **Its clock** is the union of the entry stamps of the raw object inputs of the node and of its
  upstreams, and the real stamps of the findings it reads; a scalar side input changes values,
  never adds a row. An explicit clock input is not built (§20).
- **Chunks:** `kInstantsPerChunk` clock instants after a watermark, each one `GridRunner` job
  (`submitRows`) with one reservation in the shared line, so a cursor request runs between two
  chunks. The next chunk starts on a later event-loop turn. A missing input, a failed run, or an
  unavailable or non-finite number is a counted gap; a chunk's rows are written in one commit and
  announced at most every `kAnnounceInterval`.
- **Demand-gated (D1):** wanted while its topic is in the demand set, while a request is open
  (`request`, until it reaches the end of the clock), or while it is a preview; otherwise it stops
  and keeps its rows, and resumes from its watermark. A plugin's whole-series read
  (`read_series_arrow`, and a marker rule's input) is an explicit request through the engine's
  complete-read barrier (`SeriesRunner::requestTopic`).
- **Replays and appends:** an edit, an upstream's new revision, a re-created input and a dataset
  hold empty the topic and replay it when wanted; a live tick continues after the watermark. While
  its dataset is live (until `endLive`), a chunk's clock ends at the live edge, the oldest of the
  newest samples of the chain's raw inputs (`InputStager::laggingInputStamp`), so an input that
  lags the others appends instead of replaying the series at every entry. An object entry that
  arrives before the watermark (out of order) restarts the clock (`ObjectStore::oldestArrivalAfter`,
  `GridRunner::Rows::seen`); one at the watermark's own stamp does not, or a latched source would
  replay at every tick.
- **The visible range first:** each time plot reports where its view starts
  (`TopicDemandController` → `SessionManager::setSeriesVisibleStart`, on the display axis, the last
  plot to move per dataset wins). When a series' next chunk would start before the view does, it
  splits once per replay into [visible start, end] and a backfill up to it
  (`SeriesRunner::setVisibleStart`); the backfill's end merges them back. A plugin reads a split
  series in time order through the datastore's merged `RangeCursor`.
- **Removal and restore:** a removal retires the topic; a restore parks it, and the same key
  reinstalled with the same columns in the same dataset adopts it; a foreign topic of the key's
  name refuses the install. The series topics are derived data to a reload, the undo snapshot and
  the catalog's preview filter (`SessionManager::derivedScalarTopics`).
- **What the user sees:** every change of a series' state is reported by key
  (`SeriesRunner::Hooks::state_changed` → `SessionManager::seriesStateChanged`). A curve on an
  incomplete series reads "(not computed)" or "(computing N%)" after its legend title
  (`SeriesCurveStatus`, `PlotWidget::setSeriesStatus`, `PlotCurve::setLegendSuffix`); a live append
  keeps a complete series complete, so its legend does not blink. In the Custom Topics panel a
  persistent series is one row per number column, named `id/output`; `SeriesProgressController`
  shows a replay as a cancellable bar whose stop is `SeriesRunner::stop`, keeping the rows written.
- A Python series computes on the Python lane.

## 16. Streaming

- **Retention** is per source.
- **A poll tick runs the consumers in the streaming correctness order:** eager processors advance,
  then retention trims, then the ingest is announced.
  - Processors advance over **every topic of the dataset**, not over the topics committed this
    tick. Rejected: a changed-topic feed, because a queued tick that advanced only its own topics
    still trimmed every topic, so rows a later poll committed to another topic could be evicted
    unconsumed, and rows written outside the poll's write hosts (a toolbox committing straight to
    the engine, the terminal `flushAll()`) never reached it.
  - **The idle-node gate** keeps that advance cheap: `IncrementalEngine` skips a node, in time
    linear in its inputs, unless an input's newest chunk id (evicted chunks included) is above the
    node's chunk watermark; a MIMO node compares each input with that input's own newest id. It is
    safe because input becomes new to a node only as a committed chunk numbered from one global
    counter, and retention raises the evicted-through id, so a gap is still seen. A node with a
    failure to report, a replay pending or a time-sliced replay unfinished is never skipped. An
    advance with nothing new costs about 1.5 µs per tick at every buffer length (500 topics, 20
    filters, 3 two-input transforms; without the gate it grew to 2.7 ms at a 10 min buffer).
  - The marker recompute receives every topic too: its per-topic stamp filter (newest timestamp,
    row count, retention floor, series generation) sees a retention-only change on a quiet topic.
  - The ingest announcement (`SessionManager::notifyIngest`) names every topic and is never
    skipped, because its `live` flag alone drives the object-only docks.
  - While paused, writes land in the secondary engine and no processor advances. Resuming catches
    up every topic over the whole paused tail before the re-trim.
- **On-demand at the live edge.** A live announcement first runs `OnDemandService::followLiveEdge`
  for each dataset it names (the datasets of its ids, and the one a streaming tick names, since a
  dataset of object topics only lists no ids). A demanded node is requested at its live edge
  (`liveEdgeOf`: the newest stamp among its raw inputs, its upstreams' included, through
  `InputStager::newestInputStamp`), a finding at its pin.
  - A node is requested only when what the request would read differs from its last live request:
    the time, the epoch, the revisions and raw input identities of the node and of every upstream
    and, off a pin, the TF revision. So a tick with nothing new requests nothing, and a same-stamp
    replacement is still seen.
  - A finding runs again only when an input arrives at or before its pin. A finding whose inputs
    left the window keeps its result (`NodeScheduler::settleMissing`); a failing or removed
    upstream, a new revision or a new epoch still clear it (D6).
  - While the transport follows the live edge, the live pass owns the requests of every dataset
    with a recent live announcement (`SessionManager::isLive`) and the playhead requests none of
    them. When the transport starts following again, each demanded node is requested at its live
    edge at once (`OnDemandService::restartLiveEdge`).
  - The stager remembers each object input's topic id and checks it against the store's id index
    (`ObjectStore::isTopic`), so neither the live check nor staging scans topics by name.
  - No changed-object feed: the store is inspected after the announcement, which also sees what no
    write host reports (a toolbox writing straight into the store, the terminal flush).
  - A displayed derived topic keeps its raw inputs in the demand tracker's derived-input tier
    (`TopicDemandTracker::setDerivedInputTopics`), so a demand-capable stream sends them while only
    the derived topic is displayed.
- **The incremental engine records a gap** when retention takes input rows it never consumed.
  `TopicStorage::evictBefore` records the highest chunk id it dropped or cut rows from
  (`evictedThroughChunkId`); a node has a gap iff that id is above its chunk watermark. The node
  then resets and replays only what is still retained, and keeps the last 16 gaps, shown as `gaps:
  [{from_ns, to_ns}]` in the transform's config. A time-sliced node stopped inside a chunk, a node
  whose op failed (input arriving for it counts as consumed) and a pause longer than the window
  (`DataEngine::flushTo` hands the paused tail's floor to the primary) follow the same rule; the
  `IncrementalEngine.h` comments own the details.
- **Batch** recomputes behind the reload brake, off the GUI thread, only for a dataset something
  displays. Every completed run is published, so markers keep updating while the stream outruns
  the rule.
- **Coverage gaps** are reported in evaluation reports.

## 17. Scene integration

- **3D and 2D layers** read a derived topic from the store like any other topic: the newest entry
  at or before the cursor. There is no readiness gate: a pending node's ring keeps its newest
  result, and a missing input or an error writes an empty tombstone (D6), also where an edit or a
  dataset epoch dropped the results a layer still draws (`ResultHistory::resultsDropped`).
  - An empty point cloud is a valid empty sample, drawn as nothing with no warning (D25); an empty
    image, color or depth, is an explicit clear.
  - With nothing at or before the cursor, a layer clears when the cursor is before the entry it
    shows, and keeps that entry when it is not (the ring evicted it under the cursor).
  - A result landing at the dock's time re-runs the layers on that topic
    (`derivedEntriesPublished`, `ISceneLayer::refreshAt`); a layer whose shown entry did not change
    decodes nothing.
  - A `pj_snapshot` SceneEntities topic (every derived one, D27) folds the shown entry alone.
  - Scene2D core stays runtime-free: a derived topic reaches it as a store topic.
- **What the dock shows beside the geometry** comes from the node's state
  (`derivedTopicPresentation` in `pj_scene_common/include/pj_scene_common/scene_layer.h` owns the
  wording), re-read on `derivedStateChanged`, never per frame:
  - the row warning says why a layer is empty (missing input, error): the layer's derived warning
    part (`SceneWarningParts`);
  - the row status is not a fault: "behind 0.4 s" (the scheduler's lag), "computing…" before any
    result, a finding's "computed at 12.4 s";
  - the dock's status strip shows the largest lag of its visible derived layers. The lag is per
    node: two docks at different times share the newest request's number.
- **Initial framing.** A user drop into an empty 3D view frames the first content once: the fixed
  frame becomes the layer's frame and the camera fits its bounds where they are drawn
  (`placeObject`). User camera input, a fixed-frame pick or a layout restore cancels it.
- **A dock shows one dataset.** A topic of another dataset is refused with a visible reason
  (`SceneDockWidget::takeRefusalReason`). A saved layout that mixed datasets restores the same way
  whatever the load order: the dataset of the first saved element wins, and the restore's last step
  (`IDataWidget::finishRestore`) picks the winner when the first never arrived. Its edge cases are
  in §20.
- **The Scene2D pairing check:** an annotation of an on-demand derived topic naming its image is
  drawn only over that image, within one cadence of the delivered frame's own stamp
  (`CompositeMediaSource::setLayerPairing`, wired by `Scene2DDockWidget` from the topic's
  `pj_derived` metadata; the stamp is `DecodedFrame::stamp`, the cadence looked up around
  `DecodedFrame::entry_ts`). A rejected annotation stays retained and is checked again at every
  composite; the layer row says why (`SceneWarningParts::Part::kAnnotation`). A parser's
  annotations are shown as they are.
- **Scene tabs** are kinds of `pj.plot_tabs.v1` tabs (`PlotTabsRuntimeHost` and `pj_app`'s
  `PluginPlotTabsController` over `PluginSceneViewsController`). A plugin's tabs of every kind share
  one id namespace and are routed by the kind of the live widget; a kind the shell cannot build is
  refused by name. A topic is attached through the dock family's own gate (`tryAcceptObjectTopic`),
  and a scene tab's topics are its layers plus its config topics (`SceneDockWidget::configTopics`).
  A tab the plugin or eviction empties keeps its kind (`SceneDockWidget::detachTopic`).
- **Dialog-embedded scene views** give a plugin panel a live 3D or 2D view. `pj_dialog_host` stays
  scene-library-free: a panel's `EmbeddedViewHost` (three callbacks the app supplies) creates,
  attaches to and deletes an opaque widget, and `applyWidgetData` reconciles a frame's `scene_view`
  request by difference. What it refuses reaches the plugin as `scene_view_error` once the update
  is applied. A modal sub-dialog never embeds views. In `pj_app`, `MainWindow::makeEmbeddedViewHost`
  builds the views as seeded scene docks (so an attached topic is demanded) and attaches through
  `PluginSceneViewsController::attachEmbeddedTopic`; a 2D view refuses a second background.

## 18. Plugin surfaces (SDK host contract)

The router (`DataProcessorsKindRouter.h`) registers the one `pj.data_processors.v1` service and
routes each call to the bridge that owns its kind: `MarkersRuntimeHost`, `DataProcessorsRuntimeHost`
(transforms) and `OnDemandRuntimeHost`, assembled per plugin binding by `DerivedRuntimeHosts`. The
on-demand bridge (`OnDemandRuntimeHost.h`, which owns the rules) serves the typed create (INSTANT
pins a finding), list, config with the contract's state keys, remove, validate, check, and the
evaluations over the session's `GridRunner`.

- **Previews** live under `<plugin>/~preview/<n>/<id>`, `n` unique to the bridge: no persistent
  recipe can take that key, so a layout never names a live preview, and another instance of the
  plugin neither sees nor ends it. A node is a plugin's only when its recipe's owner is
  (`RecipeGraph::upsert` refuses a key another owner holds). A scripted submit is a preview by
  definition and is bound, never installed.
- **Bounds:** params at `kMaxParamsBytes` and 32 levels (deeper would overflow the GUI stack when
  copied or dumped); `kMaxHandles` live handles and `kMaxReportBytes` of reserved reports per
  bridge; a report is serialized on a pool worker.
- **Catalog snapshot v2:** object topics with dataset, type, entry count and time range, served by
  `DatastoreToolboxHost` (`pj_datastore/include/pj_datastore/plugin_data_host.hpp`) under the SDK's
  read-prefix rule (`sdk::storeCatalogSnapshotV2`). A derived topic's count and range are what its
  ring retains now (`ObjectStore::forEachTopicRetained`).
- **Data processors v2:** a typed request with typed outputs, an optional dataset id, and INSTANT
  or WINDOW (no label). Asynchronous evaluations: submit, poll and release, returning a JSON report.
- **`pj.plot_tabs.v1` tail slots:** `create_tab_v2`, `attach_topic`, `detach_topic`, `focus_tab`.
  Each slot is filled exactly when it is implemented, and a kind the host cannot serve is rejected
  with an error naming it (§17).
- **Dialog scene views:** one `scene_view` key holding the kind and the topics, gated by
  `PJ_DIALOG_HOST_EMBEDS_SCENE_VIEWS`, which a panel announces exactly when it has an
  `EmbeddedViewHost` (`detail::announceHostInfo`). A topic the host cannot attach is reported to
  the plugin (§17).
- **Manifest keys:** `badge`, and `custom_topics_editor` as a tag in the existing `capabilities`
  array (read without loading the plugin). A row's badge and a plugin tab's watermark are the
  owner's `badge`, else its name (`ExtensionCatalogService::toolboxBadge`).
- **The Custom Topics editor:** the toolbox whose manifest lists `custom_topics_editor`
  (`ExtensionCatalogService::customTopicsEditorId`; of several, the one in use stays selected, else
  the first scanned, with a warning). The panel's "+" launches it with no initial config; the pencil
  with the row's recipe `params_json`, so the editor stores there what it needs to reopen a recipe.
  A row is editable only when its recipe's `owner_plugin` is the editor's id.
- **Metadata keys:** `pj_snapshot`; `pj_derived`, an open value set (the key means "produced by a
  host data processor", the value names the kind); the marker keys of §13.
- **Capability detection,** in the DATA PROCESSORS section of the SDK's `pj_base/plugin_data_api.h`,
  by three rules, never by version: the typed tail slots by `hasTypedRequests()`, a flag bit by its
  refusal on a host that does not know it, a build-dependent backend (Python) by doing it.

A derived topic exposes the results in its window to object reads at any `t`. A finding reports
its fixed store timestamp in `out_timestamp`; the real stamp is in the payload (§9, §20).

## 19. Reference numbers

The budgets and limits that bound behaviour, and the measurements a regression would visibly
break. Benchmarks run with `PJ_BENCH=1` (the streaming plateau with `PJ_STREAM_BENCH=1`); the
measured values are from an i7-13700H, Linux, RelWithDebInfo, the median of three runs.

| What | Value | Where |
|---|---|---|
| GUI cost of an on-demand request | `requestAt` 4.8-8.0 µs cold, 0.4 µs warm over a lazily stored 260k-point cloud, independent of its size; a playhead tick through `AppSession` 6.9-14.1 µs cold, 0.8 µs warm | `NodeSchedulerBench.RequestAtGuiCostFor260kCloud`, `EngineFreezeBench.OnDemandColdAndWarm` |
| GUI wait of a bounded marker create, and of a transform's first replay slice | 8 ms each (`BatchEngine::kCreateBudget`, `DataProcessorService::kMaterializationSliceBudget`) | |
| GUI freeze over 500k rows | a transform: 8.3 ms per slice (worst stall 15 ms); a layout load's marker create: 8.0 ms max slice for a heavy rule over an in-memory copy, 15.5 ms through the production resolver's staging copy; a marker re-run after an input change: 14.8 ms max slice, nearly all the resolver's staging copy | `EngineFreezeBench` (`pj_runtime/tests/engine_freeze_bench_test.cpp`) |
| Series chunk | `SeriesRunner::kInstantsPerChunk` (64) instants, ending on `GridRunner::kRowsMaxMillis` (100 ms); rows announced at most every 66 ms; at most `kMaxRunningChunks` (1) chunk in the runner | |
| Series replay GUI cost | 8-12 µs per row, longest event-loop pass about 65 µs, over 400 clouds of 24k points | `SeriesRunnerBench.ReplayOver400Clouds` |
| Result budgets | ring 256 MiB (serialized), compute cache 256 MiB (decoded), each node's window ±30 s around its cursor (`ResultHistory::Limits`) | |
| Run memory | VM cap 64 MiB (`BudgetLimits::mem_bytes`) plus native budget 64 MiB (`kDefaultNativeBudgetBytes`) = one on-demand reservation (`InFlightBudget::kRunBytes`); a marker run's output ceiling 16 MiB (`kMaxMarkerOutputBytes`) | |
| Explicit evaluations | `GridRunner::kHostCaps`: 5 s, 128 MiB, 2000 evaluations, 8 MiB report; at most 2^20 grid instants; `kMaxRunningEvaluations` (1) per bridge in the runner, `kMaxHandles` (64) handles and 64 MiB of reports per bridge | |
| GUI drain | at most 32 publications per pass (`EnginePool::kMaxPublishesPerDrain`) | |
| Batch brake | 2 Hz by default, 0.2-20 Hz (`BatchEngine::kDefaultReloadHz`) | |
| TF snapshot | period 500 ms while a dataset ingests (`SessionManager::kTfSnapshotPeriod`); `snapshot()` 0.02 µs and 0 bytes; the next write while one is held copies the state once: 0.15 ms for a live 5 s window, 72 ms / 648 MB for a 1 h file | `TransformSnapshotBench` (`pj_tf/tests/tf_buffer_test.cpp`) |
| Python limits | on-demand run and filter load 2 s, filter call 250 ms (`BudgetLimits`), allocation cap 64 MiB, validate wait 100 ms (`kPythonValidateWaitMillis`) | |
| Per-run setup | Luau 7.3 µs per trivial run (one state per worker); Python 2.8 µs (4-line script) and 15.6 µs (43-line) per call, compiled once | `ObjectScriptBench.SandboxedStateShareOfATrivialRun`, `PythonObjectScriptBench.PerCallSetup` |
| Binder per element | a list element's `translation.x`: Luau about 112-119 ns, Python about 77 ns; `xyz_points()` 32 ns (Luau) and 121 ns (Python) per point | `ObjectScriptBench.BinderPerElementCost`, `PythonObjectScriptBench.ListIterationAndXyzPoints` |
| Python allocation cap | `sum(range(n))` 30.1 ns per element capped against 20.4 bare (1.5x); the browser figures are in §14 | `PythonSandboxBench.AllocationCapOverhead` |
| Native point-cloud operations | per point over a 260k-point scan: filter 3.0 ns, crop 9.5 ns, transform 6.0 ns, `downsample` 61 ns | `PointCloudOpsBench` (`pj_scripting/tests/native_ops_test.cpp`) |
| Streaming plateau | 5 min of 10 Hz clouds through the live edge: 51 raw and 301 derived entries, a 9.9 MB ring, RSS flat (+0.8 MB from minute 1 to 5) | `DerivedStreamingBench.FiveMinuteRetentionAndMemoryPlateau` (`pj_runtime/tests/on_demand_live_edge_test.cpp`) |
| 2-hop chain at 10 Hz | 6.8-6.9 ms from the reader's request to its result (two Luau crops of 260k points), of which scheduling 0.17-0.20 ms | `NodeSchedulerBench.TwoHopChainLatencyAt10Hz` |

## 20. Known limits and upgrade paths

Each item: the limit, what a user or a test sees, and what would lift it.

**Engines and scheduling**
- **The incremental engine runs on the GUI thread**, under the `DataEngine` lock, in
  `kMaterializationSliceBudget` slices for a replay. Symptom: a slow transform or filter costs the
  GUI up to a slice per event-loop pass (worst stall about 15 ms over 500k rows). Upgrade: move it
  onto the pool, after measuring a case that needs it.
- **Python filters take the GIL on the GUI or commit thread without a bound**
  (`python_engine.cpp`: inspect, create, `calculate`, teardown). Symptom: a Python on-demand run
  holding the GIL delays a filter call on the GUI thread, and the reverse. Upgrade: route filter
  calls through the Python helper thread.
- **No per-run memory reservation.** An evaluation and a series chunk hold one reservation for
  their whole life, so `kMaxRunningEvaluations` = 1 and `kMaxRunningChunks` = 1 cap them. Symptom:
  series never compute in parallel, even on idle pool threads, and several plugins' evaluations
  still add up in the budget's line. Upgrade: reserve per run in `GridRunner`, which lifts both caps.
- **A Python run waiting for its one thread holds its in-flight reservation.** Symptom: a burst of
  Python requests keeps Luau runs behind it in the line. Upgrade: a reservation per lane.
- **A script stuck in a builtin C loop** (`sum(range(10**11))`) holds the GIL until it ends; neither
  the watchdog nor a cancellation can interrupt it. Symptom: every Python run, first compiles
  included, waits behind it. Upgrade: out-of-process Python.
- **A chain runs one task per node**, not in one worker task. Symptom: none measured (scheduling
  is 0.17-0.20 ms of a 2-hop chain's 6.8 ms). Upgrade trigger: a measured chain latency that
  matters; the chain task must keep each intermediate result in its node's history and the store,
  and cover budget and cancellation for the whole chain.
- **`Slot::readers` is never pruned** (`NodeScheduler.h`). Symptom: an upstream still serves a node
  that stopped reading it, one wasted run before that node converges. Upgrade: drop a reader from the list once it stops reading the upstream.
- **A batch run that waits for room and is refreshed** keeps one pending request, so it may
  capture its inputs once more than needed.
- **The batch staging copy stays on the GUI thread:** 13-16 ms for a 500k-row series (§19).
  Upgrade: stage on the worker, after measuring a case that needs it.
- **A plugin's marker create runs its first run to completion on the GUI thread** (no opt-in
  non-blocking flag on the v2 create), and a v2 marker create takes no typed `scope` (the
  `{"scope":"all"}` params and `__global__` key carry it). The marker metadata keys are host
  constants (`marker_topic_metadata.hpp`), and a `__markers__/` topic registered with partial
  marker metadata is neither stamped nor recognized. Upgrade: the SDK takes the keys and the typed
  scope; the bridge then validates the full description.

**Results and identity**
- **TF identity is the snapshot revision** (TF R5 skipped). Symptom: unrelated live TF traffic
  reruns a TF-reading recipe, at most twice per second (the snapshot brake), never one that reads
  no TF (`OnDemandLiveEdgeBench.TfOnlyRerunsOnALiveSession`: 0-17 % spurious runs at ≥ 10 Hz).
  Upgrade trigger: a measured session where those reruns of an expensive recipe exceed the brake's
  rate; then record the edges and samples a run used.
- **A finding that read TF is not retried when the TF moves.** Symptom: a finding keeps the result
  computed against the TF at its install. Upgrade: compare the TF revision for findings too.
- **A video packet changed or appended during a run** shows the frame the run decoded until the
  next request. Upgrade: record the frames a run queried and validate them at completion.
- **A plugin reading a finding** through the object-read service sees the fixed timestamp in
  `out_timestamp`; the real one is in the payload. Revisit if a generic store read must report a
  finding's real time; the fallback, a datastore "latched" flag, is not honoured by `rangeByTime`
  and `maxUidAtOrBefore`, so it pushes work into the readers.
- **An outside writer on a derived topic** confuses `ResultHistory`'s ledger: a partial external
  eviction of one output leaves the survivor untracked, and an external replacement can mislead
  which entry the budget spares as displayed. No production path writes there.

**Inputs and outputs**
- **Frame-transform outputs are not producible** (`producible()` excludes `kFrameTransforms`): a
  derived TF topic would have to feed the dataset's buffer through the ingest tap. Video frames are
  inputs only.
- **A window evaluation over a raw string input is refused** naming it (GAP-54): `GridRunner`'s
  scalar change lookup reads numeric series only. An instant is evaluated, and a series computes
  (its clock never takes a side input's changes). Upgrade: read the string's row stamps.
- **A scalar upstream output is not an input** to another recipe; only object outputs are
  upstream edges.
- **A derived depth image never reaches the 3D DepthCloud:** the layer decodes through a parser
  only (`depth_cloud_layer.cpp`). A script turns depth into a cloud itself
  (`to_point_cloud`). Upgrade: let the layer decode canonical `sdk::DepthImage` entries.
- **Image scope:** a `compressedDepth` stream is unsupported ("unsupported image encoding"); an
  8-bit grey PNG decodes to 3 channels. An evaluation's chained image has no store topic, so its
  annotations carry the input name the script gave.
- **No headless media bridge:** the image and video codecs are injected by `pj_app`'s `MainWindow`
  (`SceneImageDecoder`, `SceneVideoFrameSource`); a session built without the shell has none, and
  `decode` reports "no image decoder" there. Upgrade: one media bridge both shells inject.
- **`downsample` sorts points by cube**, about 60 % of its 61 ns per point. Upgrade: a radix sort
  over the key bits the extent uses, if it shows up in a profile.
- **An explicit series clock input** is not built; the clock is the union of the object inputs'
  entries. Build it only for a strong case.

**Life cycle and merges**
- **A merge re-binds a recipe to a new output topic,** so a layer bound to the old one loses it,
  and the demand set stays keyed by the consumed dataset until a layer references the new topic.
  Symptom: the layer empties after a merge; the user re-adds it.
- **Two merges before the event loop turns** (B into A, then A into C) can re-bind a recipe with
  the first merge's shift only.
- **`replaceDataset`** (no production caller; a reload refills) erases a dataset's derived topics
  with its vanished ones.
- **A derived topic the user trashes** leaves its recipe publishing into nothing.

**Scene and TF**
- **Same-stamp TF replace:** the buffer keeps one value per stamp, so header 100 logged at 110 as
  v1 and at 300 as v2 reads v2 at 200, before v2 arrived (the arrival-order reference test pins
  this).
- **A static transform stamped before its log time** resolves in the entry clock only from its
  log time, so TF triads and cursor-placed layers can be missing at the start of a file. Upgrade:
  R7 (frames published once are valid before their first stamp), after checking a real bag.
- **R8 (chunked copy-on-write edge storage)** only if a snapshot copy is measured to matter; **R9
  (split TF ingest out of `TransformService`)** later.
- **Scene entities ignore their own stamps and `frame_locked`;** they are placed at the cursor.
- **Mixed-dataset dock restore:** two loaded datasets with the same source and path are told
  apart only by a topic-name tiebreak, so the load order can still choose; a dock state naming one
  live dataset with two attribute sets reads as mixed; the robot-description selector and a pose
  trail's topic stay outside the one-dataset gate (GAP-47).
- **The Scene2D pairing compares topics by bare name,** so where two datasets share a topic name
  an annotation can pair with the other dataset's image.
- **A playhead scrubbed back while streaming is paused:** the docks jump to the live edge each
  tick and the playhead requests at the cursor, so a node's requests alternate between the two
  (the pending slot keeps the latest).

## 21. Changing it

Each recipe lists the files, the tests and the pitfalls. Write the failing test first.

**Add a script output type.**
1. The type needs an SDK field table: `producible()` (`pj_scripting/src/script_contract.cpp`)
   accepts any type `sdk::describe` knows, except `kVideoFrame` and `kFrameTransforms`. No table
   means an SDK release first, then a pin bump.
2. A native builder in `pj_scripting/src/native/` that charges its `MemoryReservation` before it
   allocates and is spent by `finish()` (the pattern of `point_cloud_builder.h`,
   `voxel_grid_ops.h`).
3. Twin bindings in `object_binder.cpp` and `python_object_binder.cpp`, and the type in
   `visitDescribed` (`script_contract.cpp`) so an input of it can be read.
4. `estimateBytes` (`object_script.h`) must size it from the decoded object, never by serializing.
5. Its tombstone: `emptyObjectBytes` in `pj_runtime/src/ResultHistory.cpp` (an unavailable output
   writes an empty object of its type).
6. Per-type topic metadata in `derivedTopicMetadata` (`pj_runtime/include/pj_runtime/DerivedTopics.h`):
   snapshot streams, a codec tag.
7. A scene layer that accepts the type through its dock's `tryAcceptObjectTopic`, decoding
   canonical entries (a parser-only layer never sees a derived topic: §20, DepthCloud).
- Tests: `pj_scripting/tests/object_script_test.cpp` and `python_object_script_test.cpp` (both
  languages), `pj_runtime/tests/result_history_test.cpp`, the layer's dock test.

**Add a binder operation.**
- The native half in `pj_scripting/src/native/`, one kernel per type and comparison, dispatched
  once per call, never per element (`PointCloudOpsBench` in `pj_scripting/tests/native_ops_test.cpp`
  measures the per-point cost).
- The Luau half in `object_binder.cpp`, the Python half in `python_object_binder.cpp`, with shared
  wording in `script_contract.h`. An allocating operation goes through `retryAfterCollect` (Luau)
  or `released` (Python, which also drops the GIL): a failure on the native budget runs one full
  collection and retries once.
- Pitfalls: charge before allocating; read options before touching a builder (script code a
  conversion runs must not spend or free what is being read); a call that needs the run's limits
  or media must raise outside its run; Python returns `(value, None)` / `(None, reason)` where
  Luau returns `value` / `nil, reason`; Python indices are 0-based.
- Tests: both script test files, run under the sandbox limits (`BudgetLimits`).

**Debug "a layer shows nothing".** Walk the chain in order:
1. Demand: is the topic in `TopicDemandTracker`'s active set for its dataset? A recipe is requested
   only while one of its output topics is.
2. Node state: `OnDemandService::stateOfTopic` gives the `NodeScheduler::NodeState`; `kMissingInput`
   and `kError` carry the reason in `detail`.
3. Presentation: `derivedTopicPresentation` turns it into the row warning and status; a tombstone
   (empty entry) means a missing input or an error, an empty cloud is a valid result.
4. The dock may have refused the topic: `SceneDockWidget::takeRefusalReason` (another dataset, a
   type the family cannot show).
5. A Python script that does not compile installs silently; its error surfaces only at the node's
   first demanded run, as `kError`.
6. Then §20 (a derived depth image, a merge's re-bound topic).

**Add an engine or a runtime host.**
- Implement the §13 verbs and add a column to `DerivedObjects::restore`, respecting the ordering
  rule.
- Runs go on an `EnginePool` lane (`laneOf` picks it for on-demand nodes) and reserve through
  `InFlightBudget::whenRoom`, joining the one line.
- A plugin-facing kind needs a backend slot in `DataProcessorsKindRouter` and a bridge in
  `DerivedRuntimeHosts`, whose router member stays last (destroyed first: it holds pointers into
  the bridges). In `SessionManager` the budget is declared before the pool, the pool before the
  engines: a run still on the pool holds its reservation, and each engine cancels its runs before
  the pool joins.

**Change the result policy.** The `NodeScheduler.h` class comment (on-demand) and the
`BatchEngine.h` class comment (markers) are the spec; change them with the code. The pinning tests
are in `pj_runtime/tests/node_scheduler_test.cpp`: `SlowScriptStillPublishesWhileCursorMoves`
(GAP-02), `PendingSlotKeepsOnlyTheLatestTime` (GAP-03), `OlderCompletionNeverReplacesNewerResult`
(GAP-04), `TimeOnlyChangeDoesNotCancelTheRunningRun` (GAP-06), `MeaningChangeCancelsTheRunningRun`,
`MissingInputClearsEvenWithPreviousResult` (GAP-07) and `LagIsMeasuredAgainstTheCurrentRequestsTarget`;
the scene side of GAP-05 is in `pj_scene3D/widgets/tests/scene3d_dock_derived_topics_test.cpp`.
A change to the policy is a change to D30 in REQUIREMENTS.md.
