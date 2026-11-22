# pj_scene_common

The backend-agnostic layered scene dock framework shared by the scene families (`pj_scene2D`, `pj_scene3D`):
`SceneDockWidget` owns an ordered stack of `ISceneLayer`s, one per object topic, plus tracker time, repaint gating,
one-dataset-per-dock, layer persistence and deferred restore. Rendering-specific view state belongs in the
families. Links `pj_runtime` and `pj_datastore`. How it works: [docs/ARCHITECTURE.md](./docs/ARCHITECTURE.md).

## Invariants

- A layer's `timeRange()` is the data as it stands now (`liveTopicTimeRange`), never a value cached at the last
  render: the dock clamps the tracker to it, and a stale range freezes the scene while streaming.
- A layer's `renderKey()` is cheap (no payload decode) and changes whenever its output would. A key that misses a
  change leaves a stale frame; the default (the tracker time) is safe but never coalesces.
- A family drives its per-tick view work from `refreshView()`, not `onTrackerTime()` (so it is gated with the
  repaint); content the view draws outside any layer goes through `viewRenderKey()`, or it freezes while the layer
  keys are unchanged. A paint outside the gate calls `invalidateTrackerRenderKey()` afterwards.
- A concrete dock whose view holds raw layer pointers calls `clearLayers()` in its own destructor: the base
  destructor cannot reconcile the view, which would be left pointing at freed layers.
- Layers are removed in the order: view re-pointed, `detach()`, `layerRemoved`. The view must not reference a
  layer's caches when `detach()` tears them down.
- A dock shows one dataset: `addLayer` and `deferTopicIntent` refuse a foreign topic through `topicRefused`, and
  restore code calls `refuseForeignTopic` for saved elements, so a mixed layout loads with a visible reason
  instead of failing. The saved layout, not load order, picks the winner; the host calls `finishRestore()` after a
  restore is final.
- Pending (deferred) restore elements are detached XML until the object resolver can bind them. Restore code that
  writes view state synchronously calls `ensureSceneViewCreated()` first, or the write finds no view.
- `workspaceChanged` goes through `notifyWorkspaceChanged()`, which is silent while a restore replays.
- Every restore and every `xmlValidateState` reads a saved `<layer>` / `<config_topic>` header through
  `parseSavedLayerHeader` / `parseSavedTopicHeader`, and checks a payload with its type's `LayerFactory` validator:
  a second reader drifts from the first, and the MCP `widget_state` tool then lists or accepts what a load refuses.
- Attributes named `dataset_id` / `source_dataset_id` anywhere in a deferred payload are dataset references, and
  family payloads follow that: `discardPendingRestoresFor*` match exactly those.

## Key headers (`include/pj_scene_common/`)

- `scene_layer.h`: `ISceneLayer`, `SceneLayerContext`, `SceneWarningParts`, `liveTopicTimeRange`,
  `latestEntryRenderKey` / `kNoSampleRenderKey`, `derivedTopicPresentation`, `topicKey`, `resolveSavedObjectTopic` /
  `uniqueObjectTopic` / `objectTopicsMatching` (bind a saved reference; ambiguous stays pending).
- `scene_dock_widget.h`: `SceneDockWidget`, the dock base a family subclasses.
- `layer_factory.h`: `LayerFactory`, a family's object-type registry: layer creator and payload validator.
- `saved_layer_header.h`: `parseSavedLayerHeader` / `parseSavedTopicHeader`, the one reporting reader of a saved
  `<layer>` / `<config_topic>` header (`SavedTopicRef` sets the topic rule).
- `layer_params.h`: `serializeLayerParams` / `applyLayerParams`, copy and paste of a layer's settings as XML.

## Tests

`./test.sh -R '^pj_scene_common_tests\.'` (`tests/scene_common_test.cpp`, offscreen `QApplication`, one process per
case). The derived-topic tests use the fake script run in `pj_runtime/tests`.

## Docs

[docs/ARCHITECTURE.md](./docs/ARCHITECTURE.md): layer life cycle, tracker time, repaint coalescing, one dataset per
dock, persistence and restore, derived topics, recipes for a new layer type or family, known gaps.
