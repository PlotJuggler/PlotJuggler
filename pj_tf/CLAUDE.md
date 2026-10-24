# pj_tf

## Purpose

The TF frame tree: `Transform` (SE(3) pose), `StampedTransform`, `TimePoint`,
`TransformBuffer` (the per-dataset forest of parent-child edges, ZOH lookup,
frame-hierarchy DFS) and `TransformSnapshot` (one immutable state of a buffer, what
scripts read). Namespace `pj::tf`. Public headers:
`include/pj_tf/{transform.h, tf_buffer.h}`.

A buffer's state *is* a `TransformSnapshot`, held by `shared_ptr` and copied on write
(`docs/derived_objects/ARCHITECTURE.md` §12, R1/R4): `snapshot()` shares it in O(1), the next write copies it
first while a snapshot holds it, `clear()` swaps in an empty one and `adoptStateOf()`
another buffer's (a reload commit, in O(1)), so a snapshot never changes and is read
lock-free from any thread, a buffer nobody writes (a loaded file) is never copied, and a
buffer's holders see every replacement without rebinding (`epoch()` counts them, for a
reader that extends a cached result). The snapshot's public API stays minimal (lookup with a clock,
frame list, revision); the buffer reaches its internals as a friend. Revisions come from
one process-wide counter, so a revision names one state across buffers.

Every sample keeps two times: its header stamp and its entry time (when its message
entered the dataset; `setTransform(tf, entry_time)`, or the stamp itself through the
one-argument overload). Every lookup names its `Clock`: `kEntry` for the cursor and
query times, `kHeader` for placing an object at its own stamp. With header == entry
the two clocks agree. The design and the per-consumer choice of clock are in
[docs/derived_objects/ARCHITECTURE.md](../docs/derived_objects/ARCHITECTURE.md) §12
("The two clocks").

A **Qt-free leaf**: links only `glm`, `plotjuggler_sdk::base` (`pj_base/{expected,time}.hpp`),
and `nlohmann_json` (`getFrameHierarchyJson()`). No app/runtime dependency, so it
carries no risk of pulling Qt or the datastore into a consumer.

Consumers: `pj_scene3d_core` (geometry/frame-picking math) and `pj_runtime`'s
`TransformService` (ownership, load/streaming ingest, Qt signals — see
`pj_runtime/CLAUDE.md`). `TransformService` itself — and everything Qt or
session-lifecycle related — stays in `pj_runtime`, not here.

## Gotchas

- **The default `cache_window` is 10 s** (`TransformBuffer(Duration = 10 s)`), which trims every dynamic edge to its tail.
  A buffer fed a whole loaded file must be constructed with `kKeepAll`, or lookups earlier in the timeline fail;
  `TransformService` keeps `kKeepAll` for file loads and sets a finite window (`setLiveCacheWindow`) only for live streaming.
- **Locking differs by object.** A `TransformBuffer` is internally locked (`std::shared_mutex`): every read and write is
  safe from any thread. A `TransformSnapshot` is immutable and read lock-free; hold the `shared_ptr` for as long as the
  reads must agree with each other.
- **`setTransform` returns `SetTransformError`; it never throws.** A malformed edge in a bulk feed drops that one edge
  (`kInvalidFrameName`, `kInvalidRotation`, `kNonFiniteTranslation`, `kSelfLoop`, `kReparentConflict`), so callers count
  and continue. `kReparentConflict` is permanent: a child already parented to another frame drops every later edge,
  and the frame freezes at its last pre-reparent pose (tf2 would let the newest parent win). Only the throwing
  `lookupTransform` wrapper raises; `tryLookupTransform` returns a `LookupError`.

## Tests

`tests/` — gtest, runner `pj_tf_tests` (plain main: no Qt). `tf_buffer_test.cpp` covers
the write and lookup contract (rejection reasons, ZOH, reparent policy, cache window,
repeat-sample skip, the two clocks against a reference model) and the snapshot
(immutability, copy-on-write by storage identity, both clocks, unique revisions,
`adoptStateOf` and `epoch`);
`TransformSnapshotBench` there prints the snapshot and copy-on-write cost with
`PJ_BENCH=1` (baselines in `docs/derived_objects/ARCHITECTURE.md` §19). `tf_buffer_hierarchy_test.cpp` covers
the frame-hierarchy DFS and its JSON form. The behavioral contract is `pj_scene3D/docs/REQUIREMENTS.md` §9.
