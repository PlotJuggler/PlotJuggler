# pj_scene_common: architecture

The layered scene dock shared by `pj_scene2D` and `pj_scene3D`. `SceneDockWidget` owns an ordered stack of
`ISceneLayer`s, one per object topic, and everything that does not depend on a renderer: tracker time, repaint
gating, one-dataset-per-dock, layer persistence and deferred restore, derived-topic presentation. A family
(`Scene2DDockWidget`, `Scene3DDockWidget`) subclasses the dock, supplies the view and the layer types, and keeps
rendering state to itself.

## Components

- `ISceneLayer` (`scene_layer.h`): one topic in a scene: `info()`, `timeRange()`, `setTrackerTime()`,
  `renderKey()`, visibility, config UI, XML state. Plus `SceneWarningParts` (a row warning with one slot per
  writer, joined in enum order), `liveTopicTimeRange()` and `latestEntryRenderKey()` for single-topic layers, and
  `derivedTopicPresentation()`.
- `LayerFactory` (`layer_factory.h`): a family's registry from object type to layer creator.
- `SceneDockWidget` (`scene_dock_widget.h`): the dock. It is an `IDataWidget` and `IObjectViewer`, so the shell
  drives it through `onTrackerTime`, `retryPendingRestores`, `finishRestore` and `revalidateObjects`.
- `layer_params.h`: `serializeLayerParams()` and `applyLayerParams()` round-trip a layer's `xmlSaveState` through a
  string, for copy and paste of settings between layers of one family. The caller gates on
  `SceneLayerInfo::family_name`; a foreign blob is applied as-is and ignored by the layer.

## Layer life cycle

`addTopic` (`tryAcceptObjectTopic`) goes through `addLayer`: refuse a duplicate topic, refuse a foreign dataset,
offer the topic to `handleSceneConfigTopic` (a family may consume it as scene-wide config, such as TF, with no
layer), then `acceptsObjectType` and the factory. `insertLayer` then attaches the layer, wires its signals,
registers it (draw order, visibility cache, tracker time seeded from the playhead clamped to the layer's own range)
and reconciles the view before it emits `layerAdded`. A family inserts layers that no store topic backs (3D trails)
through the protected `insertLayer` under a synthetic id; it skips the config-topic offer and the dataset check.

Removal order is fixed: drop the layer from the maps, `syncViewLayers` (the view releases its GPU state for it),
`detach` (tears down its meshes and textures), then `layerRemoved`. The layer outlives the notification, so a slot
that paints never touches a destroyed source. `clearLayers` does the same for all layers.

- A concrete dock whose view holds raw layer pointers calls `clearLayers()` in its own destructor. The base
  destructor cannot reconcile the view (`syncViewLayers` is pure virtual) and only detaches and destroys; a view
  still pointing at the layers would dereference freed memory.
- Hidden layers get no tracker ticks, so `setLayerVisible(true)` re-delivers the playhead (clamped per layer).
- The view is created by a queued single-shot. Restore code that writes view state synchronously (fixed frame,
  camera) calls `ensureSceneViewCreated()` first, or the writes find no view.

## Tracker time and range

`onTrackerTime(seconds)` takes display seconds: absolute time minus the display offset of
`representativeDatasetId()` (the first dataset among the render layers; a dock whose time-bearing topics are not
layers, such as a TF-only 3D dock, overrides it, or its offset reads as zero). Non-finite input is dropped.

The tracker is clamped to the union of the layers' clock ranges (`clockRangeOf`: a derived topic's input range,
else `timeRange()`). A spanning layer (first < last) bounds both ends. A latched layer (first == last, a map pinned
to the recording start) lowers the minimum but caps nothing: otherwise its lone early stamp drags a live playhead
backwards and hides everything keyed to "now". Empty layers and findings report an inverted range and bound nothing.
`timeRange()` must be the data as it stands now: a cached range pins the clamp to an evicted time and freezes the
scene while streaming.

## Repaint coalescing

Playback ticks at the display rate while scene data changes far less often, so `onTrackerTime` skips the layer
advance and the repaint when nothing a layer would draw has changed.

- Each layer's `renderKey(time)` fingerprints what it would draw at `time` (its active sample plus any positioning
  transform), computed without decoding payloads. `latestEntryRenderKey()` is the single-topic form: the store
  entry's UID, so a same-stamp replacement still changes it. The default is the time itself, which never coalesces.
- The dock combines the visible layers' keys, each mixed with its topic key, by XOR, which is independent of map
  iteration order, and folds in `viewRenderKey()`. Unchanged key: no `setTrackerTime`, no `refreshView`.
- Content the view draws outside any layer (the 3D view's TF triads and connection lines) reaches the gate only
  through `viewRenderKey()`; without it that content freezes whenever every layer key is unchanged.
- The gate covers the tracker path only. Settings and async changes reach `refreshView` through `repaintRequested`.
  A family therefore drives its per-tick view work (render-time push, scene bounds) from `refreshView`, not
  `onTrackerTime`, so it is gated with the repaint.
- A structural change (add, remove, visibility, clear) calls `invalidateTrackerRenderKey()`. A subclass that paints
  outside the gate (a live-edge push that drives the layers directly) calls it after that paint, or the stored key
  lags the pixels on screen.
- A layer whose `renderKey` misses a change shows a stale frame until another layer's key moves.

## One dataset per dock

A dock has one display clock (and in 3D one TF tree), so `addLayer` and `deferTopicIntent` refuse a topic whose
dataset differs from `representativeDatasetId()` (zero on either side never refuses). `refuseForeignTopic` emits
`topicRefused(reason)` with a sentence naming both datasets; the shell shows it as a toast. Refusals within one
event-loop turn (a multi-select drop, a mixed layout) emit once; `takeRefusalReason()` is never coalesced, for a
caller adding topics one at a time. Restore code calls the same helper, so a layout that mixed datasets still loads
and the foreign element is dropped with the same reason instead of failing the restore.

## Persistence and deferred restore

`xmlSaveState` writes a `<scene version="1">` (the family's `xmlTag()`) with one `<layer>` per layer in draw order:
`dataset_id`, `dataset_source`, `dataset_path`, `topic_name`, `object_type`, `display_name`, `visible`, `order`, and
the layer's own `xmlSaveState` payload as its child. The dataset id alone is not stable across sessions, so the
source and path are saved too and `resolveObjectDataset` resolves the identity (the injected resolver, else the
session's ambiguity-safe ladder; ambiguity resolves to nothing).

`xmlLoadState` validates the child tags and each layer's `visible` and `order` first, so a malformed state is
rejected before anything is cleared. A layer whose dataset or topic is not loaded yet is kept as detached XML in the dock's pending queue and
retried by `retryPendingRestores`. A permanently rejected element (malformed dataset id, bad object type, type mismatch, rejected payload)
counts toward `workspaceRestoreFailed()`; a pending one does not. `workspaceChanged` reports committed persistent
mutations and is suppressed while a restore replays (`beginWorkspaceRestore`). `unresolvedBlockingPendingRestores`
separates saved state that blocks an exact restore from advertised-topic intents (`deferTopicIntent`, marked
`pending_intent`), which are durable workspace state. The topic-demand binder may hold demand for pending entries
(`pendingRestoreDemands`) but never materializes them. `discardPendingRestoresFor*` match the `dataset_id` and
`source_dataset_id` attributes anywhere in a payload. A family that defers its own element kinds (3D's
`<config_topic>`) shares the queue and overrides `restoreOnePending`.

A load that fails can be rolled back with `capturePendingRestoreState` / `restorePendingRestoreState`; the
snapshot carries the open dataset choice too, or reloading the previous state would arm a gate that no settle
follows.

### Which dataset wins a mixed layout

The saved layout decides, not load order. `xmlLoadState` records the distinct datasets its `<layer>` and
`<config_topic>` children name (`recordSavedDatasets`; unnamed and advertised-intent elements do not count).
`datasetForSavedElement` resolves each element: while the first saved dataset has not resolved nothing of a later
one binds; once it has, elements of any other dataset are refused. `finishRestore()` settles the choice after the
load is final (`MainWindow::settleSceneRestores` after a whole layout, `DockWidget` after a paste or family
switch): the dataset the dock already shows wins, else the first saved dataset that is loaded; pending elements of
the losers are dropped with a reason, the winner's stay pending until their topics arrive. A winner's topic that has
not arrived resolves to nothing, which is not "another dataset". With nothing loaded at settle there is no winner and
the first dataset to resolve afterwards wins. Runtime pending intents and single-dataset layouts are not gated.

## Derived topics

A layer reads an on-demand derived topic like any store topic. The dock adds what the node's state says
(`derivedTopicPresentation`): the layer's derived warning part (missing input, error), a row status
(`layerStatus` / `layerStatusChanged`: the scheduler's lag, "computing…", a finding's computation time in display
seconds) and a status strip (`derivedStatus`, the largest lag of the visible derived layers). The presentation is
re-read on state change and display-offset change, never per frame. A published result calls
`ISceneLayer::refreshAt` on the layers of its topics at the dock's time, so a result shows with no tracker tick;
a layer that skips a repeated time overrides it to drop that memo first. Design: `docs/derived_objects/ARCHITECTURE.md`
"Scene integration".

## Recipes

- **New layer type in a family:** implement `ISceneLayer`, register a creator on `layerFactory()`, return the type
  from `acceptsObjectType`, and override `renderKey` (`latestEntryRenderKey` for one topic) or the layer repaints
  on every tick.
- **New family:** subclass `SceneDockWidget`: `createSceneView`, `makeContext`, `acceptsObjectType`,
  `syncViewLayers`; call `clearLayers()` in the destructor; override `xmlTag`, `viewRenderKey`, `refreshView`,
  `representativeDatasetId` and the restore hooks as the view needs.

## Known gaps

- A dock cannot show two datasets: the display offset, clock and TF tree are per dock, so a foreign topic is refused
  and the offset is converted once, not per layer.
- With nothing loaded when a restore settles, load order decides the winning dataset.
- `representativeDatasetId()` finds the dataset through render layers only; a dock with config topics alone must
  override it or its tracker time is read without the display offset.
