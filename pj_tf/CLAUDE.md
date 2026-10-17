# pj_tf

## Purpose

The TF frame tree: `Transform` (SE(3) pose), `StampedTransform`, `TimePoint`, and
`TransformBuffer` (the per-dataset forest of parent-child edges, ZOH lookup,
frame-hierarchy DFS). Namespace `pj::tf`. Public headers:
`include/pj_tf/{transform.h, tf_buffer.h}`.

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

## Tests

`tests/` — gtest, runner `pj_tf_tests` (plain main: no Qt). `tf_buffer_test.cpp` covers
the write and lookup contract (rejection reasons, ZOH, reparent policy, cache window,
repeat-sample skip, the two clocks against a reference model); `tf_buffer_hierarchy_test.cpp` covers the frame-hierarchy DFS and
its JSON form. The behavioral contract is `pj_scene3D/docs/REQUIREMENTS.md` §9.
