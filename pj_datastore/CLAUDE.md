# pj_datastore

The in-memory store every source writes to and every view reads from: `DataEngine` (columnar scalar time series),
`ObjectStore` (timestamped opaque payloads), `IncrementalEngine` (derived series), the host side of the plugin data
C ABI, and the `PJ::proc` data-processor substrate that `pj_scripting` builds filters on. Pure C++20; links
`plotjuggler_sdk::base` plus fmt, tsl::robin_map, nanoarrow and nlohmann_json, never Qt or `pj_plugins`. Plugins reach
it only through the C ABI. Timestamps are absolute int64 nanoseconds. How it works:
[docs/ARCHITECTURE.md](./docs/ARCHITECTURE.md).

## Invariants

- Every `DataEngine` access holds `lockEngine()`. `getTopicStorage()` and `typeRegistry()` do not lock: hold the lock
  while using what they return, or a worker commit races the read.
- Nothing read from a chunk outlives the lock it was read under: a writer's commit grows a topic's last chunk in place
  and re-encodes it when it fills, on the writer's thread. A `TopicChunk*` survives that; a value's address, a
  `readString()` view or a column pointer does not. Copy what must last (`PointSeriesXY` copies its points).
- A row never changes under its chunk id, and only a topic's last chunk gains rows and columns. A consumer that
  remembers a chunk id also remembers how many of its rows it read (`IncrementalEngine` does), or it misses the rows
  the chunk gains.
- Freeing or moving chunks (retention, clear, retire, detach, `flushTo()`, `replaceDatasetFrom()`, `mergeDatasets()`,
  `removeDataset()`) happens on the GUI thread after consumers drop cached `TopicChunk*`, or those pointers dangle.
- Two engines are locked together with `lockEnginePair()`; locking one, then the other deadlocks against `flushTo()`.
- The engine lock and the `ObjectStore` locks never nest. Store predicates, visitors and holders of an
  `EntryTimestampsView` never call the store. Both deadlock.
- Curves are keyed by (topic id, column index): reload and merge keep both, or saved layouts lose their curves.
- Columns can appear mid-stream, so earlier chunks have fewer: bounds-check `col_index < chunk->columns.size()`.
- `readNumericAsDouble()` returns 0.0 at a null: check `isNull()` or use `readColumnAsDoubles()` (NaN at nulls).
  `readString()` views chunk memory.
- Transforms see input in ascending time order; whatever breaks that order must reset and replay the node, or derived
  values are wrong.
- A dataset's name on every surface (tree, plugin catalog snapshot, `label:topic/field` qualifiers) is
  `DataEngine::datasetLabel`; deriving one from `source_name` instead makes the surfaces disagree on same-named
  datasets. Persist the dataset id and `source_name`, never the label.
- No exception crosses the plugin ABI.

## Key headers (`include/pj_datastore/`)

- `engine.hpp`: `DataEngine`: datasets and their labels, topics, commit, retention, reload, merge, pause/resume flush,
  the lock.
- `writer.hpp`: `DataWriter`, rows and column batches into chunk builders.
- `reader.hpp`, `query.hpp`: `DataReader`; row cursors, series views, latest-at.
- `chunk.hpp`, `topic_storage.hpp`, `encoding.hpp`, `column_buffer.hpp`, `buffer.hpp`, `type_registry.hpp`: storage.
- `object_store.hpp`, `ordered_entries.hpp`, `sequential_uid.hpp`, `resident_payload_pool.hpp`: `ObjectStore`.
- `incremental_engine.hpp`, `builtin_transforms.hpp`: the derived-series graph and the built-in transforms.
- `plugin_data_host.hpp`: the C-ABI host bridges; the windowed series read and value scan the MCP tools use.
- `arrow_import.hpp`: Arrow IPC and C streams into a `DataWriter`.
- `merge_result.hpp`, `replace_result.hpp`: what a merge and a reload report.
- `marker_topic_metadata.hpp`: the metadata that marks a plot-markers topic.
- `saturating_time.hpp`: `Timestamp` arithmetic that clamps at the int64 limits.
- `colormap_registry.hpp`: a registry of named colormap callbacks that no code uses.
- `data_processor.hpp`, `sample.hpp`, `processor_siso_adapter.hpp`, `processor_detail.hpp`: `PJ::proc`.

## Tests

`./test.sh -R '^pj_datastore_tests\.'` runs the main runner (one process per case). Separate executables:
`chunk_test` (death tests), and `engine_concurrency_test` and `engine_thread_safety_test`, the ThreadSanitizer entry
points of `build.sh` and CI (90 s timeout). `plugin_host_write_test.cpp` and `plugin_host_read_test.cpp` are not
built: they exercise a read/write ABI the SDK no longer has. `benchmarks/` prints numbers (read, ingest, writer
against a GUI reader); `examples/parquet_import` builds with `PJ_BUILD_PARQUET_IMPORT_EXAMPLE`.

## Docs

- [docs/ARCHITECTURE.md](./docs/ARCHITECTURE.md): data model, ingest, storage, reads, threading, pause/resume,
  reload and merge, derived series, ObjectStore, host bridges, processors, known gaps.
- [docs/TECHNICAL_NOTES.md](./docs/TECHNICAL_NOTES.md): measurements and the alternatives tried or considered.
- [docs/USER_GUIDE.md](./docs/USER_GUIDE.md): plugin-facing write and read patterns.
