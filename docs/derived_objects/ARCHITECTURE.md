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
   - The display only moves forward: never older than the result already shown for the
     current request. State comes from the newest request. The scene shows the newest
     result at or before the cursor, plus how far it lags behind the result the current
     request produces (D30).
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

**Status: Built.** The contract details are decided and land with the host-contract release;
the host serves them through the on-demand bridge (slice 8a-2b, §18).

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
  reserved and never emitted; a client treats it as not-success. The host reports `FAILED` when
  the error came before any bundle, and `COMPLETED` with `stopped:"error"` after some.
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
fixes below, and so has the Python binder (slices 10b and 10b-2,
`pj_scripting/src/python_object_binder.cpp`).

- **SDK field tables** describe each builtin struct's members: name, kind, accessors. They
  drive generic binders in Luau and Python, so a script and an agent see the same fields.
- **Heavy operations are native C++ per type,** in `pj_scripting/native/`: filter, crop,
  transform, bounds, builders, presets. They serve both languages.
- **Lazy lists (built, slice 5c-1):** binders read list elements one at a time through
  `list_at`, instead of materializing whole lists on every field access (the prototype's
  O(N²) iteration). Iterating 100,000 elements takes 11 ms in Luau, 20 ms in Python, whose
  views are plain CPython types rather than pybind11 classes (150 ms as those) (§19).
- **Producible outputs (built, slice 5c-1):** an output declaration accepts only the object
  types a script can produce: point cloud, scene entities, image annotations, image, depth
  image, camera info and, since slice 14, a voxel grid (built, never read: the SDK has no field table for it yet,
  so reading one of its fields is an error). A video frame is an input only (`frame_at`, below), frame transforms wait for a decision on
  the TF ingest tap (a derived TF topic would feed the dataset's buffer). The prototype's
  `parseOutputSpec` accepted any builtin type, so a script could declare an output it could
  never return.
- **Cloud presets and the voxel builder (built, slice 14, D14 and D16):** `pj.cloud.new{preset, frame_id}` builds a
  cloud of PCL's familiar point types (xyz, xyzi, xyzrgb) by name and type, in 16-byte records whose colour is PJ4's
  canonical `rgb` (R, G, B, 255 by address); `cloud:downsample(voxel_size)` keeps one centroid per occupied cube as an
  xyz cloud. `pj.voxels.new{frame_id, origin, cell_size, dims}` builds a dense grid of one float32 `value` channel,
  at most 2^24 cells, checked before anything is charged or allocated. Both follow the scene builder: charged before
  they allocate, spent by `finish()`, twinned in Python over the same native code
  (`native/point_cloud_builder.h`, `native/voxel_grid_ops.h`).
- **Images and depth (built, slices 9a-1 and 9a-2):** an image or depth image decodes, on the
  run's worker, through a headless codec the shell injects (`pj_scripting/image_decoder.h`;
  `pj_app`'s `SceneImageDecoder` over Scene2D's JPEG/PNG codecs and the raw encodings, handed to
  `SessionManager::setImageDecoder`, so `pj_runtime` links no codec), into its original samples
  as floats: a uint16 depth keeps its millimetres, nothing is normalized for display. Without
  the codec (a build without Scene2D), `decode` reports "no image decoder". The decoder decodes
  only a payload whose container is what its encoding says, and charges before a codec runs
  its output, the input copy and a bound on its workspace (row pointers, row buffers, a
  multi-scan JPEG's coefficients).
  Decoded pixels are copies charged to the run's native budget,
  which the in-flight reservation already covers; a run decodes each input once (a per-run
  memo, not a cache across nodes: MED-01 changed). The native operations
  (`native/image_ops.h`, `native/depth_ops.h`) crop, threshold (against the script's number as
  a double), convert to gray, measure and unproject a depth image through a `CameraInfo` into
  an organized metric cloud; an image method called on another object is a script error.
  Annotations made from an image input pair with it: its bare store topic (a qualified input
  name included) and its own stamp on the dataset clock (on an image, 0 is a stamp, which a
  merge's shift moves); annotations for no image input take the result's stamp
  (`RunOptions::result_stamp`). An input with no store topic (an evaluation's chained image) is
  named by its input name. Scope: a `compressedDepth` stream is unsupported ("unsupported image
  encoding"), and an 8-bit grey PNG decodes to 3 channels (Scene2D's PNG codec expands it to RGB;
  a 16-bit one keeps its single channel). Python scripts read them the same way (slice 10b-2).
- **Video (built, slice 9b):** a video input is the packet at or before the requested time (decode order);
  `video:frame_at(t)` returns the full frame presented at `t` (B-frames reordered), as an rgb8 image charged to the
  run's native budget, through a source the shell injects (`pj_scripting/video_frame_source.h`; `pj_app`'s
  `SceneVideoFrameSource`, one Scene2D streaming decoder per topic, handed to `SessionManager::setVideoFrameSource`).
  The decoder addresses packets by UID (`ObjectStore::indexOf`), so equal and late DTS never alias, walks a GOP by
  following its packets through a front eviction, keeps every retained packet of a PTS (the newest presents it, an
  older one again once the newer is evicted), and refreshes its index from the store's revisions
  (`ObjectStore::contentIdentity`): nothing on an unchanged poll, the new arrivals after an append, everything after a
  rewrite. It indexes each packet at its PTS plus its entry's `payload_stamp_shift`, so `t` and the presentation time
  are on the dataset clock and a merge's same-name topics never collide. Annotations made from a frame pair with the
  video's topic at its presentation time; a builder naming the topic pairs with the frames decoded so far when they
  agree, else with the packet's stamp (0 is a stamp, which a merge's shift moves). The run's cancel check reaches the
  decoder, so a cancelled run stops inside a long GOP walk instead of holding the topic, and a frame converts to rgb24
  through swscale (`convertToRgb24`, the shader's matrix within 1) outside the topic's lock. A frame is its image only while an
  anchor only that image holds lives, so a collected frame never lends its address to another image. A topic's decoder
  is made again when its parser changes, and the session retires a dataset's decoders before its storage changes
  (`suspendOnDemand`); a call in flight on a replaced or retired decoder fails "video source generation changed".

## 7. Time and alignment

**Status: Built** (D5, D17).

- **Inputs** are resolved independently at their closest sample at or before the
  evaluation instant. There is no primary clock. A string series (D4; built, slice S-str)
  holds its newest row, and a null cell there is a missing input (D6), whose time still
  moves the live edge, so the result the previous string made is cleared; a numeric series
  skips null rows (`InputStager::sampleAt`).
- **The result's stamp** is the newest resolved input timestamp.
- **The query clock** (`pj.tf.now` and the default lookup instant) enters a result's
  identity only when the run observed it.
- **TF lookups are a zero-order hold** in the lookup's clock (entry or header, §12): the
  newest sample at or before `t`, with no interpolation.

The TF clock question is in §12.

## 8. The on-demand pipeline

**Status: Decided.** Each component's row says whether it is built.

`OnDemandService` is a thin facade over concrete classes, with no single-implementation
interfaces. Built (slice 6a-4a, `pj_runtime/include/pj_runtime/OnDemandService.h`): it owns
the four components below and keeps only the orchestration that spans them, install and
remove, forwarding requests and state queries to `NodeScheduler`; the pool, the in-flight
budget, the TF snapshot provider, the dataset epoch, the scalar sampler and the parser
lookup are injected, and it reports through callbacks, not Qt signals. Restore stays in
`DerivedObjects` (slice 6b).

The session owns it (built, slice 6a-5). `SessionManager` builds it over its pool and
budget and wires the resolver (an object topic by name in each dataset, with the builtin
type from its metadata; a scalar series, a number or a string, through the catalog's lookup
(`CatalogModel::findScalarForMarkerKey` with strings, which a marker rule never reads), which
`AppSession` injects and the catalog memoizes until its items change; a `source:` qualifier stripped;
never an on-demand output, which the graph binds itself), the parser lookup, the dataset epochs and the TF providers: while a dataset
ingests, runs share one snapshot for 500 ms (§19), and the revision a request compares is
that snapshot's, read without taking one. It turns the callbacks into its signals
(`derivedEntriesPublished`, `derivedStateChanged`, `onDemandRecipeInstalled`,
`objectTopicsChanged`). `AppSession` requests every demanded recipe at the playhead, in
its dataset's raw time. A recipe that is not a finding is demanded while one of its object
output topics is in its dataset's active set (the demand tracker); there is no DAG walk,
since a reader's request requests its upstream. A recipe installed or newly demanded is
requested where the cursor is parked (also when a display offset changes there), a finding
at its pin once per install, and demand leaving withdraws the node and, recursively, the
upstreams nothing else needs (DEM-03, `NodeScheduler::withdraw`). The dataset life cycle is
§13. Each consumer moves to the
component it needs (a layer reads its content from the store and its state from
`NodeScheduler`, the Custom Topics panel moves to `RecipeGraph`).

| Component | Responsibility |
|---|---|
| `RecipeGraph` | Recipes, bindings, the DAG and cycle rejection, validation, ownership, previews, findings, dependents. Pure logic, no Qt. Built (slice 6a-1); a node holds its script compiled in its language, Luau or Python (slice 10c, §14). |
| `InputStager` | Resolves inputs at `t` on the GUI thread by identity (an object entry's topic, UID and parser, a scalar sample by its row, the sealed chunk's id and the row's index, so equal-time rows never alias and no value is hashed, a number or a string copied, an upstream's result) without fetching a payload, and builds the request's cache key; `decode` fetches and decodes them on any thread, a parser-backed entry under its per-topic parser lock and a canonical point cloud as a view over the store's bytes, and returns the parser keepalives its objects need, which `ResultHistory` keeps with a cached result. Built (slice 6a-3). |
| `ResultHistory` | Per node: the compute cache and the ring (§9). Results keyed by cache identity (§10), ordered by stamp and sized by time span: a window around the node's cursor, then one global byte budget each that evicts the result farthest from its node's cursor. A miss checks the store first (§9), and the cache keeps no decoded copy of what the ring holds. Built (slice 6a-2). |
| `NodeScheduler` | Per node: one running run plus one pending request, the in-flight budget, submission to `EnginePool`, supersede on meaning change (invariant 1), and the node state that layers read (idle, pending, ready, missing input, error, with the lag). Built (slice 6a-4a); `withdraw` when demand leaves and the hold of a dataset whose storage changes (slice 6a-5). |
| `GridRunner` | The per-instant loop shared by window evaluations and series (§15). Built for explicit evaluations (slice 8a-1a, `pj_runtime/include/pj_runtime/GridRunner.h`): a window's grid (its start, or the end of its leading retention gap, and every raw input change after it, through the whole chain; a finding adds its real stamp, an instant is a window of one), the chain evaluated at each instant with the job's own upstream results (an installed upstream served read-only from its history, a result reused while it serves the next instant), one in-flight reservation per job, and a report in the SDK's shape; nothing published, no cursor moved. Slice 8a-1b adds the budgets (time and evaluations between instants, the job's memory split between each run's VM cap and one native ledger all its runs share, so the objects a chain keeps count against its later runs; all clamped to host caps), the JSON report (`GridRunner::toJson`; its size kept as a running count, never re-serialized per bundle) and the object summaries, made on the worker. Slice 8a-2b puts it in `OnDemandService` (`evaluations()`), with the session's scalar change lookup, TF, epoch and run, and serves it to plugins through `OnDemandRuntimeHost` (§18). Slice 11a adds the column sink (`submitRows`): a series chunk, the first instants of a node's object clock after a watermark, each ending in a row of its number outputs or a counted gap (§15). That lookup reads numeric series only, so a window (not an instant) whose chain reads a raw string input is refused naming the input, until it reads the string's row stamps, its first retained row and the instant limit (GAP-54). |

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
**Built (slice 6a-4a), decided 2026-10-17: one task per node.**
A request for a reader first requests its upstream at the same time; the reader's request
waits as its pending request until the upstream publishes or settles, then stages. Each
publication of an upstream serves its readers again, and they take the result it shows rather than
wait for its newest request, which a moving cursor (or a TF revision that changes at every
request) never lets settle: a reader keeps updating while its upstream lags (D30), and
converges with it. An upstream that misses an input or fails is its reader's missing
input, with the reason (D25). Of the 2-hop latency at 10 Hz requests (§19) the scheduling
is 0.1-0.2 ms, so the upgrade, a whole chain in one worker task, waits for a case that needs
it, under two conditions:
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
  the store (invariant 1). The lag is how far the entry read is behind the result the
  current request produces (D30), not the cursor minus its stamp.
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
    fixed timestamp). The dock's time-range clamp and the live-edge fold skip findings
    (built, slice 7a: `SceneDockWidget::clockRangeOf`).
  - A finding stays where it was measured in a moving frame: its SceneEntities layer is
    placed at the result's real stamp through `placeObject` (built, slice 7a).
  - **Accepted consequence:** a plugin reading a finding through the object-read service
    sees the fixed timestamp in `out_timestamp`. The real one is in the payload.
  - **Revisit if** a generic store read must report a finding's real computation time: a
    plugin's `out_timestamp`, a TF lookup at the finding's own stamp in a moving frame, or
    lifetimes counted from the real stamp. The fallback is a datastore "latched" flag (the
    first entry is returned for earlier `t`), but `rangeByTime` and `maxUidAtOrBefore` do
    not honour it, so it pushes work into the readers.
- **The declared range:** a derived layer's time range comes from its inputs' range, not
  from what has been computed (built, slice 7a: `SessionManager::derivedTopicInputRange`,
  the envelope of the raw leaf inputs' ranges, upstream nodes followed).

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
- **A video input** joins with its topic's content revision as well as its entry (slice 9b): what a run's
  `frame_at` reads is a function of the key (its inputs, params and the observed clock and TF) and of the topic's
  content, so any change of the packets is a miss for a new request and nothing is resolved on the GUI thread (no
  packet is fetched or parsed there). A run's completion ignores a moved revision alone (`CacheKey::completes`, D30):
  under a live stream a paused request's run is shown instead of cached and run again forever. Ceiling: a packet
  changed or appended during the run shows the frame the run decoded until the next request; the upgrade is recording the frames
  a run queried and validating them at completion.
- **The dataset epoch** is the session's (slice 6a-5): it moves before every change of a
  dataset's storage that changes what its inputs mean (§13), so a run staged before is
  never published.
- **Proposed:** one observed-dependency list per result, with clock and TF as
  entries, instead of key variants (video joins the staged key, above). The TF entry records the edges walked and the sample
  chosen on each (§12, R5), so unrelated live TF traffic no longer invalidates results.

## 11. Scheduling and execution

**Status: Built (slice 2)** (the pool), **Built (slice 3)** (the batch queue per node),
**Built (slice 6a-4a)** (the on-demand queue, worker decoding, the in-flight budget),
**In review (slice 6a-4b)** (batch runs join the in-flight budget: one line for both
engines) and **Decided** (GIL release).

- **`EnginePool`** (`pj_runtime/include/pj_runtime/EnginePool.h`) runs ready tasks. It keeps
  no per-node state: no mailboxes, no coalescing, no supersede by generation.
  - A task is a `compute` (worker thread, its captured inputs only) and a `publish` (GUI
    thread).
  - The shared lane is a dedicated pool bounded to `max(1, ideal/2)` threads. The Python
    lane is one separate thread, since the GIL serializes Python anyway. In the browser
    both run on Web Workers.
  - A node's language picks the lane of every run of it, a cursor request's, an evaluation's
    instant and a series chunk's instant alike (`laneOf`, slice 10c): Python's thread for a
    Python script, the shared lane otherwise. Ceiling: a Python run waiting for its thread
    already holds its in-flight reservation, so many Python nodes requested at once keep Luau
    runs behind them in the budget's line; upgrade: a reservation per lane.
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
  - **On-demand (built, slice 6a-4a).** Per node: one run in flight and one pending request
    holding the latest time; the full policy (cancellation, history hits, chains, the
    order of the waiters for room) is the `NodeScheduler.h` class comment. Only demanded
    nodes follow the cursor (slice 6a-5); when demand leaves, the node's waiting request is
    dropped and it is idle, its running run completes into the history, and nothing starts
    after it; its upstreams follow unless demanded themselves or read by another reader that
    still has a request.
- **The in-flight memory budget** (`InFlightBudget.h`; on-demand runs since slice 6a-4a,
  batch runs since 6a-4b) bounds what the runs of both engines on the pool hold at once. A
  run holds a reservation from its submission to its publication (a cancelled one until the
  pool lets it go):
  - an on-demand run, its VM cap plus its native budget (128 MiB at the defaults); its
    decoded inputs are views on the store's bytes, so they are not charged again;
  - a marker run, its VM cap plus one 16 MiB native output ceiling for the whole run,
    however many datasets contribute (80 MiB), and once staged the bytes of its staged
    series copy (8 MB for a 500k-row series). It waits for its room before it stages; a copy
    that does not fit keeps its copy, gives the room back and waits at the head of the line
    for room for the whole run, so at most one copy waits uncharged.

  The default holds max(1, pool threads) on-demand runs. Runs that do not fit wait in ONE
  line for both engines, first in, first out, served from its head while the head fits: a
  small request never passes an earlier large one, and a continuously demanded node cannot
  starve a marker run. One larger than the whole budget runs alone rather than never. A
  release never serves the line inside the call that released: it posts a drain, and each
  publication drains once its own work is done, so a waiting run of either engine starts
  before the next publication; a grant may re-enter either engine, so a run staged meanwhile
  is checked again before it starts. A layout load's bounded create takes its room at once,
  or is installed with its first run waiting in the line like a refresh (it publishes late,
  as a slow one does); a finished create returns its room at once. Inline runs (no pool, the
  creates to completion, the whole-dataset recomputes) hold no pool thread and reserve
  nothing. The session owns the budget, declared before the pool, which it outlives.

  An explicit evaluation (`GridRunner`) holds one reservation for its whole life, so a plugin
  that submits many would queue the playhead's next request behind all of them (D30). Each
  plugin instance's bridge therefore lets at most one evaluation into the runner
  (`OnDemandRuntimeHost::kMaxRunningEvaluations`); the others wait PENDING in a FIFO of the
  bridge, so the playhead waits for at most one evaluation per plugin instance (measured in
  §19). Several plugins still add up in the line. Follow-up: reserve per run in `GridRunner`,
  which lifts both.

  Series chunks (§15) are `GridRunner` jobs too. At most one of them is in the runner at a
  time, session-wide (`SeriesRunner::kMaxRunningChunks`), a cancelled one counted until its
  run's publication lets the pool thread go; the other wanted series wait their turn in a FIFO
  of the `SeriesRunner`, outside the budget's line. A chunk's time budget is 100 ms
  (`GridRunner::kRowsMaxMillis`, checked between instants). So the playhead waits for
  ≤ one chunk ≤ 100 ms (64 instants at most, plus the run in flight when the budget ends),
  however many series compute, plus one evaluation per plugin instance. The ceiling: series never compute in
  parallel, even on idle pool threads; per-run reservations would lift it as well.
- **GUI-thread work still to move:** the batch staging copy, and TF snapshot copies (§12),
  get measured first. The batch re-run over 500k rows now costs the GUI thread 15 ms at most
  per pass (§19), nearly all of it the resolver's copy of the series (staging alone: 13-16
  ms).
- **Incremental engine.** Its per-ingest cycle runs on the GUI thread under `lockEngine`,
  and the first replay runs in 8 ms slices. Moving it to the pool is later work, after
  measurement.
- **Native operations release the GIL** while they work on C++ data (decided). Without
  this, a native call on the Python lane blocks any GUI-thread Python work. The run
  machinery allows it (slice 10a-1: the audit guard and the allocation cap are per thread, no
  lock is held across a call); each binding releasing it is slice 10b's.

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
| Scene entities (markers and their models) | Entry, at the cursor; a finding (D26) at its result's real stamp, as a stamped object | Entities carry their own stamps and `frame_locked`; honouring both is a follow-up. |
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
previews end with their owner, one host assembly (slice L2, #720); on-demand recipes
join it (slice 6b). The rest is **Built**.

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
    generators), then one `markersChanged`. On-demand recipes are the most downstream
    engine: kept last, cleared first, installed after the generators (slice 6b, below).
  - Layout persistence lives in `pj_runtime` (`DerivedObjects`; built, slice L1):
    `save(doc, scope)` writes the `<data_processors>` block, `parse(root, intent)` reads it
    into a `DerivedSnapshot` without touching the session (the snapshot carries the
    intent, so a history parse cannot drive a layout restore), `refusal(snapshot)` is
    the history preflight and `restore(snapshot)` applies it. Diagnostics come back as
    data, the parse's first in the restore report; `MainWindow` reports them and keeps
    thin wrappers, and `applyWorkspace` parses once for the preflight and the restore. The layout format is unchanged. That is
    verified by loading layouts saved by a `release-4.1` build
    (`pj_app/tests/fixtures/layouts/release-4.1/`, `MainWindowRelease41LayoutTest`) and
    by the prototype (`pj_app/tests/fixtures/layouts/prototype/`,
    `MainWindowPrototypeLayoutTest`), not only layouts generated by tests.
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

  | Verb | Filters (`DataProcessorService`) | Transforms (`DataProcessorService`) | Generators (`BatchEngine`) | On-demand (`OnDemandService`, its `RecipeGraph`) | Coordinator (`DerivedObjects`) |
  |---|---|---|---|---|---|
  | `unchanged(wanted)` | `unchangedFilterOutputs(wanted)` | `unchangedTransformKeys(wanted, intent)` | `unchangedGeneratorIds(wanted, intent, removed_upstream)` | `graph().unchangedRecipeKeys(wanted, intent, replaced_outputs)` | `restore` computes the keep sets |
  | `clear(intent, keep)` | `clearAllFilters(keep_outputs)` | `clearTransforms(intent, keep_keys)` | `clearGenerators(intent, keep)` | `clearExcept(intent, keep)` | `restore` |
  | `install(recipe)` | `makeRestoredProcessor` + `applyFilter` | `restoreTransform(recipe)` | `upsertGenerator(recipe, CreateWait)` | `install(recipe)` | `restore`, in rounds for filters and transforms |
  | `finish()` | — | — | — | `sweepUnclaimed()` | `CatalogModel::rebuildIfChanged` (between the transforms and the generators), then `SessionManager::notifyMarkersChanged` once; after the on-demand step, a rebuild and `notifyObjectTopicsChanged` once |
  | `removeEphemerals(previews)` | — | `removeEphemerals(keys)` | `removeEphemerals(ids)` | `graph` `removeEphemerals(keys)` (bridge: slice 8a-2b) | `SessionManager::removeEphemerals`, from `~DerivedRuntimeHosts` with its bridges' `previews()` |
  | `hasHistoryExempt()` | — (never exempt) | `hasHistoryExemptTransforms()` | `hasHistoryExemptGenerators()` | `graph().hasHistoryExemptRecipes()` | `refusal` |
  | `exemptDependentsOf()` | `removalImpact(intent, keep_keys, keep_filter_outputs)` | same call (`exempt_transform_names`, `removed_output_names`) | `exemptDependentsOf(removed_inputs)` | `graph().exemptDependentsOf(removed)` | `refusal` |
- **On-demand recipes in the layout** (built, slice 6b). An `<on_demand owner_plugin id
  history_exempt language query_time pinned_t_ns>` element carries `<input name
  dataset_id dataset_source>` (and `dataset_path`, stamped at file save like a transform
  input's; an input reading another node's output also names that node and the output's
  type, `upstream` and `type`), `<output name type>`, `<params>` and `<script>`, written
  producers first. A preview is not written; a history snapshot omits the exempt ones.
  Empty params are stored as `{}` (`RecipeGraph::upsert`), what the parse reads back, so a
  save round-trips byte for byte.
  - The parse reads the prototype's layouts too: `label` and `infer_outputs` are read past
    and are no change. A malformed entry (a pin or `query_time` that does not parse, an
    unknown output type, an input dataset id that is not a number, a saved path that names
    no loaded dataset or not the same one for every input) is kept with its error and
    reported by the restore. Entries are ordered producers first (a reader names
    `<key>/<output>`); a cycle, and every reader of one, is reported "cyclic", and the rest
    restore. The saved input dataset (its source, its path) resolves into
    `Recipe::dataset`, and the graph binds every input in that dataset only, also when
    another loaded dataset carries the same names; an entry with neither binds the one
    dataset carrying every input (D7). A scalar input names a scalar topic, whose first
    column it reads, or else a series (`topic/field`), as a transform input resolves. A
    reader whose producer is absent (removed, the reader left missing its input) installs
    in that same state from the recorded `upstream` and `type`, and relinks when its
    producer is installed again.
  - The restore keeps a node whose recipe the snapshot describes unchanged, whose upstreams
    stay, and which reads no series rooted at an output the restore removes or creates (the
    `replaced_outputs` the generators get, LAY-21). A kept node keeps its topics, results,
    cursor and state: nothing runs and nothing is written. `clearExcept` removes the rest
    first and cancels their runs, keeping their topics; the install of each wanted recipe
    that is not live reclaims a topic of the same output (same id), and requests the node
    where the session wants it (a finding at its pin, a demanded node at the cursor), on
    the pool; `sweepUnclaimed` retires the topics nothing reclaimed and requests their
    remaining readers again. A node bound to another dataset than the saved one is not
    unchanged. A node outside the restore's authority (a preview, an exempt one under
    history) reading a processor output the restore removed or created runs again, with
    its readers (`OnDemandService::rerun`). One `objectTopicsChanged` when anything was
    cleared or installed: the installs between `clearExcept` and `sweepUnclaimed` leave
    their retired topics to it. An entry that fails, to parse or to install, fails the
    restore, as a transform does.
  - History refuses a snapshot that takes away an output an exempt node reads: a processor
    output it removes, or an on-demand object output (its node dropped, moved to another
    dataset, or the output no longer declared). An edit that keeps the output is not
    refused: the reinstall keeps the topic and the exempt reader runs on the new result.
    The exemption of an installed key never changes (`RecipeGraph::upsert` refuses it), so
    no history snapshot holds a key under the other authority. Under both intents, a
    preview holding the key of a recipe the snapshot wants is refused before anything
    changes: no restore clears a preview, so the saved recipe would be skipped.
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
- **On-demand recipes through the dataset life cycle** (built, slice 6a-5).
  - Before a reload (when the refill begins), a merge (the anchor and every source), an
    eviction of object topics and a clear, and again when a rollback restores the data, the
    session moves the dataset's epoch, drops its nodes' results (before the refill detaches
    the data, so a rollback brings none back), cancels their runs and holds their requests.
    The hold ends from
    the event loop, after the caller rebuilt the catalog and committed the reloaded TF with
    no event loop in between, and each node that had a request is requested again at its
    cursor.
  - A removed dataset removes its recipes and retires their topics, announced once; a run
    in flight is never published. A raw input evicted from under a recipe leaves it
    installed, missing that input by name, and the input coming back relinks it.
  - A merge installs the anchor's and the consumed datasets' recipes again, from the event
    loop (after the catalog rebuild, through which a scalar input resolves), producers before
    readers from one snapshot, so they re-bind on the anchor; an input qualified with a
    consumed dataset's source resolves there too, and the script keeps the name it reads.
    One that no longer resolves is removed, with a warning. A consumed
    finding's pinned time, and a consumed node's cursor, move by that dataset's raw shift
    (saturating). The derived
    entries are cleared before the object fold, never merged as raw observations: the
    finding timestamp is a sentinel and is never shifted.
  - Known limits: a recipe a merge re-binds gets a new output topic, so a layer bound to
    the old one loses it (a consumed dataset's raw topics keep their ids), and the demand set
    stays keyed by the consumed dataset until a layer references the re-bound topic (not
    handled: the user re-adds the layer). Two merges before the event loop turns (B into A, then A into C) can re-bind a
    recipe with the first merge's shift only: not handled. `replaceDataset`, which no production path calls
    (a reload refills), erases a dataset's derived topics with its vanished ones; and a
    derived topic the user trashes leaves its recipe publishing into nothing.
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

**Status: Built** for Luau; for Python the core and the binder are built (slice 10a-1: the shared
interpreter, its limits and the hook; slice 10a-2: the strict profile and compile once; slice 10b:
objects, builders and `pj.tf`; slice 10b-2: images and depth) and its runtime wiring is in review (slice 10c). Luau
VM reuse is proposed (N1, N2).

- **Luau** gets a fresh sandboxed VM per call, from bytecode compiled once per recipe
  (built, slice 5c-1: about 43 µs per call on this stack, §19). **Proposed:** one sandboxed state per pool worker plus a fresh sandboxed thread per call.
  That cuts about 86 µs of setup to about 2 µs. It needs the memory cap and instruction
  budget re-armed per call, and the random seed reset per call. Trigger: decide when the
  series slice measures per-call overhead over a long history. Before adopting it, a test
  tries to leave state behind (a global table, the string metatable) and checks that the
  next call cannot see it.
- **Python** runs on its own lane in a fresh namespace per call (built, slice 10a-2;
  `pj_scripting/include/pj_scripting/python_object_script.h` owns the contract).
  - The safety pre-check and the compile run once per recipe revision; a run executes the
    code object: 2.8 µs per call for a 4-line script and 15.6 µs for a 43-line one, against
    93 µs and 880 µs compiling every call (§19). A one-hour series at 10 Hz (36,000 calls) of
    the 43-line script spends about 0.6 s on setup instead of 32 s.
  - Scripts import read-only copies of the allowed modules, so `math.pi = 3` is an error and
    never reaches the next call (D2). A copy holds the module's public names only and leaves
    out the modules it imports outside the allowlist, so `statistics.sys` and `json.codecs`
    do not exist (the strict profile; filters keep the real modules).
  - **Decided (2026-10-18): the trust model.** Python runs in-process with cooperative limits,
    documented as not a security boundary. Isolation in a separate process waits for a request.
  - **Decided (2026-10-20): two profiles of one sandbox.** Both engines run every call into
    script code (a filter's teardown included) under the same limits: the watchdog (250 ms
    per filter call, 2 s for a load, a create or an on-demand run), the 64 MiB allocation cap
    (a filter's spans its life; decided 2026-10-20: past the cap only a block of 1 KiB or more
    is refused, smaller ones are always served, because CPython 3.12 releases a code object
    twice when a function's allocation fails, and a call whose charges pass twice the cap is
    stopped by a MemoryError the watchdog raises at its next bytecode, within about one GIL
    switch interval of allocations), sticky failures (a caught `MemoryError` or `TimeoutError`
    still fails the call; a call past its deadline fails even if the watchdog never
    interrupted it) and the process-wide audit hook (built, slice 10a-1). Filters use the
    compatible profile: they keep the real builtins, no AST check and the real modules behind
    their `math`/`cmath`/`statistics` allowlist, so every existing filter runs unchanged and
    stays as escapable as before. On-demand scripts are new, so they get the strict profile
    on top (slice 10a-2): an AST check, gated builtins, read-only module copies and a hook
    that also refuses `exec` and `compile`. A copy's functions still share the real module's
    globals, so a filter can patch, say, `statistics._sum`, and later on-demand runs call the
    patched code; accepted under the trust model.
- **The Python binder** (slices 10b and 10b-2; `python_object_script.h` owns the surface) reads the objects with
  Luau's native operations, through the language-neutral `script_contract.h`. In Python's idiom:
  0-based indices, tuples for vectors, keyword arguments for the builders, `!=` for not equal, and
  `(value, None)` or `(None, reason)` where Luau returns `value` or `nil, reason` (`pj.tf.lookup`,
  an image's `decode()`, a video's `frame_at`); a call whose result is an output (`to_point_cloud`, `threshold`) returns a
  value or `pj.unavailable(reason)`, as in Luau. Every operation
  that walks the data releases the GIL (N6), so GUI-thread Python work proceeds meanwhile. Every type
  is a plain CPython type: pybind11 dereferences an allocation the cap refuses (in a keyword call's
  dispatch and in instance creation) and costs a type lookup per list element (1.5 µs against about
  195 ns). A call that needs the run's limits, media or observations raises outside its run (field
  reads, `count`, `bounds`, `extreme_point` and `xyz_points` iteration work anywhere), and a builder
  outside the run that made it: an object a script leaves behind never reaches a dead run's limits,
  and it keeps its input's keepalive
  (the producing parser's library), so its payload never outlives the code that frees it. A result
  keeps its outputs' native charge, so retained results count against a shared ledger. A video's
  `frame_at(t)` returns `(frame, None)`, the frame a dict of `image` and `presentation_ns`, or `(None, reason)`
  (slice 10c), and every media
  input is held for the run (`ScriptMedia`, both binders), so its address names that input even once
  the script drops it.
- **In the runtime** (slice 10c): a recipe's `language` picks its compiler and runner in one place
  (`pj_scripting/include/pj_scripting/on_demand_script.h`): `RecipeGraph` compiles each revision once into
  a `CompiledOnDemandScript` (Luau bytecode or the opaque Python handle, so the graph includes no
  CPython), its runs take the lane the language picks (§11) and run through `runOnDemand`. Cursor
  requests, evaluations, series and the plugin bridge's `validate` (headless sessions included) accept
  `python` wherever `pythonAvailable()`; a build without it (WASM) refuses it with "python is not
  available in this build". An identical upsert keeps the compiled script (no compile). Each input
  carries the libraries its object needs (`NamedValue::keepalive`, an upstream override's owners
  included), so a proxy a script parks past its run keeps its parser loaded; a ring-held result drops
  its outputs' keepalives with their values. A run's cancellation reaches pure Python through the
  watchdog, which polls the pool token's flags (`RunOptions::cancel_flags`) and raises
  KeyboardInterrupt (past `except Exception`), so a removed or superseded loop frees the Python lane
  within about 50 ms instead of at its timeout. A compile (create, validate) and the
  interpreter's boot run on one Python helper thread (a thread and a queue,
  `python_object_script.cpp`); the GUI waits for a compile at most `kPythonCompileWaitMillis` (2 s) and
  otherwise fails it with "Python is busy (a running script holds the interpreter); try again", the late
  result dropped on the helper. A compiled script's last release is posted there, never waited for. The
  interpreter boots lazily, at the first Python compile, so a session without Python pays nothing.
  What remains: a script stuck in a builtin C loop (`sum(range(10**11))`, `math.factorial(10**7)`) holds
  the GIL until it ends, which neither the watchdog nor a cancellation can interrupt, so it keeps the
  Python lane busy until then. The first compile that times out marks the helper's current item stuck,
  and until that item finishes every compile fails at once (no clock decides it), so a restore of many
  Python recipes waits 2 s once, not for each; a compile in the moment the helper takes to finish that
  item after the script ends fails too, so a caller retries. Python filters are not routed through the
  helper: their inspect, create, `calculate` and teardown still take the GIL on the
  GUI or commit thread without a bound (`python_engine.cpp`); routing them is a later slice.
- **Python in the browser** (D18 with D29) is CPython 3.14.8's Emscripten target, linked
  statically with pthreads behind `PJ_WASM_WITH_PYTHON` (slice P-a): off in production, on
  in WASM CI's acceptance build. Only the on-demand lane runs there; Python filters stay
  unavailable. It adds about 1.93 MB (+13.6 %) of Brotli download; the first boot costs
  0.13-0.21 s, and a 4-line script's call is faster than the Luau lane's in the same browser
  (20 against 44 µs in Chromium, 112 against 300 µs in Firefox, which runs the whole build
  4-6 times slower). The build, the stdlib zip policy and the symbol audit are in
  `docs/WASM_DEPLOYMENT.md`. Open before production: a GUI-thread compile still waits up to
  2 s while a script holds the GIL inside one C call (slice P-c), and allocation-heavy code
  runs about 27 times slower than native (P-b).

## 15. Series

**Status: Decided** (D10, N8).

- A numeric output of a function with object inputs can be a plottable series, sampled at
  the union of its object inputs' entry timestamps. An explicit clock input is the upgrade
  path, only if a strong case appears.
- It is computed on the on-demand engine, which has the right semantics: a stateless value
  per instant.
- It runs through `GridRunner` with a column sink, the same loop as window evaluations (built for those in
  slice 8a-1a; the column sink is slice 11's).
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

**Built (slice 11a, the engine half): `SeriesRunner`** (`pj_runtime/include/pj_runtime/SeriesRunner.h`, owned by
`OnDemandService`, `series()`). A node that is not pinned, reads an object and declares a number output writes one
`DataEngine` topic named like its key, one float64 column per number output (`DataEngine::createColumnGroupTopic`).
Its clock is the union of the entry stamps of the raw object inputs of the node and of its upstreams, and the real
stamps of the findings it reads; a scalar side input changes values, never adds a row. It is computed in chunks of 64
clock instants after a watermark, each one `GridRunner` job (`submitRows`): one reservation in the shared line, so a
cursor request waiting there runs between two chunks, and at most one chunk of all series is in the runner at a time,
the others taking turns (§11); the next chunk starts on a later event-loop turn, so a chunk whose every instant is a gap
(staged and skipped without a run) never chains into the next inside one turn; the job's own upstream results, so no
upstream's ring or state moves; decoding and the runs on the pool, one run per task. A missing input, a failed run, or an unavailable or
non-finite number is a gap, counted with its first reason; the rows of a chunk are written in one commit at its end
and announced at most every 66 ms (the last chunk always). The series is wanted while its topic is in the demand set,
while a request is open (`request`, until its replay reaches the end of the clock), or when it is a preview (its
plugin's trial); otherwise it stops and keeps its rows, and resumes from its watermark. An edit, an upstream's new
revision, a re-created input and a dataset hold (`suspend`, a reload) empty the topic at once and replay it when
wanted; a live tick continues after the watermark. While its dataset is live (from a live tick to the stream's end,
`endLive`), a chunk's clock ends at the live edge, the oldest of the newest samples of the chain's raw inputs, object
and scalar (`InputStager::laggingInputStamp`; nothing computes while one has none): an input that arrives behind the
others (a 10 Hz lidar 50-100 ms behind a 30 Hz camera) or a scalar side input that lags then lands after the
watermark and appends, where a clock that followed the fastest input would replay the series at every entry of the
slow one. The stream's end computes the tail. Each chunk records the newest arrival its clock was cut from
(`GridRunner::Rows::seen`); an object input entry that arrived since then before the watermark (out of order: under the
live edge an in-order arrival never is) restarts the clock from its first instant, and that chunk's rows replace the
topic's (`ObjectStore::oldestArrivalAfter`, one binary search when nothing arrived). An entry at the watermark's own
stamp does not: a latched source republishing it would replay at every tick. Every change of a series' state, its
removal included, is reported by key (`SeriesRunner::Hooks::state_changed`, for 11b's progress). An edit keeps an open
request. A removal retires the topic, a restore parks it and the same key
reinstalled with the same columns in the same dataset adopts it; a foreign topic of the key's name refuses the install.
The series topics are derived data to a reload, the undo snapshot and the catalog's preview filter
(`SessionManager::derivedScalarTopics`). A Python series computes on the Python lane (slice 10c).

**Built (slice 11b-1, the app half without the Custom Topics panel).** *The visible range first*, as two ranges
(decided 2026-10-20): each time plot reports where its view starts to `SessionManager::setSeriesVisibleStart` (display
seconds, kept so and converted with the dataset's display offset again when it changes; one start per dataset, the
last plot to move wins), from `TopicDemandController`, before the demand on a curve change (so a dropped series starts
there) and on every move of that plot's time axis (`PlotWidget::visibleXRangeChanged`: a gesture, a linked, plugin or
state-strip zoom). When a series' next chunk would start before the view does (live: only
at or before the live edge), it splits, once per replay: its watermark moves to the visible start and computes
[visible start, end] first, then a backfill keeps the old watermark and arrivals cursor and computes the clock up to
the visible start. At most these two ranges: while both exist, a view starting at or before the backfill's end runs the
backfill's chunk next, else the forward range's, which a live tick continues; a later pan only reorders what is left,
and the backfill's end merges them back into one (at the backfill's watermark when the forward range evaluated
nothing). A restart from the first instant (an out-of-order entry) clears `complete` at once; a live append keeps it.
A plugin reads the split series in time order: `read_series_arrow` exports through the merged `RangeCursor`. The progress of a split series counts the whole clock
(`GridRunner::Rows::before`, the instants at or before a chunk's start). *Not computed*: the app sends every plot plain
`SeriesCurveStatus` data (dataset, topic, progress) on `SessionManager::seriesStateChanged`, and a curve on such a topic
reads "(not computed)" or "(computing N%)" after its legend title, the title itself staying the curve's identity, while
the series is not complete; a live append keeps a complete series complete (`replaying` then reports the append), so a
streaming series' legend does not blink at every tick. *A plugin's
read*: a whole-series read (`read_series_arrow`, and a marker rule's input) is an explicit request (decision 2): the
engine's complete-read barrier, which the session composes with the transforms' replay finish, calls
`SeriesRunner::requestTopic`, from another thread through a posted call; the reader gets the rows so far. Left for slice
11b-2: the Custom Topics progress row and its stop button.

## 16. Streaming

**Status: Built (slice 4)**; the on-demand live edge in slice 6c.

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
- **On-demand at the live edge** (built, slice 6c). The docks drive their visible layers to
  the newest stamp on every live tick, whatever the playhead does, so a live announcement
  (`SessionManager::notifyIngest` with `live`) first runs `OnDemandService::followLiveEdge`
  for each dataset it names: the datasets of its ids, and the one a streaming tick names,
  since a dataset of object topics only lists no ids. A demanded node is requested at its
  live edge (`liveEdgeOf`: the newest stamp among its raw inputs, its upstreams' included,
  read by identity through `InputStager::newestInputStamp`), a finding at its pin.
  - A node is requested only when what the request would read differs from its last live
    request: the time, the epoch, the revisions and raw input identities of the node and of
    every upstream it reads (so a change behind an upstream nothing displays is seen before
    that upstream runs) and, off a pin, the TF revision. So a tick with nothing new requests
    nothing, a same-stamp replacement is still seen, and a TF-only tick retries what read the
    TF under the snapshot brake (§19), which also covers a dataset with a live announcement in
    the last brake period: a stream never begins an ingest.
  - A finding runs again only when an input arrives at or before its pin; its install request
    records what it read. A finding with a raw input missing at its pin (evicted, or not come
    yet) is not requested, so it never pulls a displayed upstream back to the pin; and when a
    retry cannot stage it because its inputs left the window (an upstream's results followed
    that upstream's cursor, or the upstream misses its own inputs there), it keeps its result
    (`NodeScheduler::settleMissing`). An upstream that fails, a removed upstream, a new revision
    or a new epoch still clear it (D6).
  - While the transport follows the live edge (playing, held at the range's end), the live
    pass owns the requests of every dataset with a live announcement in the last brake period
    (`SessionManager::isLive`), and the playhead requests none of them: it sits at the newest
    sample, not at each recipe's inputs'. When the transport starts following again, each
    demanded node of a live dataset is requested at its live edge at once
    (`OnDemandService::restartLiveEdge`), since its result there may have left the ring while
    the cursor was elsewhere and a quiet input would announce nothing new.
  - The stager remembers each object input's topic id and checks it against the store's id
    index (`ObjectStore::isTopic`) on each use, so neither the live check nor staging scans the
    topics by name; a topic removed or registered again under the name is found again.
  - No changed-object feed (STR-05/07 removed): the store is inspected after the
    announcement, which also sees what no write host reports (a toolbox writing straight
    into the store, the terminal flush).
  - A displayed derived topic, a finding's included, keeps its raw inputs in the demand
    tracker's derived-input tier (`TopicDemandTracker::setDerivedInputTopics`, set by
    `AppSession` on a demand change, an install, a re-binding and a removal), so a
    demand-capable stream sends them while only the derived topic is displayed.
  - Known limit: a playhead scrubbed back while streaming, paused. The docks jump to the live
    edge each tick, the playhead requests at the cursor, so a node's requests alternate
    between the two; the pending slot keeps the latest.
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

**Status: Built** (slice 7a for D30 and findings).

- **3D and 2D layers** render derived topics.
  - They read a derived topic from the store like any other topic: the newest entry at or
    before the cursor. There is no readiness gate: a pending node's ring keeps its newest
    result, and a missing input or an error writes an empty tombstone (D6), also where the
    ring shows nothing after an edit or a dataset epoch dropped the results a layer still
    draws (`ResultHistory::resultsDropped`). An empty
    point cloud is a valid empty sample, drawn as nothing with no warning (D25); an empty
    image, color or depth, is an explicit clear through the 2D source, the compositor and the viewer.
  - With nothing at or before the cursor, a layer (3D or 2D) clears when the cursor is
    before the entry it shows (nothing is there) and keeps that entry when it is not (the
    ring evicted it under the cursor); a SceneEntities layer keeps the entities too.
  - A result landing at the dock's time re-runs the layers on that topic there
    (`derivedEntriesPublished`, `ISceneLayer::refreshAt`), with no tracker tick; a layer
    whose shown entry did not change decodes nothing.
  - A `pj_snapshot` SceneEntities topic (every derived one, D27) folds the shown entry
    alone: an entity absent from the next result is gone.
  - Scene2D core stays runtime-free: a derived topic reaches it as a store topic.
  - The dock's time-range clamp and the live-edge fold use the inputs' range and skip
    findings (§9).
- **What the dock shows beside the geometry** comes from the node's state
  (`derivedTopicPresentation` in `pj_scene_common/scene_layer.h` owns the wording), re-read
  on `derivedStateChanged`, never per frame:
  - The row warning says why a layer is empty (the missing input, the error): the layer's
    derived warning part (`SceneWarningParts`).
  - The row status is not a fault: "behind 0.4 s" (D30, the scheduler's lag: how far the
    shown result is behind the current request's, never the cursor minus a stamp),
    "computing…" before any result, and a finding's "computed at 12.4 s" (display seconds).
    It is what makes showing an older result acceptable, and it tells the user that the
    function needs attention.
  - The dock's status strip shows "computing (behind 0.4 s)", the largest lag of its
    visible derived layers. The lag is per node: two docks at different times share the newest
    request's number.
- **Initial framing.** A user drop into an empty 3D view (or the first topic into one built
  from its icon) frames the first content once: the fixed frame becomes the layer's frame
  and the camera fits its bounds. User camera input, a fixed-frame pick or a layout restore
  cancels it. It fits the bounds where they are drawn (`placeObject`, once a tracker time
  exists), and a SceneEntities finding keeps one placement hold per entity frame.
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
- **The Scene2D pairing check (built, slice 9a-2):** an annotation of an on-demand derived
  topic naming its image (`image_topic` not empty) is drawn only over that image, at its time:
  the base layer's store topic must match and its timestamp must lie within one cadence (the
  median gap of the image topic's last 16 entries, 100 ms with fewer) of the stamp of the frame
  actually delivered, never the tracker's instant (`CompositeMediaSource::setLayerPairing`,
  wired by `Scene2DDockWidget`, which marks the layer from the topic's `pj_derived` metadata).
  That stamp is the image's own (`DecodedFrame::stamp`: its payload's plus the entry's merge
  shift), which a script stamps the annotations with; the cadence is looked up around the
  frame's store entry (`DecodedFrame::entry_ts`; under embedded timestamps a parser's record
  stamp is the header's, not the entry's). A rejected annotation stays retained and is checked
  again at every composite; the layer row says why (`SceneWarningParts::Part::kAnnotation`),
  and the reason leaves the row when the layer is hidden. A parser's annotations are shown as
  they are: the `image_topic` of the shipped yolo handler is the header's frame id, which is
  informational. Known limit: the topics compare by bare name, so where two datasets share a
  topic name an annotation can pair with the other dataset's image of that name.
- **Scene tabs** are kinds of `pj.plot_tabs.v1` tabs (built, slice 8b-2: `PlotTabsRuntimeHost`
  and `pj_app`'s `PluginPlotTabsController` over `PluginSceneViewsController`). A plugin's tab
  of any kind lives in one id namespace and is routed by the kind of its live widget, so a
  scene tab a layout restored still answers; a kind the shell cannot build is refused by name.
  A topic is attached through the dock family's own gate (`tryAcceptObjectTopic`), and a
  scene tab's topics are its layers plus its config topics (a 3D tab's TF,
  `SceneDockWidget::configTopics`) for config, detach and clear alike. A tab the plugin
  empties keeps its kind (`SceneDockWidget::detachTopic`), and so does one eviction empties: the
  shell resets only a user's emptied dock to a plot tab, never a plugin's own. `clear_tab` also
  drops the layout-restore intents still pending, and neither it nor `tab_config` touches a layer
  the user added by hand (a URDF file, TF trails).
- **Dialog-embedded scene views** give a plugin panel a live 3D or 2D view (built, slice 8b-3).
  `pj_dialog_host` stays scene-library-free: a panel's `EmbeddedViewHost` (three callbacks the
  app supplies) creates, attaches to and deletes an opaque widget, and `applyWidgetData`
  reconciles a frame's `scene_view` request by difference. An unchanged request is a no-op,
  a malformed one included; a changed kind recreates the view; a changed set retries every
  topic not attached. What it refuses (a topic, an unservable kind, a malformed request) comes
  back from the apply and reaches the plugin as `scene_view_error` once the update is applied,
  never during it; the next tick applies the plugin's answer, also while an interactive
  sub-panel is open. Only the panel and its sub-panel embed views (a dismissed sub-panel is
  deleted with its views); a modal sub-dialog never does: as the SDK contract says, its
  `scene_view` is not served and its failures are not reported. The view's own controls
  (Scene3D's camera and fixed-frame combos) are not panel widgets: `connectWidgetSignals` and
  `installDeclarativeRules` skip everything inside it. In `pj_app`,
  `MainWindow::makeEmbeddedViewHost` builds the views as seeded scene docks (so an attached
  topic is demanded) and attaches through `PluginSceneViewsController::attachEmbeddedTopic`,
  the plugin scene tabs' path, except that a 2D view refuses a second background instead of
  swapping it in. A detach undoes the topic its attach resolved, whatever the name resolves to
  by then, once no other entry (the same topic by bare name and by dataset) still holds it.
  The tracker broadcasts tick the views, a hidden one only on the full broadcasts, and a
  revealed one catches up to the playhead; a catalog change revalidates them like a plugin's
  scene tab, then re-attaches each topic whose layer went that the frame still asks for, or
  records it as evicted so a detach skips it (`ObjectStore::clear()` reuses ids). Their
  changes take no workspace snapshot and their refusals show no toast (the plugin gets
  `scene_view_error`), and each dies with its frame.

## 18. Plugin surfaces (SDK host contract)

**Status: Built in the SDK (#202).** It ships in the minor after the header-only 0.36.0. Its
contract decisions were settled on 2026-10-13 (§5 and below). In the host, the router's typed
slots and the v2 create on the transform and marker kinds are built (slice 8a-2a,
`DataProcessorsKindRouter.h`), and so is the on-demand bridge (slice 8a-2b,
`OnDemandRuntimeHost.h`, the router's third backend in every plugin binding): the typed create
(INSTANT pins a finding), list, config with the contract's state keys, remove, validate, and
the evaluations over the session's `GridRunner` with bounded per-bridge handles, one of them in
the runner at a time (the others wait PENDING, §11), each report serialized on a worker (§19). A plugin's preview lives under `<plugin>/~preview/<n>/<id>`, `n`
unique to the bridge: no persistent recipe can take that key, so a layout never names a live
preview, and another instance of the plugin neither sees nor ends it. A node is a plugin's only
when its recipe's owner is (an id may contain `/`, so two owners can spell one key, and
`RecipeGraph::upsert` refuses the second). Params are bounded at 1 MiB and 32 levels, the
scripting engine's depth, since copying or dumping a deeper value would overflow the GUI stack.
A scripted submit is a preview by definition and is bound, never installed. The header owns the
rules.

- Catalog snapshot v2: object topics with dataset, type, entry count and time range. The
  plugin sets `struct_size`, under the read-prefix rule. The host writes back how much it
  filled, and rejects a size below the v2 minimum. A flag to skip the scalar half is added
  only if measuring the editor's once-a-second refresh shows that it matters. Built in the
  host (slice 8b-1, `DatastoreToolboxHost` in `pj_datastore/plugin_data_host.hpp`, which
  owns the rules): a derived topic's count and range are what its ring retains now, and
  the snapshot costs about 0.9 µs per object topic at 10k (§19).
- Data processors v2: a typed request with typed outputs, an optional dataset id, and INSTANT or
  WINDOW (no label).
- Asynchronous evaluations: submit, poll and release, returning a JSON report.
- `pj.plot_tabs.v1` tail slots: `create_tab_v2`, `attach_topic`, `detach_topic`,
  `focus_tab`. Each slot is filled when it is implemented (not all-or-nothing), and a kind
  the host cannot serve is rejected with an error naming it. `hasSceneTabs()` is removed;
  it was never released. Built in the host (slice 8b-2, §17).
- Dialog scene views: one `scene_view` key holding the kind and the topics, gated by
  `PJ_DIALOG_HOST_EMBEDS_SCENE_VIEWS`. A topic the host cannot attach is reported to the
  plugin, not only logged. Built in the host (slice 8b-3, §17): a panel announces the bit
  exactly when it has an `EmbeddedViewHost`, a modal dialog never.
- Manifest keys: `badge`. `custom_topics_editor` is a tag in the existing `capabilities`
  array: declarative tags read without loading the plugin, distinct from the
  `capabilities()` bits in code.
- Metadata keys: `pj_snapshot`; `pj_derived`, an open value set (the key means "produced
  by a host data processor", the value names the kind); the marker keys of §13.
- Marker generators on the v2 request take a typed `scope`, instead of `"__global__"` and
  `{"scope":"all"}`. v1 keeps accepting those.
- Capability detection, in the DATA PROCESSORS section of `plugin_data_api.h`, by three rules,
  never by version: the typed tail slots by `hasTypedRequests()`, a flag bit by its refusal on a
  host that does not know it, a build-dependent backend (Python) by doing it.

Under the ring, a derived topic exposes the results in its window to object reads at any
`t`. A finding reports its fixed store timestamp in `out_timestamp`; the real stamp is in the
payload (§9).

## 19. Measurements

These come from the prototype, whose design document describes the machine and method,
except the slice 2, slice 3, slice 5a, slice 5n, slice 5c-1, slice 6a-2, slice 6a-3, slice 6a-4a, slice 6a-5, slice 6c, slice 7a, slice 8a-1, slice 6b, slice 8a-2b, slice 8b-1, slice 9a-1, slice 9a-2, slice 9b, slice 10a-2, slice 10b and slice 11a rows. The slice 2 and 3 rows are measured on this stack by
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
| String inputs after slice S-str (`InputStagerBench.StageAStringInputAgainstANumber`, same file, `PJ_BENCH=1`; one input over 10k samples read through `InputStager::sampleAt`, median of 101 calls, three runs, GCC 15 -O2) | `stage` on the GUI thread, pinned to a performance core: 266-275 ns for a string (the failed numeric probe, the row lookup and the copy; its identity is its row, no hash) against 251-261 ns for a number; `decode` of the staged string, on a worker: 40-42 ns (one copy). The fixture names its series by a scan of three names; the session's catalog lookup and exact-topic scan come on top for both kinds alike |
| On-demand request after slice 6a-4a (`NodeSchedulerBench`, `pj_runtime/tests/node_scheduler_test.cpp`, `PJ_BENCH=1`; median of 101 requests, three runs) | GUI time per `requestAt` over a lazily stored 260k-point cloud: 4.8-8.0 µs cold (staged, a miss, the run submitted), 0.39-0.42 µs warm (a hit), against the prototype's 4-5 ms cold. With 600 and 1800 results in the window (`RequestAtWithAFullWindow`): warm 0.38-0.42 µs either way, cold 1.31-1.43 µs and 1.29-1.33 µs (the lookup reads only the staged stamp) |
| On-demand playhead tick after slice 6a-5 (`EngineFreezeBench.OnDemandColdAndWarm`, `pj_runtime/tests/engine_freeze_bench_test.cpp`, `PJ_BENCH=1`; through `AppSession`, one demanded recipe over a lazily stored 260k-point cloud, the real Luau run on a worker; median of 100 ticks, three runs) | GUI-thread CPU time per tick (the demanded keys, the conversion, staging, the lookup, the submission): 6.9-14.1 µs cold (a new instant), 0.76-0.89 µs warm (a hit), against 4.6-6.5 µs and 0.36-0.63 µs for `requestAt` alone in the same runs. An install (resolver, compile, history install) among 1 and 10,000 object topics: 120 µs and 163 µs, so the resolver's scan costs about 43 µs, once per install. The same recipe with 4 scalar inputs, among 10,029 catalog items: 508-514 µs cold and 483-485 µs warm per tick while each staging scanned the catalog for each scalar, 5.3-13.5 µs and 1.7-3.7 µs once the catalog memoizes its lookups |
| Scene dock after slice 7a (`SceneDockWidgetBench`, `pj_scene_common/tests/scene_common_test.cpp`, `PJ_BENCH=1`; median of 15 x 10,000 calls) | A derived layer's clock range (`derivedTopicInputRange`, run per layer on every tracker tick by the clamp): 298 ns for a reader two hops from its raw input, among 10,000 topics, so no per-layer cache. Its presentation (`derivedTopicPresentation`, on a state change): 155 ns, 14 ns for a raw layer |
| Unchanged on-demand restore after slice 6b (`DerivedObjectsBench.OnDemandRestoreComparisonPerRecipe`, `pj_runtime/tests/derived_objects_on_demand_test.cpp`, `PJ_BENCH=1`; a chain of N recipes the snapshot describes unchanged, parse, refusal and restore, median of 5) | 50 / 200 / 800 recipes (N-1 edges): 142 µs / 584 µs / 2.48 ms, 2.8-3.1 µs per recipe, linear; no script runs, no entry is written (`UnchangedRecipeKeepsItsEntryAndRunsNothing`). The refusal returns at once here (no exempt recipe); with one it adds a pass over the live nodes |
| Publication of a 4.2 MB point-cloud result on the GUI thread (`GuiPublishCostOfA4MBResult`, 260k points, median of 51) | Serialized at the publication: 1.45-1.49 ms. Serialized on the worker (`ResultHistory::serializeOutputs`), the payload moved into the ring: 2-3 µs |
| 2-hop chain after slice 6a-4a (`TwoHopChainLatencyAt10Hz`; requests at 10 Hz, from the reader's request to its published result, 50 requests, three runs) | Two Luau crops keeping all 260k points: median 6.8-6.9 ms, max 11-23 ms, every request drawn (14.6-15.0 ms while the compute cache kept a decoded copy of each result the ring holds). The scheduling alone, an instant fake run with empty outputs: 0.17-0.20 ms, so a chain in one worker task would save little |
| Explicit evaluations after slice 8a-1a (`GridRunnerBench`, `pj_runtime/tests/grid_runner_test.cpp`, `PJ_BENCH=1`; three runs) | A window's grid over 10k object entries and 100k scalar samples (`GridOverManyInputChanges`, the submission: grid, gaps; the fake scalar lookup generates its samples, a session's reads them from the datastore): 1.7-3.2 ms, 15-29 ns per instant. A two-hop explicit evaluation, two Luau crops keeping all 260k points (`TwoHopExplicitEvaluationLatency`, from submission to report, median of 21): 5.6-6.4 ms, against 6.8-6.9 ms for the cursor's 2-hop chain. After slice 8a-1b (`GuiCostPerInstant`, thread CPU time of the GUI thread, the test's event-loop pump included, fake pass-through runs): 48-67 µs per instant of a 400-instant window over 24k-point clouds; 42-69 µs per bundle at 200 instants and 50-57 µs at 2000 (flat: the report grows by appending); `toJson` 1.5-2.9 µs per bundle at 200 and 2000 bundles |
| The on-demand plugin bridge after slice 8a-2b (`OnDemandRuntimeHostBench`, `pj_runtime/tests/on_demand_runtime_host_test.cpp`, `PJ_BENCH=1`; through `AppSession` and the SDK's client view, fake runs; three runs) | `data_processor_config` of a finding (the JSON, state, findings accounting over two recipes): 5.5-10.4 µs, so the editor's once-a-second read is negligible. With 63 live handles: submit, poll and release of one more 2.0-4.5 µs, a poll 39-75 ns. The playhead tick of one demanded recipe (GUI CPU, median of 100, the cold path of 6a-5's bench with a 2 ms fake run instead of a 260k-point Luau crop): the move 6.6-6.7 µs with no evaluation, 6.8-6.9 µs while 64 mixed evaluations (instants on the installed node, scripted windows) run and finish between the ticks, within 6a-5's 6.9-14.1 µs; with the drain after it (publications, evaluation steps, completions) 58-61 µs and 59-63 µs. One evaluation whose report reaches the 8 MiB cap (1908 bundles of a 4 KiB string): `GridRunner::toJson` takes 34 ms, over the GUI-thread slice a create may take (`BatchEngine::kCreateBudget`, 8 ms), so the bridge serializes (and frees) the report on a pool worker and moves the string into the slot (decided 2026-10-19): the longest GUI event-loop pass of that evaluation fell from 35.1-35.4 ms to 0.09-0.21 ms. The playhead's result latency right after 64 evaluations are submitted (from the request to its result in the ring, on a 20-thread shared lane): 32.4-39.7 ms with all 64 in the runner, 4.4-4.6 ms with one per bridge (decided 2026-10-19); the tick's GUI CPU is unchanged (move 6.5-14.6 µs, with the drain 60-78 µs). On a one-thread lane the first figure grows with the evaluations' total run time |
| Catalog snapshot v2 after slice 8b-1 (`PluginDataHostCatalogV2Bench.SnapshotNsPerObjectTopic`, `pj_datastore/tests/plugin_data_host_catalog_v2_test.cpp`, `PJ_BENCH=1`; through the SDK's client view, which also checks every entry; one entry per topic, a derived cloud's metadata, no scalar topics; median of 21, three runs) | 1.27-1.33 µs per object topic at 100 topics, 0.83-0.90 µs at 10,000 (about 9 ms per snapshot), 1.03 µs at 100,000. Reading each topic's type from its metadata (a JSON parse, after the store's lock is released) is about two thirds of it: 0.11 / 0.27 / 0.41 µs without it (one run) |
| Series after slice 11a (`SeriesRunnerBench.ReplayOver400Clouds`, `pj_runtime/tests/series_runner_test.cpp`, and `GridRunnerBench.RowsChunkAmong100kEntries`, `grid_runner_test.cpp`, `PJ_BENCH=1`; three runs) | A replay over 400 clouds of 24k points through `AppSession`, the fake run instant (a real script adds worker time only: the GUI thread stages, collects and writes): GUI-thread CPU 9.9-11.6 µs per row, the test's pump included, the longest event-loop pass 56-65 µs against PERF-03's 8 ms, 64 rows per commit (one per chunk). Submitting a 64-instant chunk in the middle of 100k entries (the clock sliced by binary search): 0.85-0.94 µs; a live tick with nothing after the watermark: 0.46-0.47 µs, so the clock is never held or rebuilt. After the 11a fix round (the late-arrival check, input topics found through `InputStager`'s remembered ids): 1.01-1.09 µs and 0.57-0.58 µs; GUI CPU 11.0-13.0 µs per row, longest pass 61-65 µs. An idle live tick of 5 complete series among 10k object topics (`GridRunnerBench.IdleSeriesTickAmong10kObjectTopics`): 166.6 µs (a scan by name per input) → 3.2-3.6 µs. After the gate round (the live edge, looked up through `InputStager` per chunk, the 100 ms chunk budget, the state reports): the idle tick 3.2-3.3 → 3.7-4.0 µs (the live edge's lookups); a chunk submit 1.08-1.11 → 1.01-1.10 µs, nothing new 0.58 → 0.56-0.57 µs; the replay 11.1-11.5 → 8.0-11.1 µs per row, longest pass 59-65 → 60-63 µs |
| Streaming: 5 min of 10 Hz, 2048-point clouds | Bounded plateau of entries and bytes |
| The same after slice 6c, through the session's live edge (`DerivedStreamingBench.FiveMinuteRetentionAndMemoryPlateau`, `pj_runtime/tests/on_demand_live_edge_test.cpp`, `PJ_STREAM_BENCH=1`; 3,000 ticks simulated back to back with the fake run, the raw topic kept 5 s, one demanded recipe; three runs) | From the first minute on: 51 raw entries, 301 derived entries (the ring's ±30 s window around the cursor), ring 9.9 MB under its 256 MiB budget, compute cache 46 KB, nothing protected past the budget, 0 bytes reserved between ticks; RSS 77-78 MB, +0.8 MB from minute 1 to minute 5 |
| Live edge after slice 6c (`OnDemandLiveEdgeBench.LiveEdgeAmongTenThousandTopics`, same file, `PJ_BENCH=1`; one demanded recipe reading a cloud topic registered after 10k others and a scalar, median of 101 calls, three runs) | `liveEdgeOf`: 21.4-21.7 µs → 194-197 ns; an unchanged tick's `followLiveEdge` (the edge plus what the request would read, no request): 41.6-42.6 µs → 510-529 ns, once the stager remembers each input's topic id (checked by the store's id index) instead of scanning the topics by name. `stage` among 10k topics and 10k scalar names (`StageAmongTenThousandTopics`): 35 µs → 2.5 µs, the rest being that bench's own scalar scan |
| Per-call setup: Luau | about 86 µs per call (fresh VM) |
| Object scripts after slice 5c-1 (`ObjectScriptBench`, `pj_scripting/tests/object_script_test.cpp`, `PJ_BENCH=1`; three runs) | Per-call setup (fresh VM over compiled bytecode, a one-line script): 40-47 µs. Iterating a 100,000-element list field (`FrameTransforms.transforms`, reading `translation.x`): 11.0-11.5 ms (about 112 ns per element) holding the list in a local, 15.7-16.3 ms re-reading the field each iteration. The same loop with the list materialized on every element read (a one-line mutation of the binder): 18.6 s |
| Per-call setup: Python | 73 µs (4-line script) to 1.2 ms (43-line script), re-parsing every call |
| Python per-call setup after slice 10a-2 (`PythonObjectScriptBench.PerCallSetup`, `pj_scripting/tests/python_object_script_test.cpp`, `PJ_BENCH=1`; median of 2001 calls, three runs; a number input, a keyed result) | Compiled once: 2.8 µs for a 4-line script, 15.5-15.7 µs for a 43-line one (a class, four helpers, two imports); compiling every call (the pre-check and the compile included): 92-94 µs and 870-883 µs. With the allocation cap disabled: 2.5 µs and 12.9 µs, so its per-block bookkeeping costs 0.35 µs and 2.7 µs. The 4-line figure misses the 1.2 µs target (an estimate for a bare `exec` of the code object, without the cap, the builtins copy or the import gate), about 0.35 µs of the miss being the cap's bookkeeping; accepted (decided 2026-10-20), as it is 30 times below compiling per call. The 43-line one beats its 63 µs target. |
| Python binder after slice 10b (`PythonObjectScriptBench.ListIterationAndXyzPoints`, `pj_scripting/tests/python_object_script_test.cpp`, `PJ_BENCH=1`; 100,000 elements, median of 5 runs pinned to one P-core with `taskset`; unpinned runs land on a slower mode up to 15% above) | Iterating a list field (`FrameTransforms.transforms`, reading `translation.x`): 194-195 ns per element with a `for` loop, 269-270 ns indexing the list held in a local, against Luau's 112 ns; 1.49-1.60 µs while the views were pybind11 classes (a type lookup by `std::type_index` and a dispatch per element). `xyz_points()`: 149 ns per point (428-443 ns as a pybind11 class; 222-232 ns releasing the GIL for every point, so it scans 256 records with it first). Both include the allocation cap's bookkeeping as it was then, a lock and a map entry per Python allocation (two views per element); slice P-b cut it (next row) |
| Python allocation cap after slice P-b (`PythonSandboxBench.AllocationCapOverhead`, `pj_scripting/tests/python_sandbox_test.cpp`, `PJ_BENCH=1`; ns per element, best of 9 runs in each of three processes pinned to one P-core with `taskset`; a loop run bare, with the cap's wrapper installed but no scope armed, and inside a `ScriptCallScope` whose limit is never reached, so only the bookkeeping is timed) | Before: `sum(range(n))` 20.9 bare, 67.7 capped (3.24x); a list-append loop 34.6, 116.2 (3.36x); a dict-building loop 75.4, 147.2 (1.95x); 200,000 live `bytes(600)` 160.1, 334.9 (2.09x); float arithmetic over a list, no allocation, 17.0, 16.9. After: 20.4, 30.1 (1.48x); 34.3, 46.5 (1.36x); 75.0, 84.8 (1.13x); 158.9, 210.1 (1.32x); 16.9, 16.9. The cap now costs 10-12 ns per small block allocated and freed (47-82 ns before) and 54 ns per 600-byte block (175 before), the table's insert and erase and the budget's two atomic operations. The per-call setup of a compiled script is 6.3-6.6 / 17.9-18.2 µs (6.7-6.8 / 20.2-20.4 before) on this machine, and the `ListIterationAndXyzPoints` figures 115 / 146 / 120 ns (201-203 / 285-286 / 146-148 before). A locality-free hash made the million-live-block loops 4.1x and 2.8x, worse than the map it replaced; the table keys on `address >> 4`. Where the browser spike's 27x on `sum(range(n))` (257 ns per element against 9.5 native) comes from is not measured here: this bookkeeping was a share of it that grows with the platform's allocator and atomics cost (a `std::unordered_map` node through dlmalloc, a pthread mutex and two atomic reference counts per block), and the interpreter's own 2.5x on pure bytecode is the rest. Re-measure the browser with this build before choosing anything else |
| TF snapshot copy (prototype: a copy per snapshot) | 0.26 ms (5 s window) to 248 ms and 691 MB (1 h file) |
| TF snapshot after slice 5a (copy-on-write; measured on this stack by `TransformSnapshotBench`, `pj_tf/tests/tf_buffer_test.cpp`, `PJ_BENCH=1`; 25 edges at 100 Hz, every sample distinct, median of 5) | `snapshot()`: 0.02 µs and 0 bytes at any size. The next write while it is held copies the state once, on the writing thread under the buffer's write lock: live 5 s window 0.15 ms / 0.9 MB of samples, 60 s window 2.9 ms / 10.8 MB, complete 1 h file 72 ms / 648 MB (only when something writes the file buffer after a snapshot, e.g. a robot model's fixed-joint bridges); a buffer nobody writes is never copied. Consumer rule: during a dataset's TF ingest (a first file load writes the active buffer, streaming never ends) a fresh snapshot per revision makes every next write copy the growing state, quadratic over a load; take a new snapshot no faster than the brake rate there, or wait for the ingest to end |
| Native point-cloud operations (slice 5n; `PointCloudOpsBench`, `pj_scripting/tests/native_ops_test.cpp`, `PJ_BENCH=1`; median of 20 calls on a 260k-point XYZI float32 scan, flat and as 64 x 4096 with padded rows, which cost the same per point) | `filterByField` (50% kept) 3.0 ns per point, `cropBox` (30% kept) 9.5 ns, `bounds` 3.2 ns, `extremePoint` 0.9 ns, `transformXyz` 6.0 ns, a full `nextFinitePoint` iteration 6 ns. The same loops with a datatype and comparison switch per point, a filter predicate evaluated twice and a division per record run 1.25x to 5.3x slower |
| Voxel downsampling (slice 14; `PointCloudOpsBench.NanosecondsPerPoint`, the same file and scans, `PJ_BENCH=1`; cubes of 0.05 over the unit cube, about 8,000 centroids, 3.1% of the points; median of 20 calls, three runs pinned to one P-core with `taskset`) | `downsample` 60.5-62.8 ns per point, flat or organized (16 ms per 260k scan). The sort of the points by cube is about 60% of it (25 ns per point without it); a radix sort over the key bits the extent uses is the upgrade if it shows up in a profile |
| Native image operations (slice 9a-1; `ImageOpsBench`, `pj_scripting/tests/image_ops_test.cpp`, `PJ_BENCH=1`; median of 20 calls on 1920 x 1080 decoded pixels, three runs) | Per pixel, RGB: `to_gray` 1.2 ns, `threshold` 1.8 ns (1% matching) to 6.3-6.7 ns (50%: 1M points collected; 5.9 ns while the threshold was rounded to float), `stats` 3.2 ns, a whole-image `crop` 1.3 ns; depth `to_point_cloud` (16UC1) 7.7 ns. Each comparison is a templated kernel, dispatched once per call |
| Image decode and the on-demand request (slice 9a-2; `SceneImageDecoderBench`, `pj_app/tests/scene_image_decoder_test.cpp`, and `OnDemandMediaBench`, `pj_runtime/tests/on_demand_media_test.cpp`, `PJ_BENCH=1`; three runs) | `SceneImageDecoder` at 1080p: a JPEG 10.8 ms (Scene2D's turbojpeg plus the float conversion), a PNG 7.8 ms, raw rgb8 2.5 ms (1.2 ns per pixel); all run on the pool worker. GUI time per cold `requestAt` of a recipe that decodes and thresholds a 1080p image: 5.6-5.8 µs median (51 requests, a raw fake decoder: the decode runs on the worker, so the codec adds no GUI time), the same as for a 260k-point cloud: independent of the image's size (GAP-26) |
| Language dispatch on the request path (slice 10c; `NodeSchedulerBench.RequestAtGuiCostFor260kCloud`, `pj_runtime/tests/node_scheduler_test.cpp`, `PJ_BENCH=1`; a Luau and a Python recipe over a lazily stored 260k-point cloud, the fake run, median of 101 requests, three runs pinned to one core) | GUI time per `requestAt`: cold 0.79-0.82 µs Luau, 0.80-0.85 µs Python; warm 0.41 µs and 0.41-0.42 µs: picking the lane is one read of the script's variant index |
| Video on-demand request (slice 9b; `OnDemandMediaBench.GuiCostOfAVideoRequest`, `pj_runtime/tests/on_demand_media_test.cpp`, `PJ_BENCH=1`; one video input over 10k packets, 51 cold requests, three runs pinned to one core) | GUI time per cold `requestAt`: 0.80-0.92 µs median (six runs) with the topic's content revision in the key, against 0.77-0.83 µs without it (one more shared-lock read, near the noise); the frame decodes on the worker (a store-reading fake; `SceneVideoFrameSource` decodes the codec there), so nothing is fetched or parsed on the GUI thread. The frame's rgb24 conversion at 1080p (`VideoRgbConversionBench`, `pj_scene2D/tests/streaming_video_decoder_test.cpp`, three runs): 16.6 ms with the per-pixel loop, 10.8 ms through swscale (its faster paths, 0.65-2.5 ms, miss the shader's matrix by 2-255 on random samples), run outside the topic's lock |
| `ResultHistory` after slice 6a-2 (`ResultHistoryBench`, `pj_runtime/tests/result_history_test.cpp`, `PJ_BENCH=1`; three runs) | `lookup`, median of 1001 calls, the store's `at` per output included: 0.10-0.20 µs for a hit or a miss at 1, 100 and 10,000 results per node. 600 results of a 260k-point XYZI cloud at 10 Hz under the default limits: the ring holds 253.9 MiB (64 results); the compute cache 254 MiB with a decoded copy of each ring-held result → 11 KB without. Publication on the GUI thread (`PublishCost`, serialized there, 50 to 100 entries per topic): median 4-8 µs with a 64-byte cloud, 177-183 µs with a 1.5 MB one. One that removes nothing, at 600 to 800 retained results (`PublishCostAt600RetainedResults`): 11.8-12.7 µs with a drop predicate the store scans every entry for → 2.3 µs without one |
| Runtime lookups after F2 (measured on this stack, `PJ_BENCH=1`, pinned with `taskset` to one 5 GHz core, three runs) | `RecipeGraphBench.QueriesOverADiamondLadder` (`pj_runtime/tests/recipe_graph_test.cpp`; 16 diamonds, 33 nodes, median of 101 calls): `missingInput` of the last node 5059-5074 µs → 4.6-5.5 µs (each walk visits a shared upstream once), `dependentsOf` the root 12.8-12.9 µs → 8.2-9.3 µs (one reverse-edge map per call). `ResultHistoryBench.LookupCost`: a hit at 1, 100 and 10,000 results 98-99, 106-107 and 126-128 ns → 82-83, 89-90 and 106-109 ns, once `present` tests an entry through the store's decode-free `indexOf` instead of resolving it with `at`. `InputStagerBench.StageAmongTenThousandTopics`: 2.5-2.6 µs before and after: the stager already remembers each input's topic id (slice 6c), and the rest is that bench's own scalar scan, so no per-node topic table. `ResultHistoryBench.PublishCost` evicting every publish: 3.1-3.3 µs (64-byte cloud) and 101-112 µs (1.5 MB cloud) per publication, of which the per-entry `drop` predicate (`std::function` and `planRemoval`, 50-100 entries per topic) is 0.06-0.2 µs in a 40 kHz `perf` profile, so it stays |
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
