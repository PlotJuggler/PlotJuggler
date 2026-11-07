# pj_runtime: architecture

How the runtime services are wired to each other and to the shell, and the contracts
that are not obvious from one header. Each public header's doc-comment owns its API
contract; the module entry point is [../CLAUDE.md](../CLAUDE.md). The derived-data
engines (on-demand, batch and series, scheduling, TF clocks) are designed in
[docs/derived_objects/ARCHITECTURE.md](../../docs/derived_objects/ARCHITECTURE.md),
cited below as "derived_objects §N".

## Link surface

Public (`pj_runtime/CMakeLists.txt`): `Qt6::Core`, `Qt6::Network`, `Qt6::Xml`,
`pj_datastore`, `pj_tf`, `pj_marketplace` (not on WebAssembly),
`plotjuggler_sdk::plugin_host`, `nlohmann_json`, and `pj_scripting`.

- `pj_tf` is public because `TransformService.h` exposes `pj::tf::TransformBuffer` and
  `pj::tf::TransformSnapshot`; `glm` reaches `pj_runtime` transitively through it.
- `pj_scripting` is public because `RecipeGraph.h` exposes `scripting::OutputSpec` and
  `ResultHistory.h` `scripting::ScriptBundle`; `DataProcessorService` routes by-id
  `applyFilter` through the Luau filter catalogue and `BatchEngine` runs rules through
  `runMarkerScript`.
- On `EMSCRIPTEN` the `pj_marketplace` link is dropped and only `pj_marketplace/include`
  is added as a private include directory, so `DiagnosticHistory`'s lightweight
  `QtDiagnosticBridge` type still compiles.

Private: `plotjuggler_sdk::source` (not on WebAssembly; the source-cache mechanics
`SourceCacheStore` wraps), `tsl::robin_map`, `pj_internal_fmt`, and `mcap::mcap` (not on
WebAssembly). Only the recording **sink** is platform-bound: `find_package(mcap)`, the
`McapRecordingWriter` sources and the tests that read an MCAP back sit inside
`if(NOT EMSCRIPTEN)`, while `RecordingService`, `Recorder` and the recording interface
headers build everywhere. No compile definition is exported; `pj_app` gates its Record
UI on `RecordingService::isSupported()` alone.

## Session and data lifecycle

### AppSession

- **Source Timeline support.** `datasetRawTimeRange(DatasetId)` gives a dataset's raw
  `[min,max]` over its catalog-visible topics (the bar extents of `pj_app`'s
  `SourceTimelineController`). `recomputeRange()` updates only the `PlaybackEngine` range
  from the offset-adjusted union of visible topics; it is called throttled on the drag
  path, never resets `currentTime` while data remains, and returns the union's
  display-min, which `seedPlaybackFromSession()` reuses for the first-load playhead snap.
  When no visible data remains it collapses the transport with `resetPlaybackToEmpty()`
  (pause, `[0,0]` range, cursor 0), the same reset the `CatalogModel::cleared()` hook runs
  on a full wipe: this is what stops playback when the last dataset is removed. Both scans
  share the private `forEachVisibleRawRange`, so the visibility and dedup rules live in one
  place. The playback range skips host-published topics (`isOffClockTopic`).
  Zero-time reference geometry is also outside the clock: a dataset containing a point
  cloud or mesh whose entire scalar/object range is `[0,0]` remains drawable as a static
  reference, but contributes neither playback bounds nor the global relative-time origin
  (`SessionManager::isStaticReferenceDataset`). Loading it beside a recording does not
  create a gap back to epoch zero. Its stored samples are unchanged.
- **Merge.** `mergeDatasets(selected)` destructively folds the selected datasets into one:
  the anchor (leftmost on the display axis) keeps its clock and ids, the others are
  shifted into it by their relative display offset and removed from the catalog, scalar
  topics union by name, object topics fuse by name, and the anchor is relabelled
  `*_merged`. `objectMergeConflicts(selected)` is the builtin-object-type preflight (it also
  reports same-named marker topics whose scope, target or owner differ); a successful merge
  emits `datasetsMerged(anchor, consumed)`.
- **On-demand wiring** (derived_objects §8, §16): requests every demanded recipe at the
  playhead in its dataset's raw time; forwards the demand tracker's active sets
  (`OnDemandService::setDemand`); requests a recipe installed or newly demanded at the
  parked cursor (a finding at its pin, once per install); re-requests a dataset's demanded
  recipes when its display offset changes; withdraws a recipe demand left; keeps the raw
  inputs of demanded recipes in the demand tracker's derived-input tier; injects the
  catalog's scalar lookup.

### SessionManager

- **Owned services.** The session's `CurveColorRegistry` (also reached through
  `AppSession::curveColorRegistry()`, cleared when the catalog empties), the
  `ObjectIngestTap` registry shared by file, streaming and delegated toolbox ingest, the
  `EnginePool`, the `InFlightBudget`, `BatchEngine` (`batchEngine()`) and
  `OnDemandService` (`onDemandService()`).
- **Display-offset model.** The total `displayOffset(id)` everything on the display axis
  reads = the per-source alignment shift (`sourceDisplayOffset(id)`, the `TimeDomain`
  offset the Source Timeline edits) **plus** one global `globalTimeReference()` (the
  earliest raw sample across all datasets when "Use time offset" is on, else 0, applied
  uniformly so cross-dataset gaps are preserved). `setUseTimeOffset` flips only the global
  reference and never writes per-source offsets, so toggling never moves the Timeline bars;
  `SourceTimelineController` bridges `globalTimeReference` across the playback/timeline
  frame gap. `setDisplayOffset(DatasetId, DisplayOffset)` is idempotent (an unchanged
  offset emits nothing) and emits the per-dataset `displayOffsetChanged(DatasetId)`, which
  `PlotWidget` (drops that dataset's adapter offset cache, refreshes zoom-out extent and
  magnifier limits, replots at current zoom) and `SourceTimelineController` (bar offsets,
  throttled `recomputeRange`) consume. The no-arg `displayOffsetChanged()` is the global
  frame's signal; connect either with `qOverload<>` / `qOverload<PJ::DatasetId>`.
  `displayTimeForSource(DatasetId, absolute_ns)` maps a raw instant onto the display axis
  (nullopt for an unknown dataset): the `pj.playback.v1` conversion slot.
- **Dataset removal.** `removeDataset(DatasetId)` erases the dataset's scalar storage from
  the `DataEngine` and invalidates the time-origin caches, so `globalTimeReference()`
  re-bases to the survivors; when the origin actually moves it emits
  `displayOffsetChanged()` so surviving curve adapters drop stale offsets and re-fit. It
  also drops the dataset's source-path, source-record and loader-metadata entries and runs
  `BatchEngine::clearGeneratorsForDataset`. Calling `dataEngine().removeDataset` directly
  leaves the memoized earliest-sample origin stale.
- **Dataset identity registry.** `DatasetId` → normalized full path
  (`setDatasetSourcePath` / `datasetSourcePath`, written by `pj_app`'s `FileLoader`;
  `DatasetInfo::source_name` is only a display label and cannot tell same-basename files
  apart). `resolveDatasetIdentity(id, source, path)` never guesses by load order: exact
  live id (trusted only while the saved qualifiers agree) → unique full-path match →
  unique source-name match → reject with `DatasetIdentityResolution::ambiguous` (a nullopt
  id with `ambiguous=false` means "not loaded yet"). `resolveObjectDatasetIdentity` adds
  an object-topic-name tiebreak for same-file fan-out siblings.
- **Token-addressed progressive ingest.** `beginIngest(DatasetId, label, total,
  cancellable, cancel_fn)` returns an `IngestToken` (`quint64`; 0 = invalid). A new
  `beginIngest` on a dataset supersedes the prior token: the old token's later
  `updateIngest` / `endIngest` / `requestCancel` become silent no-ops, and the superseded
  run is reported as `ingestEnded(old, kCancelled)` before the new `ingestBegan`, so no
  observer infers a supersede or leaks state bound to the old token. At most one ingest per
  dataset is tracked, and `liveEntry(token)` is the one definition of staleness.
  `updateIngest(token, current, total)` publishes the dataset's flushed topics **before**
  the staleness gate (a publish-ordering slip can never drop data) and reports
  `ingestProgressed` factually, also after a stop was requested; freezing a stopping row is
  presentation policy in `IngestProgressController`. **Cancellation intent wins:** after
  `requestCancel(token, keep_partial)` whichever terminal arrives reports `kCancelled`;
  a cancel after the terminal is refused. `kUnknown` exists because the plugin C-ABI's
  `on_ingest_finished` carries no outcome code. Observers: `ingestActive(DatasetId)`,
  `activeIngests()`, and the `ingestBegan` / `ingestProgressed` / `ingestEnded` signals.
  View side: `pj_widgets`' `CurveTreeView` draws per-dataset progress with two stop
  affordances on a cancellable row (stop and keep, stop and discard), reported as
  `cancelRequested(row_key, keep_partial)`; honouring `keep_partial` is the producer's job.
  `pj_app`'s `IngestProgressController` keys each row by `IngestToken::id` and owns the UI
  lifetime policy (completion linger, error flash, ghost rows, frozen `kStopping` bar). The
  title-bar strip `IngestProgressWidget` is kept but not shown
  (`kShowTitleBarIngestStrip`).
- **Loader metadata.** `setDatasetMetadata` / `datasetMetadata` / `clearDatasetMetadata`
  hold one loader-supplied JSON document per dataset, attributed to the emitting plugin,
  bounded by `validateDatasetMetadata`; a rejection keeps the previous document and never
  affects ingest. Same lifecycle as the source-path and source-record registries: erased by
  `removeDataset`, invalidated for every contributor of a destructive merge, cleared by a
  committed refill (`RefillGuard::commit`). `FileLoader` publishes each load's staged
  document (`DataSourceRuntimeHost::stageLoaderMetadata`, fed by the SDK
  `set_dataset_metadata` slot) at the same commit seams that set source paths;
  `CatalogModel::datasetMetadata` is the read-only facade and the curve tree's Info dialog
  renders it generically.
- **Derived-engine wiring.** `removeEphemerals(transform_keys, generator_ids,
  on_demand_keys)` ends the listed previews still present and still ephemeral in the three
  engines, repainting overlays once when a marker preview went and announcing
  `objectTopicsChanged` once when on-demand topics retired; `DerivedRuntimeHosts`'
  destructor passes the previews its bridges created, so a preview ends with the plugin
  instance that made it. `DataProcessorService::setOutputsChangedSink` is wired to
  `BatchEngine::recomputeForChangedInputs(names, dataset)`, so a changed processor output
  marks the marker rules reading it stale.
- **On-demand engine wiring** (derived_objects §8, §13). `onDemandService()` is declared
  after the pool and the budget. The session wires its resolver, parser lookup, dataset
  epochs, TF providers, image decoder and video source (`setTransformService`,
  `setImageDecoder`, `setVideoFrameSource`; `pj_app` passes its `SceneImageDecoder` and
  `SceneVideoFrameSource`; `suspendOnDemand` retires the dataset in the video source) and
  `setScalarSeriesLookup` (called by `AppSession`; a scalar input names a scalar topic,
  whose first column it reads, else a series). Its callbacks become
  `derivedEntriesPublished`, `derivedStateChanged`, `onDemandRecipeInstalled` and
  `objectTopicsChanged`, and a series' rows become `notifyIngest`.
  `derivedScalarTopics(ephemeral_only)` lists the `DataEngine` topics the host derives
  (processor outputs and series): a refill does not treat them as the file's, the undo
  snapshot does not treat them as raw data, and the catalog lists them only when they are
  not previews. A live `notifyIngest` requests the recipes of the datasets it names at
  their live edge (`OnDemandService::followLiveEdge`) and keeps the TF snapshot brake on
  for that dataset. Refill, rollback, merge, eviction, clear and removal suspend, re-bind
  or remove recipes. `enginePool()` exposes the pool (`waitIdle` publishes everything
  submitted). `derivedTopicInputRange(topic)` is a derived topic's declared clock range,
  which the scene docks clamp to.

### Catalog and demand

- **`CatalogModel` demand surface**: advertised-but-unsubscribed placeholders
  (`setAdvertisedTopics`, data-less rows real data supersedes) and the per-dataset
  `setPerTopicPauseCapable` / `isPerTopicPauseCapable` flag, set by `pj_app`'s
  `StreamingSourceManager`, which gates every demand-driven affordance (dimming, census
  previews, the force-streaming menu) so file datasets are never shown as pausable.
- **Identity facade**: `datasetSourceName` (the raw `DatasetInfo::source_name`, not the
  deduped label), `resolveDatasetIdentity`, `datasetSourcePath`, so widget code needs no
  `FileLoader` dependency. `resolveCurveKey(...)` is the one three-tier curve resolver
  used by layout/undo restore (`PendingDisplayBinder`) and clipboard paste (`PlotWidget`);
  its `SeriesCapability` filters by consumer (`kPlottable` for plot curves, `kDiscrete` for
  State Transitions rows): integers and bool satisfy both, floats are plottable and also
  discrete when every retained value is whole (`TopicStorage::wholeNumbersOnly`), strings
  are discrete only. The catalog fingerprint mixes `TopicStorage::wholeNumbersSignature()`,
  so a float field's first fractional value rebuilds the catalog.
- **Marker and on-demand inputs**: `findScalarForMarkerKey(dataset, key, strings_too)`
  backs `AppSession`'s batch resolver and the on-demand scalar inputs; it walks items in
  place, dataset first, and memoizes until the items change. It relists on
  `onDemandRecipeInstalled` / `objectTopicsChanged` and reports preview recipe topics
  (`ephemeralPreviewKeys`), which user-facing trees leave out. `objectTopicKey` and
  `scalarColumnKey` are hash lookups from held ids to keys.
- **`TopicDemandTracker`**: active set per streaming dataset = displayed (ref-counted
  `addReference` / `removeReference`) ∪ infrastructure (`setInfrastructureTopics`, TF and
  CameraInfo) ∪ derived inputs (`setDerivedInputTopics`, set by `AppSession`) ∪ forced
  (`setTopicForced`, a tier, never an override: unforcing cannot pause a displayed topic).
  `activeTopicsChanged` fires only on change; `forcedTopicsChanged` fires even when the set
  does not change. `hasAnyReference` (displayed only) is the batch engine's demand gate and
  `referenceDemandChanged` fires when it flips. A **pending** display intent
  (`PendingDisplayBinder`) counts as a reference, or a restore bind would deadlock (no
  subscription, no data, binding never completes). `pj_app`'s `StreamingSourceManager`
  forwards the set to `DataSourceHandle::setActiveTopics` (only with
  `kCapabilityPerTopicPause`); `pj_app`'s `TopicDemandController` produces references from
  plots and scene docks and owns the bounded field-discovery previews (census of
  unclassified placeholders, at most 8 concurrent, once per topic per session; manual peek
  on double-click; a preview holds a reference until promotion or a ~4 s timeout). A
  preview-promoted topic's samples are disowned (`DataEngine::evictTopicHistory`) when a
  real reference arrives, so they never draw a bogus flat segment.

### Playback and view hosts

- `PlaybackControlHost` (`pj.playback.v1`): topic conversion rejects unknown or ambiguous
  topics (`CatalogModel::datasetForTopic`); the optional `to_display_time_for_source` slot
  selects a live catalog source handle. `MainWindow` supplies the per-source display
  offsets and rejects unloaded handles.
- `ViewportRuntimeHost` (`pj.viewport.v1`): zoom/reset scoped by `pj_app`'s
  `PluginPlotTabsController` to the calling plugin's tabs; times are display-axis seconds.
- `PlotTabsRuntimeHost` (`pj.plot_tabs.v1`): `PluginPlotTabsController` enforces ownership
  by stable plugin-scoped id, independent of title or tab order. Duplicate titles are
  allowed; recreating an id replaces its contents (a scene tab of the same kind is only
  retitled). Each tail slot is in the vtable exactly when its callback is set.
- `src/RuntimeHostSlot.h`: `host_slot::guarded<Self>(ctx, out_error, domain, body)` refuses
  a null `ctx` and turns an escaping exception into `PJ_ERROR_CODE_INTERNAL`;
  `reject` / `finish` fill `PJ_ERROR_CODE_REJECTED`. Used by the plot-tabs, viewport,
  playback, data-processors and markers bridges; only `ToolboxRuntimeHost`'s void slots
  and its `parserIngestFail` path stay outside it.
- `IDataWidget::finishRestore()` is called once the data a just-loaded `xmlLoadState`
  refers to is final: by `pj_app` after a layout load, by `pj_plotting`'s `DockWidget`
  after a paste or family switch into loaded data.
- `IObjectViewer` pairs with `CatalogModel`'s `cleared()` / `itemsRemoved()` signals.

## Plugin hosts

### DataSourceRuntimeHost

- **DSO keepalive.** A `shared_ptr<void>` from `DataSourceHandle::libraryOwner()` is
  threaded into every lazy payload anchor and fetch closure, deferring `dlclose` until the
  last one drops, so a decode worker never calls plugin code in an unmapped library.
- **Advertised topics** (`notify_available_topics`): each topic is classified up front via
  `ExtensionCatalogService::classifyParserObjectRoute` (object route only; no
  `bind(registry)` needed), falling back to the type name for `FrameTransforms` /
  `CameraInfo`. The optional `on_available_topics` callback fires on the **poll/stream
  thread**; `pj_app`'s `StreamingSourceManager` marshals it to the GUI thread and feeds
  `CatalogModel::setAdvertisedTopics` and `TopicDemandTracker::setInfrastructureTopics`.
- **Idempotent parser bindings.** `cbEnsureParserBinding` returns the existing binding for
  an unchanged request signature (topic, encoding, type, schema, parser config): a
  demand-driven plugin re-requests its binding on every re-subscribe, and
  `DataEngine::createTopic` mints a fresh topic per call, so a second binding would double
  every field in the catalog. A changed signature mints a fresh binding; a catalog reload
  alone never does. The scalar route winner takes the binding; the object winner supplies
  the object classification and the lazy re-decode instance.
- **Ingest completion.** `attach_source_record` (bounded JSON descriptor; the last attach
  before the first push wins; attaching after a push or after the seal vetoes capture) and
  `complete_ingest` (validated with the SDK's `copyIngestCompletion`; seals the context,
  later pushes are rejected and latched, identical repeats are idempotent, a conflicting
  or malformed terminal vetoes permanently). The host instance is the ingest generation.
  `sourceRecordDescriptor()`, `ingestCompletion()`, `captureVetoReason()` and
  `ingestCallbackFailures()` are what `SourceCaptureService::finalize` gates on; every
  `fail()` is latched, since recorded bytes would no longer match the dataset.
- **Record tap.** `setRecordTap()`: the tap is called inline on the push thread once per
  pushed message, before the parser runs, with a non-owning `RecordedBindingView` valid
  only for the call. Carrying the binding on every message lets a recording started
  mid-stream capture topics subscribed long before it. A `kPureLazy` push fetches no bytes
  and is counted in `recordTapSkippedLazy()` instead.
- `setDiagnosticSink` forwards the plugin's WARNING/ERROR reports (cap and threading in the
  header).

### ToolboxRuntimeHost

- Assembles `ToolboxHostService`, `ToolboxRuntimeHostService` and `SettingsStoreService`
  (persisted through `QSettingsBackend` into `PlotJuggler4.conf`; `/`-separated keys map
  to groups). App concerns are injected as `Callbacks`; the `[thread-safe]` callbacks
  marshal onto the constructing (GUI) thread.
- **Delegated parser ingest** (with `ParserIngestDeps`): `create_parser_ingest` /
  `release_parser_ingest` give a toolbox-created dataset the standard
  `DataSourceRuntimeHost` surface (one per dataset; release = flush and destroy,
  idempotent), driven from the toolbox worker thread, not GUI-marshalled.
  `discard_parser_ingest` is the rollback twin: the context is destroyed without
  `flushAll()`, pending rows die with their writers and no completed-import marker reaches
  the shell.
- **Progressive import.** Each unthrottled `progress_update` (default 50 ms,
  `setFlushThrottleMs`) flushes both write surfaces on the plugin's ingest thread, then
  fires the GUI-marshalled `on_ingest_started/progress/finished`, so the shell may treat
  every progress callback as "new rows are reader-visible" (`pj_app` mirrors
  `FileLoader::publishIngestProgress`). `on_data_changed` reports contexts created or
  released since the last notify plus every context mid-import, so a growing import keeps
  playback focus. `requestStopActiveIngests()` is a flag-only cooperative stop with
  keep-partial semantics (the toolbox ABI has no host-side rollback).
- **Source capture.** With `ParserIngestDeps::capture_service` and `capture_provider_id`,
  every fresh context is armed at creation. Release commits (the
  `SourceCaptureService::finalize` gate on the release caller's thread, then the store's
  `cleanup()`); discard and host teardown abort; a host-side stop makes finalize refuse. A
  record-worthy release fires `on_capture_finalized` on that thread with the descriptor,
  the `sourceCacheIdentityDigest` and the artifact path, so the shell can attach the
  dataset's `SourceRecord` and register the artifact as its backing file.

### Extension catalog and parser routing

- **`ExtensionCatalogService`** scans `--plugin-dir` (authoritative) → custom Preferences
  folders (authoritative) → the marketplace dir (managed) through `PluginRuntimeCatalog`.
  At construction it seeds the bundled dir `<prefix>/lib/plotjuggler/plugins` into the
  marketplace dir: copy when absent, staged refresh when the bundled version is newer,
  never downgrade a compatible install; an installed copy the host would reject (malformed
  version, or the ABI / `min_sdk_required` / `min_plotjuggler_version` gate, see
  `descriptorIncompatReason`) is overwritten by the bundled build. The bundled dir is never
  scanned as a load path. The bundled id→version map goes to the `ExtensionManager`
  (core-plugin uninstall lock) in default sessions only. Lookups return raw pointers
  valid until the next `reload()`, GUI thread only. Browser/static builds skip the
  marketplace and register a `StaticPluginSet`. The Custom Topics editor is the toolbox
  whose manifest `capabilities` lists `custom_topics_editor`; `toolboxBadge` /
  `toolboxDisplayName` name an owner on rows and owned-tab watermarks.
- **`PluginRuntimeCatalog`** resolves duplicate ids by authoritative → compatibility →
  version → folder priority, then hard-rejects a winner with a malformed version or an
  incompatible host contract. An authoritative copy still shadows every managed fallback,
  so a broken `--plugin-dir` build fails visibly instead of silently testing another
  plugin. This resolution is host policy; the SDK stays a thin discovery/loader layer.
  `registerStaticPlugins` applies the same gate to linked-in plugins (the WebAssembly
  path).
- **`ParserRoutingService`** (owned by `ExtensionCatalogService`): for encodings in the SDK
  claim registry the scalar and object routes resolve independently. Every parser holds
  the wildcard scalar claim for its encodings; exact per-type claims are discovered lazily
  per (provider, encoding, type, schema, config) through `pj.parser_route_claims.v1`, or
  legacy `classify_schema` (object route only). Ranking: exact > wildcard, then provenance
  (`kBundled` needs id, version and DSO signature of the shipped build; `--plugin-dir` and
  custom folders are `kFolderDrop`; else `kMarketplace`), then priority. Winners are
  instances prepared during the resolution, never shared between bindings; probe errors are
  evicted after each resolution so a transient fault retries. Encodings outside the
  registry select the first provider registered for both routes
  (`route_dispatch == false`). The service is externally synchronized:
  `ExtensionCatalogService` takes `routing_mutex_` **before** `catalog_mutex_` (shared for
  resolve/classify, exclusive in `reload()`), so one resolution never mixes catalog
  generations and queued resolutions never block a GUI reload. In `DataSourceRuntimeHost`
  divergent winners leave the in-parse object sink unregistered, and an object topic keeps
  its first decoder provider (history is never re-decoded by a replacement).
- **`FeatureCatalogHost`** (desktop only) backs the optional `pj.feature_catalog.v1`
  service: it composes the schema-1 JSON document (built-ins and limitations from the
  resource bytes its constructor takes, loaded plugins, installed-not-loaded extensions,
  installable entries of a registry the app already fetched; it never fetches) from
  injected providers, so `tests/feature_catalog_host_test.cpp` runs it over fakes. Each
  extension id lands in exactly one list (loaded > installed_not_loaded > available); an id
  past the bound is dropped, never truncated, other strings are bounded and lists sorted
  and capped. The registry state is kept per URL from every fetch the app makes (the
  startup scan and the Marketplace window, each through its own `RegistryManager`, wired
  by `MainWindow::trackRegistryFetches`); the last fetch of a URL to finish decides its
  state. `ToolboxTags.h` holds the manifest tags (`cloud`, `hidden`) the Toolbox menu, the
  Sources panel and the catalog's default entry points all read (`manifestTags`,
  `manifestHasTag`).

  **Schema 1.** The SDK leaves the document to the host, so this is its definition. A
  missing field means unknown.

  | Key | Value |
  |---|---|
  | `schema` | `1` |
  | `host_version`, `platform` | strings |
  | `core` | built-in features: `id`, `name`, `summary`, `entry_point` |
  | `limitations` | known limitations: `id`, `summary` |
  | `loaded` | plugins in this process: `id`, `name`, `version`, `category`, `summary`, `entry_point`, `in_registry` (bool) |
  | `installed_not_loaded` | `id`, `name`, `version`, `category`, `summary`, optional `reason` (`disabled`, `needs_restart`, `incompatible`); no `entry_point` |
  | `available` | installable registry entries: `id`, `name`, `version`, `category`, `summary`, `entry_point` |
  | `registry` | `state` and `reason`, both always present |

  `entry_point` is end-user navigation text: the plugin's own declaration, else the host's
  default, which is empty for a plugin with no place of its own (a `hidden` toolbox, an
  unknown category). Lists are sorted by `id`.

  | `registry.state` | `registry.reason` | Meaning |
  |---|---|---|
  | `fetched` | `""` | the last fetch of the current URL succeeded; `available` and `in_registry` are read from it |
  | `pending` | `""` | a fetch of the current URL is running and no list is in hand |
  | `unreachable` | `""` | the last fetch of the current URL failed, for a network or a parse error alike |
  | `not_fetched` | `url_changed` | fetches happened, none of them of the current URL |
  | `not_fetched` | `updates_disabled` | the startup scan was skipped and nothing else fetched |
  | `not_fetched` | `""` | nothing fetched yet |

### Widget plugins

- **Catalog.** `widgets()` exposes `RuntimeWidgetPlugin` (library, id, name, version, per-type `type/name/icon/settings_json`, `qt_compatible`, `qt_mismatch_reason`). Two rules differ from the other families. (1) A Widget plugin **never leaves the catalog except by a user disable**: one policy point, `unloadableFamily(PluginFamily)` in `PluginRuntimeCatalog.cpp`, is consulted by every drop/evict/erase path. So `reload()` does not drop it when the file disappears and does not evict or reload it when its mtime changes (it reports once `<id>: updated on disk; restart required`), `scanDirectory()` keeps what is loaded, and a static registration cannot supersede a loaded DSO. A plugin whose id is now in the disabled list is withdrawn from `widgets()` by `reload()` (panels already running it keep working; enabling it again brings it back at the next reload). The library itself is never `dlclose`d: the SDK loader (`WidgetLibrary`) keeps it mapped for the life of the process, because a Qt widget created by the plugin can be called or destroyed after the catalog entry is gone. A library that appears for the first time is still loaded hot. (2) Before `create_qwidget` is ever called, `build_info()` is compared with the host: Qt major.minor (`QLibraryInfo::version()`, patch ignored), compiler family (`PJ::widgetCompilerFamily()`, the same SDK function the plugin side uses), and on MSVC the debug/release flag. A mismatch does not discard the library: it is registered with `qt_compatible=false`, a user-facing `qt_mismatch_reason` ("this widget needs Qt X.Y (PlotJuggler uses A.B)") and a `kError` diagnostic, so the dock can explain the missing widget; its types must not be instantiated. Only for a compatible build, `qt_modules` is cross-checked against `src/ShippedQtModules.h` (derived from the SDK's `PJ_WIDGET_QT_MODULES` X-macro) and anything outside it is a `kWarning`.
- **`ActiveWidgetsMarker`.** The on-disk record of which Custom Widget plugins have live code in this process (one file per process, `<AppLocalDataLocation>/active_widgets.<pid>.json`, `{"version":1,"widgets":[{"plugin","type"}]}`), so a crash inside a widget can be named at the next start. `add(owner, plugin_id, type)` / `remove(owner)` are keyed by the panel and rewrite this process's file atomically (`QSaveFile`) on every change, deleting it when nothing is live; `clear()` is the clean exit and removes only this process's file. `read(path)` turns a missing file into no entries and an unreadable or malformed one into no entries plus `error`. `take(directory)` is the startup review: it reads and deletes every record whose pid is not alive (`isProcessAlive`: `kill(pid, 0)` succeeds or fails with `EPERM` on Linux/macOS, `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)` + `GetExitCodeProcess == STILL_ACTIVE` on Windows; the probe is injectable for tests) and merges their entries and errors, so a second instance running side by side is never reported as a crash and a dead run's record is consumed once. A pid reused by an unrelated live process hides its file until that process ends. Qt Core only, GUI thread. `pj_custom_widgets` writes it, `pj_app`'s `CustomWidgetController` owns it and reviews it at startup.
- **`WidgetRuntimeHost`.** Per-panel host side of `pj.widget_host.v1` over the SDK's `WidgetHostBridge` (which supplies the C-ABI skeleton, so `RuntimeHostSlot.h` is not used). `history` resolves the wire (input, slot) through a dock-supplied `WireResolver` to a `ResolvedWire` (the `WireTarget` plus the series label every value refers to) and reads it: numbers through `DataReader::series`, text through one `rangeQuery` cursor (a first pass keeps the sample rows, the second reads only the strings it delivers), objects through `ObjectStore::rangeByTime`; samples go to the plugin's sink in ascending time on the display axis (the target's `DisplayOffset`; the requested range saturates instead of overflowing), capped at `max_samples` (0 = 10 000, never above 100 000) by even decimation that keeps the first and last sample; it returns the number of samples the range held before the cap (the ABI's `out_total`, saturated at `UINT32_MAX`), and stops delivering as soon as the sink returns false. The sink runs under the engine lock for scalar reads. `inputs_changed` and `report_message` go to the `Callbacks` given at construction. Qt-free; one instance per panel, must outlive the `WidgetHandle` bound to it.
- **`WireTarget`.** Where one Custom Widget wire reads from (kind, scalar topic + column or object topic + type name, display offset). Owns no string (the object type name is `sdk::name`'s static storage), so the dock copies one per wire per tick without allocating.
- **`WidgetValue`.** `baseWidgetValue(input, slot, kind, time, label)`: the common fields of every `PJ_widget_value_t` the host builds, shared by `WidgetRuntimeHost` (history) and `pj_custom_widgets` (the per-tick values).

## Data processors

### DataProcessorService

- **Two tiers of one substrate.** Filters (per curve, hidden output, keyed by output topic)
  and transforms (named, plugin-owned, session-persisted, keyed `"<plugin>/<id>"`).
  Transform recipes capture dataset/source/topic/field input identity; a replacing reload
  uses `rebindAndRecomputeForReplacedSources` to rebind columns in place, keeping output
  TopicIds. Compatible script edits replace operators transactionally, structural edits
  are rejected before mutation, removal cascades by exact TopicId. The ABI `config` carries
  `gaps` (input rows retention evicted before the node consumed them).
- **History.** `TransformRecipe::history_exempt` (and `GeneratorRecipe::history_exempt`)
  makes a processor immune to undo/redo: persisted in layouts, never removed, recreated or
  replayed by a history restore. Restores keep unchanged processors live so output TopicIds
  survive: `unchangedTransformKeys(wanted, intent)` (binding-less legacy entries never
  match) and `unchangedFilterOutputs(wanted)` (matched on `PersistedFilter` by
  (dataset, `output_name`); output names are unique per dataset only). Both clears and the
  preflight derive from one private removal set; `removalImpact(intent, ...)` walks its
  cascade once and returns the exempt outputs it would strand and every output name it
  removes (a kept processor reading a removed output falls too and returns under a new
  TopicId), which `BatchEngine::exemptDependentsOf` checks (a history restore rejects
  before clearing anything) and `BatchEngine::unchangedGeneratorIds` reads.
- **Previews.** `removeEphemerals(keys)` removes listed previews still ephemeral; no clear
  or restore removes a preview unless a removed input cascades into it. A persistent
  transform or a filter reading a preview's output is refused.
- **Outputs-changed sink.** Fires with the topic names and dataset of changed outputs after
  an upsert, in-place edit or restore that leaves the output complete, after `applyFilter`
  / `updateFilter`, when a time-sliced replay ends, and before a removal, chained
  processors included. A reload's rebind does not fire it: `RefillGuard::recomputeProcessors`
  runs it and then re-runs the dataset's marker rules once. `materializedTopicNames(recipe)`
  is the vocabulary series keys are rooted at (a grouped `"t:a,b"` is one topic `"t"`).
- **Time-sliced create.** A new persistent transform replays only the first
  `kMaterializationSliceBudget` slice inside the call (so an early script failure is still
  rejected) and finishes from a zero-timer via `stepMaterialization`, oldest deferred node
  first (`IncrementalEngine::deferredNodes()` / `materializeStep`), chained processors in
  the same budget. New rows are announced through `SessionManager::notifyIngest` at most
  every `kMaterializationNotifyInterval`, and at once on completion or failure. Remove,
  undo, in-place edit and reload end a pending replay. Ephemeral creates, restores, filters
  and edits stay synchronous. The browser build runs Qt on the page's main thread (no
  `PROXY_TO_PTHREAD`), which is why a long replay must yield. Whole-series GUI readers call
  `DataEngine::awaitCompleteSeries(topic)` (the toolbox `read_series_arrow` bridge,
  `pj_app`'s marker-generator series resolver); the barrier does nothing on another thread,
  where a toolbox worker gets the current, possibly partial, rows. Known limit: only a new
  single-input node's first replay is sliced; restores, edits, reload recomputes and
  multi-input replays run to completion inside the call.
- **Failures.** After every scheduling pass, a transform whose node newly failed
  (`IncrementalEngine::nodeError`) is reported once through `setDiagnosticSink` (source
  "Data Processors", id = transform key) and again only after it recovered; `AppSession`
  passes the app's sink, so it lands in the notifications bell.

### Plugin bridges

- **`DataProcessorsRuntimeHost`** (transforms): a persistent create returns after the first
  slice; a preview create stays synchronous because its caller reads it right back. A
  created transform outlives the bridge: only an explicit `remove`, a session close or, for
  its own previews, the end of its plugin instance tears it down. `create_data_processor_v2`
  refuses a time flag, typed outputs or `dataset_id`, naming the kind.
- **`MarkersRuntimeHost`**: accepts only `kind=markers`; ids are namespaced
  `"<plugin>/<id>"`. `config` adds `computing` and `error` (older plugins ignore them).
  `create_data_processor_v2` takes WINDOW (inputs clipped; `config` reports `coverage_ns`)
  and refuses INSTANT, typed outputs and `dataset_id`. Its generators outlive it, except its
  previews.
- **`OnDemandRuntimeHost`** (derived_objects §18): typed create only (v1 refused naming v2;
  INSTANT pins a finding; WINDOW refused; `dataset_id` binds `Recipe::dataset`), list
  without previews, config with the contract's state fields, validate and the three-way
  check (`scripting::checkOnDemand`: Luau compiles; Python compiles on the helper, waited
  for at most `kPythonValidateWaitMillis`, else UNCHECKED), and submit/poll/release over
  `GridRunner` (at most 64 handles and 64 MiB of reserved reports per bridge, one
  evaluation in the runner at a time). A preview key is `<plugin>/~preview/<n>/<id>`.
  Destruction cancels its jobs; it must outlive the router holding it.
- **`DataProcessorsKindRouter`**: routes each slot by `kind`; kind-less slots go to the
  bridge whose `owns` probe holds the id; cross-kind id collisions are refused. Typed
  requests are decoded with the SDK decoders before `kind` is read; evaluation handles are
  renumbered into the router's namespace. `check_data_processor_script` forwards to the
  backend's check or maps its validate (`host_slot::checkByValidate`).
- **`DerivedRuntimeHosts`**: built by `pj_app`'s interactive toolbox session and by
  `HeadlessDescriptorProviderSession`, so a headless provider sees the same surface. The
  markers bridge binds a create to the dataset its keys name (`resolveMarkerDataset`).

### Dataset-qualified names

`DatasetQualifiedName.h` uses the SDK's `sdk::splitDatasetQualifier`: the longest loaded
prefix wins; colons in both parts can prevent round-tripping, so it is not a persistent
identity. `resolveMarkerDataset` normalizes qualified keys to bare form, refuses a bare
input found in several datasets (suggesting the qualified forms), and rejects inputs absent
from the chosen dataset; `MarkersRuntimeHost` runs it only for single-dataset generators
(under `{"scope":"all"}` keys are literal and a key whose prefix names a loaded source is
rejected). A stripped qualifier is kept in `GeneratorRecipe::declared_inputs` (persisted
as `<input declared=…>`) so the script reads `series("src:key")`. A generic layout's
generator (no dataset) binds on restore to the one loaded dataset whose catalog carries
every input (`CatalogModel::findScalarForMarkerKey`; a rule with no inputs binds only when
exactly one dataset is loaded; otherwise a `generator-restore-failed` diagnostic). Transforms
use the same splitter in `DataProcessorService::installTransform`, which rejects qualifiers
naming two datasets. Config read-back echoes each input bare unless the bare name is
ambiguous across datasets and `dataset_source:bare` resolves back to the bound dataset.
On-demand recipes keep the qualified name as the script's alias; the session's resolver strips
it for a topic, its qualifier splitter for another node's output (`RecipeGraph::QualifierSplitter`,
[derived_objects §8](../../docs/derived_objects/ARCHITECTURE.md#8-the-on-demand-pipeline)). A dataset merged
away stays registered, so its source keeps qualifying inputs, which bind on the anchor. A
qualified input saved in a layout also records what it read (`topic`, or `upstream` for another
node's output), so a generic layout binds on another recording while the name stays the alias.

### Custom Topics delete policy

`transformDeleteRefusal` / `onDemandDeleteRefusal` say why the Custom Topics editor may not
delete a row (gone, another owner, a transform reader of another plugin the cascade would
remove, or a history-exempt recipe or generator reading an output the delete removes);
`pj_app` words the refusal.

## Marker rules (BatchEngine)

- **Execution** (derived_objects §11): a run is staged on the GUI thread (`stageRun` copies
  the input series, clipped to the optional raw-ns `GeneratorRecipe::window`), computed on
  the session pool by the static `computeRun` over that copy only, and published on the GUI
  thread, one `markersPublished()` per burst (forwarded as `SessionManager::markersChanged`).
  Without a pool runs go inline. With the `InFlightBudget`, a pooled run waits in the shared
  line for `kMarkerRunBytes`, stages, and is charged its staged copy. A cancelled run stops
  inside the script (the `CancelToken` is polled from the Luau interrupt); output beyond
  `runMarkerScript`'s 16 MiB ceiling fails like a script error.
- **Reload brake.** `recomputeForChangedInputs` and `reachNewDatasets` only mark bindings
  stale. `Mode::kAutomatic` runs stale, demanded bindings with no run in flight through a
  single-shot brake at `reloadHz()` (default 2 Hz, clamped to [0.2, 20]); `Mode::kManual`
  waits for `refresh(id)` / `refreshAll(dataset)`, which ignore brake, mode and demand.
  Settings keys: `Preferences::derived_batch_mode`, `Preferences::derived_batch_reload_hz`;
  `pj_app` applies them at startup and on OK; the dataset menu's "Refresh derived" calls
  `refreshAll`. `AppSession` injects the resolver, the dataset lister and the demand
  predicate (not a demand-capable stream, or `TopicDemandTracker::hasAnyReference`); an
  `all_datasets` rule is demanded when one contributor is. A failed binding is not
  resubmitted by the brake until its inputs change. The result policy (D30) is in the
  `BatchEngine.h` class comment.
- **Create waits.** `kToCompletion` (`MarkersRuntimeHost`, because released plugins read the
  markers right after create; undo/redo, `DerivedObjects::restore` under `kHistory`)
  computes on the calling thread. `kBounded` (layout load under `kReplace`) computes on the
  pool and waits at most `kCreateBudget`; a run still going continues as the binding's
  scheduled run (`isRunning`, `runningChanged`). A failure no caller hears reaches
  `setDiagnosticSink` (`AppSession` passes the app's sink) once per distinct error, telling
  the user the previous markers are kept. `recomputeForDataset` and
  `clearGeneratorsForDataset` stay synchronous (callers repaint from the returned topics; a
  rollback must be complete on return). `cancelRunsOn(dataset)` is called by `RefillGuard`
  when a refill begins and at a merge.
- **One object topic per owner.** Each generator writes `__markers__/<key>#<id>`
  (`markerOwnerTopicName`), registered with metadata naming the markers type, scope, target
  and owner (`markerTopicMetadataJson`, `pj_datastore/marker_topic_metadata.hpp`). A marker
  topic is a family (a direct toolbox write plus one topic per generator) that the overlay
  reads as a union (`markerTopicsOf`). So dataset-scope (`__global__`) and all-datasets
  (`__all__`) rules coexist, two generators on one key never clobber each other, retiring
  one tombstones only its topic, and a direct write is never rewritten. The store is the
  only copy. A direct write to a bare `__markers__/<key>` name is stamped with its metadata
  by `ObjectStore` on registration.
- **Bindings.** Recipes are keyed by id, each holding per-dataset bindings; applying a
  dataset-bound rule to a second dataset adds a binding. An id's `ephemeral` /
  `history_exempt` flags must agree across its bindings, and a recipe whose `all_datasets`
  differs replaces the whole id, so no id owns both scopes. At a merge
  (`remapGeneratorsToAnchor`) consumed bindings rebind onto the anchor, the anchor's own
  binding wins, and `mergeMarkerTopics` (run first) moves each source marker topic onto the
  anchor's same-named topic, shifted and concatenated. `publishedMarkerScopes` backs
  `pj_plotting`'s `PlotWidget::markerScopeRows()`.
- **All-datasets rules** read every loaded dataset carrying their inputs and publish one
  union **once on `kAllDatasetsMarkerDataset` (id 0), in the display frame** (each
  contributor shifted by its alignment offset, `setDisplayOffsetResolver`), so a Source
  Timeline drag republishes nothing. `reachNewDatasets()` (from
  `SessionManager::publishSharedMarkersToNewDatasets`, wired to `CatalogModel::itemsAdded`)
  marks them stale on every catalog addition; `clearGeneratorsForDataset` rebuilds the
  union from survivors, tombstoning it when none is left (the rule stays live).
  `RefillGuard::rollback` re-runs generators because the shared set lives outside the
  dataset snapshot.
- **Validation.** `upsertGenerator` refuses an empty id, `#` in ids or output keys, and
  non-UTF-8 ids or keys, and rejects a `declared_inputs` length mismatch. Publishing onto a
  topic describing another marker set fails and leaves it untouched. `removeGenerator`
  retires every topic whose owner metadata is the id. A streaming tick reports a topic as
  changed when its newest timestamp, row count, retention floor or series generation moved.
- **History.** `clearGenerators(RestoreIntent, keep)` retires each id once and, under
  `kHistory`, leaves exempt generators live. `unchangedGeneratorIds(wanted, intent,
  removed_upstream)` keeps an id only when every persistent binding equals the snapshot's on
  each persisted field (CRLF-insensitive), it is within the intent's authority, reads no
  series rooted at a removed or re-created name and, for an `all_datasets` rule, has
  published, has no run in flight and no contributor offset moved since. One rule decides
  whether a rule reads a name: a series key rooted at the topic (`seriesKeyBelongsToTopic`).

## On-demand engine

Designed in derived_objects §8-§11 and §15; each class's header owns its contract.
Cross-module notes:

- `RecipeGraph`: a persistent id may not start with `~`, so a plugin preview's
  `kPreviewIdPrefix` key never collides with a layout's. Scripts compile once per revision
  (`scripting::compileOnDemand`): Luau at the upsert, Python on its first run on the Python
  lane so no upsert waits for the GIL.
- `InputStager`: stages on the GUI thread without fetching payloads; a video input is also
  keyed by its content revision (`ObjectStore::contentIdentity`), since `frame_at` may read
  any of its packets. The static `decode` runs on any thread through `src/ObjectDecode.h`,
  shared with `TransformService`.
- `NodeScheduler`: picks the lane per language (`laneOf`, `src/OnDemandRun.h`, shared with
  `GridRunner`); runs that do not fit the budget wait in the line shared with marker runs.
  `hold` / `release` defer serving a dataset whose storage changes.
- `GridRunner`: `SessionManager` wires its scalar change lookup, which reads numeric series
  only, so a window over a raw string input is refused naming it. Object outputs are
  summarized on the worker (`pj_scripting/object_summary.h`).
- `SeriesRunner`: rows reach plots and the catalog through `SessionManager::notifyIngest`;
  state changes are reported as `SessionManager::seriesStateChanged`;
  `SessionManager::setSeriesVisibleStart` splits a replay into visible range first, then
  backfill.
- `EnginePool` publishes in drains of at most 32; a throwing compute fails only its run. A
  Python node runs on `Lane::kPython`, whose watchdog polls `CancelToken::flags`.
- `InFlightBudget` defaults to room for max(1, `EnginePool::sharedLaneThreads()`) on-demand
  runs; a run larger than the budget runs alone; a release posts a drain instead of serving
  the line inside itself.

## Layout persistence

`DerivedObjects(SessionManager&, CatalogModel&)` is a stateless coordinator built where
needed. `save` writes the `<data_processors>` block; `parse(root, intent)` builds a
`DerivedSnapshot` (filter inputs as `SavedSeriesPath`, transforms producers-first,
generators with resolved dataset identity or `bind_by_inputs`); `refusal(snapshot)` is the
`kHistory` preflight; `restore(snapshot)` computes unchanged sets upstream to downstream,
clears the rest downstream to upstream (generators, filters, transforms), recreates filters
and transforms in rounds, refreshes the catalog (`CatalogModel::rebuildIfChanged`) so
generators resolve new outputs, binds `bind_by_inputs` generators, creates changed
generators (bounded for a layout load, to completion for undo/redo) and announces
`markersChanged` once. On-demand recipes are the most downstream engine: cleared first
(`OnDemandService::clearExcept`), installed after the generators, swept
(`sweepUnclaimed`), announced with one `objectTopicsChanged` (derived_objects §13, which
maps the protocol verbs onto engine methods). Diagnostics come back as data;
`pj_app`'s `MainWindow` reports them under source "Layout" (`applyWorkspace` parses once
for preflight and restore) and re-derives its Custom Topics panel. `HistoryScope.h` is also
consumed by `pj_plotting`'s `TabbedPlotWidget::xmlLoadState`.

## TF (TransformService)

Owns the per-dataset `pj::tf::TransformBuffer` forest because ownership follows the data's
life cycle, which is the runtime's. It claims the `kFrameTransforms` ingest tap, or drains
the `ObjectStore` by UID cursor when no tap is active, and decodes through the topic's
parser or the canonical codec (not `pj_scene3D`'s `resolveObject`: `pj_runtime` never
depends on `pj_scene3D`). Each transform keeps its own stamp (plus
`payload_stamp_shift`) and the store timestamp as entry time (derived_objects §12, the two
clocks). `pj_app` constructs it and calls the life-cycle hooks (`invalidateDataset`,
`invalidateAll`, `beginReplacingLoad` / `commitReplacingLoad` / `abortReplacingLoad`,
`publishTransforms`, `setLiveCacheWindow`); `datasetTransformsReady` tells docks to refresh.
A dataset's buffer object never changes, only its state (cleared on invalidation, removal
and merge; adopted in O(1) from staging on a reload commit), so no holder rebinds.
`snapshot(dataset)` is O(1) and copy-free, the same object per revision while held, readable
lock-free from any thread; while TF ingest is active a consumer takes new snapshots no
faster than the brake rate. The 3D fixed-frame preference is not here
(`pj_scene3d_widgets`' `FixedFrameMemory`).

## Recording

Design: [docs/session_recorder_design.md](../../docs/session_recorder_design.md).

- **`Recorder`** (one per source): `tapFor()` hands the source's host a stateless tap that
  co-owns the recorder; the binding→channel table is guarded by one mutex `mu_`; one writer
  thread drains a byte-budgeted queue into the sink, so push threads never touch the file
  format. Live policy D4: a producer never waits; when a message does not fit
  `queue_budget_bytes` the largest of {queued, incoming} is discarded until it fits, so a
  message larger than the budget is never recorded. Drops are counted and leave holes,
  never reorderings. Downloads may opt into `OverflowPolicy::kBlock`. `requestStop()` ends
  admission and wakes producers; cancellation calls it before joining producers, then
  `stop()`. A clean summary is not proof of complete coverage: the capture owner gates
  publication on successful source completion. A sink `write` error truncates and discards
  the rest of the queue; an `addChannel` error truncates but still drains; a failing
  `close()` also sets `file_incomplete` (needs `mcap recover`). A truncation does not close
  the sink: the owner, polling `state()`, still calls `stop()`, which drains, joins, then
  closes, idempotently. Exceptions from taps or sinks truncate instead of reaching the
  `noexcept` writer thread. Must be owned by a `std::shared_ptr` (`tapFor` calls
  `shared_from_this()`), which keeps a push thread inside `onMessage` from meeting a
  destroyed recorder. `recorder_test` compiles `Recorder.cpp` against
  `plotjuggler_sdk::base` only, which keeps it in the TSan job.
- **`RecordingSink`** calls are strictly sequenced across threads (open/close on the start
  and stop threads, the rest on the writer), so a sink needs no locking.
- **`McapRecordingWriter`**: one channel per binding, schemas deduplicated, 1 MiB zstd
  chunks with messages of 200 KiB or more in their own chunk (a lazy reader never
  decompresses a video frame to reach a scalar), exclusive-create file sink that surfaces
  disk errors, fsync before close, refusal past 65535 channels or schemas (MCAP ids are
  `uint16_t`).
- **`RecordingService`**: one press = one batch folder `pj_<UTC yyyyMMdd_HHmmss>/`
  (a bare `mkdir`, so creating it claims it) with one `<sanitized display name>.mcap` per
  source (NFC-normalized, reserved characters folded to `_`, 80 characters, `_2`/`_3` on a
  case-folded collision), each with its own `Recorder` and queue budget, sharing one
  `capture_id`. Folder and budget come from `Preferences::recording_directory` (empty =
  `<AppLocalDataLocation>/recordings`) and `Preferences::recording_queue_budget_mib`
  (clamped to 8..4096 MiB, per source). `start()` is transactional: it opens every file
  before attaching any tap and undoes everything on failure without emitting `started`.
  A `RecordingTarget` is an opaque `target_key` (`pj_app` passes the `DatasetId`) plus
  callbacks, so the service never learns about `DataSourceRuntimeHost`; `pj_app`'s
  streaming manager filters on `kCapabilityDelegatedIngest` (a direct writer has no raw
  bytes). Finalization is synchronous on the GUI thread, once per recorder (`stop()`,
  `onSourceEnded`, a polled truncation, the destructor). A 500 ms poll publishes monotonic
  `progress()`. `stopped(CaptureResult)` gives each source one exclusive bucket
  (`incomplete` > `truncated` > `lossy` > `clean`): test `clean_count`, never a boolean.
  Without a compiled or injected sink `start()` fails with "recording is not available in
  this build" before touching the filesystem.
- **`RecordingFormat.h`** is shared with the `data_load_mcap` replay path; channels carry no
  PJ metadata because a recording holds one source.

## Source cache

Specification: [docs/RECORDING_AND_CACHING.md §3.4](../../docs/RECORDING_AND_CACHING.md).

- **`SourceCacheStore`** wraps the SDK's `RequestArtifactCache` (desktop only) with host
  policy: hashed provider-generic identities, pin-then-validate lookups (a contended case
  is a miss with a retry hint), `publish()` with lost-lease recovery, quarantine
  (`*.corrupt.*`), and LRU cleanup under a 10 GB default budget that never evicts a pinned
  artifact. The filesystem is the index. Pins are dataset-scoped and never cross a plugin
  ABI (`SessionManager::pinDatasetResource`). `cleanup()` scans: call it from a worker.
  `lookup()` reports `MissKind::kContended` only on filesystem evidence of an active
  publisher, so restore paths refuse instead of double-downloading. Folder and budget apply
  at the next launch; `capture_enabled` applies immediately.
- **`SourceCaptureService`**: `sourceCacheIdentity()` is the one key derivation; `arm()`
  opens a cache transaction on the source's attach and records through a lossless
  `kBlock` `Recorder`; `finalize()` publishes only with no veto, an explicit COMPLETED
  terminal, an attached descriptor, no cancellation, zero latched failures, zero pure-lazy
  skips, structural coverage and a clean recorder close, embedding the `pj.capture`
  manifest; `resolve()` restores cache-first and quarantines artifacts without a verifiable
  manifest (a contended miss makes `LayoutImportBatch` fail that source).
  `setCaptureEnabled()` gates arming only. A refused publication never touches session data.

## Network services

- `httpGetWithTimeout` / `httpPostWithTimeout`: the callback runs on the caller's thread,
  also on error, abort or timeout; the reply is `deleteLater`'d after it, so never retain
  it. Used by `UpdateChecker`, `TelemetryPing` and `pj_app`'s registry-URL probe.
- `fetchBytesWithLimit`: the size-capped GET of an untrusted body, on desktop over
  `httpGetWithLimit` and in the browser over `BrowserFetch.h`; returns a `CancelFetch`.
  Used by `pj_app`'s `PresetController` and Scene3D's `UrlFetcher`.
- `BrowserFetch.h` (browser only): downloads that bypass Qt's network backend ([why](./TECHNICAL_NOTES.md)).
- `UpdateChecker` asks the install's channel (`PJ_INSTALLATION`): `deb` runs
  `apt-cache policy plotjuggler4`, `windows` runs `maintenancetool check-updates`, anything
  else (and a fallback) asks GitHub's latest release; probes need `QT_CONFIG(process)`.
  It emits at most one result signal and routes every failure to `checkFailed`, so the
  shell can stay silent at startup.
- `TelemetryPing`: the payload is built in `TelemetryPing.cpp`; its privacy contract is
  enforced by `telemetry_ping_test`'s allowed-key set, updated together with
  [docs/TELEMETRY.md](../../docs/TELEMETRY.md). An unstamped installation (`source`, the
  self-built default, or empty) never sends. The shell owns the opt-out
  (`Preferences::send_anonymous_stats`).

## Tests

- **Freeze bench.** `tests/engine_freeze_bench_test.cpp` (`EngineFreezeBench.*`, skipped
  unless `PJ_BENCH` is set) measures how long the GUI thread stalls when a transform or a
  marker rule is applied over a 500k-row series, or a rule re-runs after an input change:
  GUI-thread CPU time (longest stretch and total) and worst wall-clock stall (a 1 ms
  heartbeat's lateness), medians of five. Baselines: derived_objects §19 (Reference numbers).
- **On-demand fixtures.** `tests/on_demand_test_utils.h` (point clouds, recipes whose
  scripts never run, the gated or delayed `FakeRun`, a raw-points parser, store readers)
  serves `result_history_test`, `input_stager_test`, `node_scheduler_test` (with one
  end-to-end case and the `NodeSchedulerBench` benches on the real Luau run, `PJ_BENCH=1`),
  `on_demand_cursor_follow_test`, `on_demand_live_edge_test`, `series_runner_test` (through
  a real `AppSession` over `tests/on_demand_session_test_utils.h`) and the on-demand cases
  of `session_manager_test` (via `OnDemandService::setRunForTest`).
- **Recording fixtures.** `tests/recording_test_utils.h` (the `pj.recording` lookup and the
  `IsolatedQtSettings` sandbox) and `tests/runtime_host_test_fixture.h` (a real
  `DataSourceRuntimeHost` over the hermetic catalog and the mock object parser) serve the
  recording tests; the other `data_source_runtime_host_*` tests use their own fixtures.
- **Recording pipeline.** `tests/recording_pipeline_test.cpp` (desktop) assembles real
  hosts → `RecordTap` → `Recorder` → `McapRecordingWriter` under `RecordingService` and
  reads the files back with `mcap::McapReader`: a mid-stream start captures topics bound
  before it, pre-recording pushes never leak in, two sources land in two files of one
  capture (same `capture_id`, ordinals 1 and 2), a second recording starts from an empty
  channel table, and ingest is unaffected.
- `transform_service_test`'s custom `QCoreApplication` main exists because its debug
  thread-affinity death test forks and aborts in the child.
