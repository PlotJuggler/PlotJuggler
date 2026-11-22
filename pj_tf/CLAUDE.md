# pj_tf

The TF frame tree, namespace `pj::tf`: `Transform` (SE(3) pose), `StampedTransform`, `TransformBuffer` (the
per-dataset forest of parent-child edges, lookups that hold or interpolate, frame-hierarchy DFS) and
`TransformSnapshot` (one immutable state of a buffer, what scripts read). A static Qt-free leaf: it links only `glm`,
`plotjuggler_sdk::base` and `nlohmann_json`, so keep Qt and the datastore out. Consumers: `pj_scene3d_core`
(geometry, frame picking), `pj_scripting` (the snapshot a script reads) and `pj_runtime`'s `TransformService`
(ownership, load and streaming ingest, Qt signals; see [pj_runtime/CLAUDE.md](../pj_runtime/CLAUDE.md)). How the
buffer is used and which consumer reads which clock:
[docs/derived_objects/ARCHITECTURE.md](../docs/derived_objects/ARCHITECTURE.md) "TF" and "The two clocks".

## Invariants

- The default `cache_window` (10 s) trims every edge to its tail. A buffer fed a whole loaded file must be built
  with `kKeepAll`, or lookups earlier in the timeline fail; `TransformService` keeps `kKeepAll` for file loads and
  sets a finite window (`setLiveCacheWindow`) only for live streaming. An edge's last sample is never evicted.
- A buffer is internally locked (`std::shared_mutex`): reads and writes are safe from any thread. A buffer's state
  is a `TransformSnapshot` copied on write: `snapshot()` shares it in O(1), the next write copies it while a
  snapshot holds it, and `clear()` / `adoptStateOf()` swap the whole state behind the same buffer object (`epoch()`
  counts them). A snapshot never changes and is read without a lock; hold the `shared_ptr` as long as the reads
  must agree. A buffer nobody writes (a loaded file) is never copied.
- `revision()` comes from one process-wide counter: a revision names one state across buffers, so it is a valid
  cache key when a buffer is replaced.
- Every sample keeps its header stamp and its entry time (`setTransform(tf, entry_time)`; the one-argument overload
  uses the stamp). Every lookup names its `Clock`: `kEntry` for the cursor and query times, `kHeader` to place an
  object at its own stamp. Mixing them places objects at the wrong pose when log time and header stamps differ.
- `setTransform` returns `SetTransformError` and never throws: a malformed edge drops that one edge, so bulk
  callers count and continue. `kReparentConflict` is permanent: a child already parented to another frame drops
  every later edge and freezes at its last pre-reparent pose (tf2 lets the newest parent win). Only
  `lookupTransform` throws; `tryLookupTransform` returns a `LookupError`.
- `Transform::q` must be a unit quaternion (`inverse()` and `operator*` assume it): `setTransform` normalizes, a
  hand-built `Transform` does not.
- `latestCommonTime` on an all-static path returns an engaged `TimePoint{}`, not `nullopt`; feeding it to a lookup
  fails with `kNoSampleAtTime`. Use `areConnected` for reachability.

## Headers (`include/pj_tf/`)

- `tf_buffer.h`: `TransformBuffer`, `TransformSnapshot`, the error enums, `Clock`, `frameNameLess` (the one
  user-facing frame ordering).
- `transform.h`: `Transform`, `StampedTransform`, `TimePoint`.

## Tests

`./test.sh -R '^pj_tf_tests\.'` (plain gtest main, no Qt). `tf_buffer_test.cpp`: write and lookup contract, repeat
skip, both clocks against a reference model, snapshots; `TransformSnapshotBench` prints the snapshot and
copy-on-write cost with `PJ_BENCH=1`. `tf_buffer_hierarchy_test.cpp`: the hierarchy DFS and its JSON form. The
behaviour contract of the 3D view's TF handling is in [pj_scene3D/docs/](../pj_scene3D/docs/).
