# pj_runtime

App runtime services and contracts for PlotJuggler 4: the **services seam** between the shell (`pj_app`) and the rest of the system. Every widget family (`pj_plotting`, `pj_scene2D/widgets`, `pj_scene3D/widgets`) reads from and reacts to services exposed here; the families are siblings and never depend on each other. Public headers under `include/pj_runtime/` are the boundary for the shell and the widget modules, and their doc-comments own each API contract. Subsystem wiring and the non-obvious contracts are in [docs/ARCHITECTURE.md](./docs/ARCHITECTURE.md); the derived-data design (on-demand, batch and series engines, scheduling, TF) is in [docs/derived_objects/ARCHITECTURE.md](../docs/derived_objects/ARCHITECTURE.md).

## Invariants

- **No `Qt6::Widgets` link**, no concrete widgets. Qt Core, Network and Xml are allowed. Anything needing `QWidget` belongs in `pj_widgets` or a widget family.
- **Host code stays domain-neutral**: no plugin-specific terms or per-plugin policy; mechanisms are generic and plugins supply the meaning.
- **Pool workers touch only GUI-staged copies.** Derived runs (`BatchEngine`, `NodeScheduler`, `GridRunner`) stage their inputs on the GUI thread, compute on an `EnginePool` worker over that copy only, and publish on the GUI thread. No on-demand, series or marker script runs while a store lock is held (derived_objects §4 Invariants, item 6).
- **Destruction order is load-bearing.** In `SessionManager`, `InFlightBudget` is declared before `EnginePool` (it must outlive the pool) and `OnDemandService` after both (destroyed first). In `DerivedRuntimeHosts` the `DataProcessorsKindRouter` is the last member (destroyed first, releasing its evaluation handles while the bridges live); destroy a `DerivedRuntimeHosts` after the plugin handle and the builder's service views. `MarkersRuntimeHost` and `DerivedRuntimeHosts` are not movable (the vtables store `this`).
- **Delete a dataset with `SessionManager::removeDataset`, never `DataEngine::removeDataset`**: only the former re-bases the time origin, drops the registries and reconciles the derived engines.
- **`BatchEngine` writes only to the `ObjectStore`, never the `DataEngine`**; time-series outputs come from transforms (`IncrementalEngine`).
- **Metadata is the identity of marker and derived topics; names are opaque ids.** Recognize them with `isMarkerTopic` / `isDerivedObjectTopic` / `isOffClockTopic`, never by parsing a topic name.
- **GUI-thread APIs stay on the GUI thread**: `CatalogModel`, `TopicDemandTracker`, `TransformService` (its ingest-tap callback is the only worker entry), `RecordingService`, the `pj.data_processors.v1` bridges (`[main-thread]` slots). Callbacks that fire on plugin threads (`DataSourceRuntimeHost`'s `on_available_topics`, `RecordTap`) own their own marshaling.
- **Whole-series readers on the GUI thread call `DataEngine::awaitCompleteSeries(topic)`** before reading, so they never see a partially materialized transform output.
- **`ParserRoutingService` lock order**: `routing_mutex_` is taken before `catalog_mutex_`.
- **File-format and privacy contracts**: renaming anything in `RecordingFormat.h` is a recording file-format break (the MCAP replay path reads it); extending the telemetry payload means updating `telemetry_ping_test`'s allowed-key set and [docs/TELEMETRY.md](../docs/TELEMETRY.md) together.

## Public headers

One line each; the header holds the contract, [docs/ARCHITECTURE.md](./docs/ARCHITECTURE.md) the wiring.

Session, catalog and time:
- `AppSession.h`: the central runtime object `pj_app` creates once; owns the services, the playback range, dataset merge, and the on-demand demand/playhead wiring.
- `SessionManager.h`: session and dataset lifecycle, display-offset model, dataset identity registry, token-addressed progressive ingest, loader metadata; owns the pool, the budget and the derived engines. Its two `displayOffsetChanged` overloads need `qOverload<>` / `qOverload<PJ::DatasetId>`.
- `CatalogModel.h`: curves, objects and sources for the UI; demand placeholders, identity facade, the shared curve resolver `resolveCurveKey`. GUI thread.
- `TopicDemandTracker.h`: per-topic streaming subscriptions (displayed ∪ infrastructure ∪ derived inputs ∪ forced). GUI thread; owned by `AppSession`.
- `CurveDescriptor.h`, `CurveDisplayName.h`, `CurveColorRegistry.h`, `SavedSeriesPath.h`: curve identity, display label formatter, session color memory, and the persisted series identity (header-only, Qt-only, shared with `pj_app`'s DOM-only tests).
- `Time.h`: display-axis time vocabulary (`DisplayOffset`, `DisplaySeconds`, `DisplayRange`) over the SDK's absolute `pj_base/time.hpp`.
- `PlaybackEngine.h`: time cursor and transport; `currentTimeChanged(double)` stays `double` (fleet-wide moc/vtable contract).
- `HistoryScope.h`: `SnapshotScope` / `RestoreIntent`, what a capture includes and what a restore may touch.
- `IDataWidget.h`, `IObjectViewer.h`: the contracts data widgets and object-store viewers implement for the shell. `IDataWidget::aboutToBeDiscarded()` runs once before the dock schedules a widget for deletion, so one that owns plugin-created objects destroys them synchronously.
- `DiagnosticHistory.h`, `DiagnosticEmit.h`: diagnostics ring buffer and the fill-and-forward helper (no-op without a sink).
- `HttpGet.h`, `UpdateChecker.h`, `UpdateVersion.h`, `TelemetryPing.h`: one-shot HTTP idiom (`fetchBytesWithLimit()` is the size-capped GET for untrusted bodies on desktop and in the browser, behind `pj_app`'s `PresetController` and Scene3D's `UrlFetcher`; `HttpUrl.h`'s `isHttpUrl()`, Qt6::Network-free, is the http(s) gate), the per-channel update check, version math, the anonymous launch ping.
- `BrowserFetch.h` (browser only): downloads that bypass Qt's network backend ([why](./docs/TECHNICAL_NOTES.md)); a pinned download is kept in Cache Storage across visits.
- `PresetCatalog.h`: `parsePresetCatalog()` reads a deployment's browser `presets/index.json` into `PresetEntry`s (`findPreset()` looks one up by id); `kMaxPresetDownloadBytes` caps each small preset download (index, layout, thumbnail), `kMaxPresetDataBytes` each recording and a preset's recordings together.
- `SourceConfigVariables.h`: `expandSourceConfigVariables()` expands `${deployment.ws_origin}` (the page's WebSocket origin) in a saved streaming-source configuration; any other `${deployment.*}` name is an error.
- `QSettingsBackend.h`, `XmlCdata.h`: `sdk::SettingsBackend` over `QSettings`; CDATA helpers shared with `pj_app`'s layout code.

Plugin hosts and extensions:
- `DataSourceRuntimeHost.h`: runs `DataSource` plugins: DSO keepalive, advertised-topic classification, idempotent parser bindings, ingest completion, record tap. A plugin `release` may run on any worker thread.
- `ToolboxRuntimeHost.h`: runs `Toolbox` plugins: write surface, settings, delegated parser ingest, progressive import, source-capture arming.
- `ExtensionCatalogService.h`, `PluginRuntimeCatalog.h`, `ParserRoutingService.h`: the loaded plugin set (scan hierarchy, bundled seeding, duplicate resolution, admission gate) and route-aware parser selection.
- `PlaybackControlHost.h`, `ViewportRuntimeHost.h`, `PlotTabsRuntimeHost.h`: SDK `pj.playback.v1`, `pj.viewport.v1`, `pj.plot_tabs.v1`.
- `ServiceRegistration.h`: host-to-plugin service registration with logged rejection.
- `WidgetRuntimeHost.h`, `WireTarget.h`, `WidgetValue.h`, `ActiveWidgetsMarker.h`: the Widget family: the per-panel `pj.widget_host.v1` host, where a wire reads from, the common fields of a `PJ_widget_value_t`, and the per-process record of live Custom Widget plugins the startup crash review reads. `ExtensionCatalogService::widgets()` / `findWidget()` list them; a Widget plugin leaves the catalog only by a user disable ([docs/ARCHITECTURE.md](./docs/ARCHITECTURE.md) "Widget plugins").
- `FeatureCatalogHost.h`, `ToolboxTags.h`: the optional `pj.feature_catalog.v1` service (desktop only; built from injected providers, never fetches) and the manifest tags (`cloud`, `hidden`) the Toolbox menu, the Sources panel and the catalog read.
- `ObjectIngestTap.h`: session-scoped exclusive consumers of builtin object bytes; the payload view is call-lifetime only.
- `parse_locked.h`: decode one object through a topic's parser under the binding's per-topic mutex (parsers are not thread-safe).
- `TransformService.h`: per-dataset TF buffers and TF ingest; lock-free immutable snapshots for scripts.

Data processors and derived data:
- `DataProcessorService.h`: filters and transforms as eager `IncrementalEngine` nodes; time-sliced create, history reconciliation, failure diagnostics.
- `BatchEngine.h`: marker rules (`kind=markers`), one object topic per owner; staged / pooled / published runs with a reload brake.
- `MarkerTopics.h`, `DerivedTopics.h`: marker and on-demand topic vocabulary and metadata predicates.
- `EnginePool.h`, `InFlightBudget.h`: the bounded worker pool (shared and Python lanes) and the shared in-flight memory budget with one FIFO line.
- `OnDemandService.h` (facade) over `RecipeGraph.h`, `ResultHistory.h`, `InputStager.h`, `NodeScheduler.h`, `GridRunner.h`, `SeriesRunner.h`: the on-demand engine (derived_objects §8-§11, §15).
- `MarkersRuntimeHost.h`, `DataProcessorsRuntimeHost.h`, `OnDemandRuntimeHost.h`, `DataProcessorsKindRouter.h`, `DerivedRuntimeHosts.h`: the per-plugin `pj.data_processors.v1` bridges, the kind router and their owner.
- `DatasetQualifiedName.h`: `dataset_source:topic/field` lookups; not a persistent identity.
- `DerivedObjects.h`: layout persistence and the one restore of all derived recipes across the engines (derived_objects §13).
- `CustomTopicsPolicy.h`: why the Custom Topics editor may not delete a row.

Recording and source cache:
- `RecordingTypes.h`, `RecordTap.h`, `RecordingSink.h`, `RecordingFormat.h`: recording value types, the push-thread tap (must not parse or touch datastore state), the sink seam, the on-disk contract.
- `Recorder.h`: one per recorded source; must be owned by a `std::shared_ptr`. Qt-free and format-agnostic.
- `McapRecordingWriter.h`: the MCAP sink (desktop only).
- `RecordingService.h`: GUI-thread owner of one capture; `isSupported()` is the single runtime gate for every Record affordance.
- `SourceCacheStore.h`, `SourceCaptureService.h`: the offline source cache and its capture/restore gate (desktop only).

Sanitizer support: `TsanAnnotations.h` (tells TSan about hand-offs inside prebuilt Qt; no-ops outside a TSan build).

Private headers (`src/`): `RuntimeHostSlot.h` (the shared C-ABI slot guard of the SDK host bridges), `ObjectDecode.h` (parser-or-canonical object decode, shared by `InputStager` and `TransformService`), `OnDemandRun.h` (run result and lane choice), `SeriesKey.h` (`seriesKeyBelongsToTopic`: `imu` is not `imu_raw`), `TextCompare.h` (CRLF-insensitive text equality for recipe comparison).

## Tests

gtest runners in `tests/`: service suites link into `pj_runtime_tests` (shared `QCoreApplication` main; each case registered as `pj_runtime_tests.<Suite>.<Case>`, one process per case). Add new sources to `PJ_RUNTIME_TEST_SOURCES`. Separate executables: `recorder_test` (Qt-free, in the TSan list), `transform_service_test` (custom main, forking death test), `extension_catalog_service_thread_safety_test`. Whole module:

```bash
./test.sh -R '^(pj_runtime_tests\.|recorder_test$|transform_service_test$|extension_catalog_service_thread_safety_test$)'
```

Shared fixtures, the freeze bench and the recording pipeline test: [docs/ARCHITECTURE.md § Tests](./docs/ARCHITECTURE.md#tests).

## Read deeper

- [docs/ARCHITECTURE.md](./docs/ARCHITECTURE.md): link surface and per-subsystem wiring and contracts.
- [docs/TECHNICAL_NOTES.md](./docs/TECHNICAL_NOTES.md): Qt pitfalls the code works around.
- [docs/derived_objects/README.md](../docs/derived_objects/README.md): the derived-data engines (start there, then ARCHITECTURE.md; §21 has recipes for common changes).
- [docs/RECORDING_AND_CACHING.md](../docs/RECORDING_AND_CACHING.md) and [docs/session_recorder_design.md](../docs/session_recorder_design.md): recording and the source cache.
