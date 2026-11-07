# Derived objects

A derived object is the result of a script (Luau or Python) over any mix of scalar series and
object topics (point clouds, transforms, scene entities, images, annotations) of one dataset.
Its object outputs are ordinary topics, which plots and scene layers show without knowing they
are derived; its number outputs can be plotted as series. Nothing is computed without demand,
scripts are stateless, and a slow script keeps updating at the rate it can sustain (D1, D2, D30).

| Engine | Kind | Runs | Where | Header |
|---|---|---|---|---|
| `IncrementalEngine` | `transform` | each arriving entry, carrying state; filters and transforms | GUI thread, under the `DataEngine` lock | `pj_datastore/include/pj_datastore/incremental_engine.hpp` |
| `BatchEngine` | `markers` | a whole series (or a window) into one `PlotMarkers` set | `EnginePool` worker, behind a reload brake | `pj_runtime/include/pj_runtime/BatchEngine.h` |
| `OnDemandService` | `on_demand` | one bundle at a requested time `t`; series and explicit evaluations through `GridRunner` | `EnginePool` worker (Python on its own lane) | `pj_runtime/include/pj_runtime/OnDemandService.h` |

**How a script output reaches a scene layer.**
1. A plugin creates a recipe (`create_data_processor_v2` → `DataProcessorsKindRouter` →
   `OnDemandRuntimeHost`), or a layout restores one (`DerivedObjects::restore`).
2. `OnDemandService::install` upserts it into `RecipeGraph` (bindings, DAG, the script compiled
   once per revision) and `ResultHistory::install` registers its output topics, `<key>/<output>`,
   with the metadata of `derivedTopicMetadata` (`DerivedTopics.h`, `pj_derived: "on_demand"`).
3. The catalog lists the new topics; the user drops one on a dock, whose `tryAcceptObjectTopic`
   adds a layer. The displayed topic enters `TopicDemandTracker`'s active set: the node is demanded.
4. `AppSession` requests each demanded node at the playhead (`NodeScheduler::requestAt`); live
   data requests it at its live edge (`OnDemandService::followLiveEdge`).
5. `NodeScheduler` keeps one run plus one pending request per node, requests upstreams first, and
   asks `InputStager::stage` to resolve the inputs by identity on the GUI thread. A hit is served
   from `ResultHistory`; a miss reserves room (`InFlightBudget`) and submits to `EnginePool`.
6. On the worker: `InputStager::decode`, the script run (`runOnDemand`), the outputs serialized.
7. On the GUI thread: `ResultHistory::publish` writes the outputs into the ring with one
   `ObjectStore::pushOwnedBundle`, and the session emits `derivedEntriesPublished` and
   `derivedStateChanged`.
8. The layer reads the newest entry at or before the cursor (`ISceneLayer::refreshAt` on a
   publication); the dock's row shows the node's state and lag (`derivedTopicPresentation`).

**Series:** a node with object inputs and number outputs is replayed by `SeriesRunner` in chunks
(`GridRunner::submitRows`) into a `DataEngine` topic, which a plot curve reads like any series.
**Markers:** `BatchEngine` publishes a `PlotMarkers` set into a `__markers__/` object topic that
`PlotMarkersItem` draws on the plots its scope names.

**Reading order:** this page, then [ARCHITECTURE.md](ARCHITECTURE.md) §1-§4, then the section for
the area you change (§21 has recipes for the common changes, §20 the known limits), then its
header. The decisions D1-D30 are in [REQUIREMENTS.md](REQUIREMENTS.md).
