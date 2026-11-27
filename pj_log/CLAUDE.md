# pj_log

The built-in Log panel: a list of the log topics (`sdk::BuiltinObjectType::kLog`) dropped on it, merged by time, with
level toggles, source/category/message filters, a detail pane, the row at the tracker tinted and later rows greyed or
hidden. A widget family like `pj_plotting` and `pj_custom_widgets`: it depends on `pj_runtime` and `pj_widgets` and on
no other family, and no family depends on it; `pj_app` creates the panel (kind and layout tag `log`). How it works:
[docs/ARCHITECTURE.md](./docs/ARCHITECTURE.md).

## Invariants

- **The panel holds no copy of a log.** `LogStore` is an index (`RowRef`: time, topic, row number) over each topic's
  `LogRowSource`; the dock's source reads the session's `LogEntries` through `ObjectStore::logView`, and the panel
  decodes only what it shows or filters. Copying rows back in brings back the memory ceiling the index removed (a
  1'000'000-row cap dropped the start of large logs).
- **An update re-indexes only what changed:** the new rows, the rows from the oldest late entry on when
  `contentIdentity().rewrite` moved, and the whole topic only when `removal` moved or the display offset changed.
  Re-indexing on each landing makes a progressive load quadratic in the log size
  ([docs/TECHNICAL_NOTES.md](./docs/TECHNICAL_NOTES.md), "Streaming"). The snapshot (identity, row range, newest UID)
  is taken under one view before the panel reads rows; a change after it shows as a new identity next update. Until
  then the rows it moved read as absent (`LogRowSource::validEnd`, from `LogEntries::firstRowChangedSince`), never as
  their new neighbour's text.
- **A source's `on_moved` never updates synchronously:** a read runs inside a paint or inside a batch holding a
  `LogSeriesView`, and an update takes a view (deadlock) and changes the model (mid-paint).
- **Never hold a `LogSeriesView` while taking another** (or calling the store): a `LogRowSource::batch` reads its own
  topic only. Holding one while acquiring a second deadlocks against a writer waiting for the store lock. This is why
  `RowRef` carries the row's time: merging and the tracker never read another topic.
- **Row times are display-axis seconds** (`rawToDisplaySecondsAtOrAfter` of the entry's store stamp); the log's own
  stamp is metadata for the detail pane only. Sorting, the tracker and seeks on the own stamp would put rows where a
  seek to them does not reveal them.
- **`LogView` is runtime-agnostic:** it takes a `LogRowSource` (or `LogEntry` rows, kept in a `VectorLogRowSource`)
  keyed by a host-chosen topic id and never reaches the session. A source's rows are in ascending time, or
  `LogStore`'s merge order breaks; `appendEntries` merges earlier entries into place.
- **Persisted names are a layout-file contract:** the `<log>` tag, its `<topic>` children with the scene-dock identity
  attributes (so `LayoutXml`'s dataset relocation applies) and the level keys in `log_view::kLevelNames`. Renaming one
  drops the setting from saved layouts.
- **Every attribute `xmlSaveState` writes is read in `xmlValidateState` through a reporting reader** (`readXmlText`,
  `XmlOptionsRecorder::noteOption`), the same parse `xmlLoadState` uses: one the validator never reads is missing from
  the MCP `widget_state` options. `ValidationDescribesTheSavedStateAndAgreesWithLoad` fails on it.
- **Unresolved topics are written back verbatim** and retried on catalog and source-path changes; dropping them
  loses the topic from the layout when its dataset is not loaded yet.
- `core/` stays Qt-free: the store and the filter are unit-tested without an application.

## Layout

- `core/` (`pj_log_core`, Qt-free): `LogRowSource.h` (`LogLevel`, `LogEntry`, the `LogRowSource` read seam,
  `VectorLogRowSource`), `LogStore.h` (`RowRef`, `LogRow`, `StoreChange`, the merged `LogStore` index and its message
  cache, `LogFilter` / `CompiledFilter`, the level names, `stripTerminalEscapes`).
- `widget/` (`pj_log`): `LogTableModel.h` (the table model over the filtered rows, tracker tint, future rows),
  `LogView.h` + `src/LogView.ui` (the list, filters and detail pane), `LogDockWidget.h` (the `IDataWidget`: indexes
  canonical `kLog` topics of the session, persistence, pending topics).
- Shell side lives in `pj_app`: `MainWindow::makeSceneDock` (the `log` kind), `TopicDemandController::registerLogDock`,
  `PendingDisplayBinder::addPendingLogTopic`, `scene_object_classification` (drop routing and the curve-list badge).

## Tests

`./test.sh -R '^pj_log_tests\.'`: `log_model_test.cpp` (index, filter, table model), `log_view_test.cpp` (the view
with plain `LogEntry` data), `log_dock_widget_test.cpp` (the dock over a real session). The shell paths (placeholder
button, drops, layouts) are in `pj_app/tests/main_window_log_panel_test.cpp`.

Benchmark (`PJ_BUILD_DEMOS`, no ctest): `benchmarks/log_panel_benchmark.cpp` prints the panel's GUI-thread CPU under
playback, streaming and one-off loads (one `LOG_BENCH` line per run, then a summary). Build and run it under
`host-job`, offscreen:

```bash
host-job log-bench-build bash -c 'PJ_BUILD_JOBS=8 ./build.sh --target log_panel_benchmark'
host-job --light log-bench env QT_QPA_PLATFORM=offscreen ./build/pj_log/benchmarks/log_panel_benchmark \
    [--only playback|streaming|oneoff|bigload] [--stream-seconds 10] [--rows 1000000]
```

## Docs

[docs/ARCHITECTURE.md](./docs/ARCHITECTURE.md): components, data flow, tracker and seeking, persistence, known gaps.
[docs/TECHNICAL_NOTES.md](./docs/TECHNICAL_NOTES.md): measured CPU cost under playback (grey vs hidden future rows),
streaming (late entries appended, against the re-read they replaced), one-off loads and large logs (1e6 and 1e7 rows).
