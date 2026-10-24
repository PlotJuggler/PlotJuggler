# pj_datastore Architecture

> **Scope:** this document covers the **scalar `DataEngine` path** — the columnar
> store, encoding, queries, and `IncrementalEngine`. The library also compiles three
> first-class components documented elsewhere or summarized below: the opaque-blob
> **`ObjectStore`** (its own doc, [`OBJECT_STORE_DESIGN.md`](./OBJECT_STORE_DESIGN.md)),
> the **`ColorMapRegistry`** + its C-ABI host (§3 *Color Map Layer*), and the
> **streaming two-engine `flushTo`/`setTarget`** swap (§3 *Plugin Host Layer*).

## 1. Module Structure

Two libraries with a strict dependency direction:

- **pj_base** (from the `plotjuggler_sdk` Conan package): Vocabulary types and SDK headers with zero external dependencies. Defines:
  - `Timestamp` (`int64_t`, nanoseconds since Unix epoch)
  - `Range<T>` inclusive min/max pairs
  - Identity types: `DatasetId`, `TopicId`, `FieldId`, `SchemaId`, `TimeDomainId` (all `uint32_t`), `ChunkId` (`uint64_t`), `NodeId` (`uint32_t`)
  - `PrimitiveType` enum (12 variants: kFloat32..kUint64, kBool, kString)
  - `NumericType`, `NumericValue` (variant of all numeric scalars)
  - `TypeTreeNode` (recursive schema tree: primitive, struct, array, enum nodes)
  - `Span<T>` (alias for `std::span`), `BitSpan` (bit-level view with offset)
  - `Expected<T>` / `Status` for fallible operations, `PJ_ASSERT` for invariants

- **pj_datastore** (`pj_datastore/`): Engine implementation. External dependencies: fmt, tsl::robin_map, nanoarrow (Arrow IPC import).

Dependency: `pj_datastore` -> `pj_base`. No reverse dependency. `pj_plugins` depends on `pj_base` only, never on `pj_datastore`.

## 2. Domain Model

Hierarchy: **Dataset -> Topic -> Chunk -> Column**

- **Dataset** (`DatasetInfo`): Identity (`DatasetId`), `source_name` string, bound `TimeDomain`. Represents one data source (file, live connection).

- **Topic** (`TopicDescriptor` + `TopicStorage`): Named data stream within a dataset. Key fields: `schema_id` (0 = schemaless), `max_chunk_rows` (default 1024), `array_expansion_limit` (default 64). `TopicStorage` owns the committed chunk deque and column descriptors for schemaless topics.

- **Schema** (`TypeRegistry` + `TypeTreeNode`): Tree of named typed nodes. `TypeRegistry` assigns `SchemaId` values, supports lookup by id/name, and additive-only evolution via `evolveSchema()`. Schemas are shared across topics.

- **Chunk** (`TopicChunk`): Sealed, immutable storage unit produced by `TopicChunkBuilder::seal()`. Contains: `ChunkId` (monotonic atomic counter), timestamps vector, columns vector (each holding `EncodedData` + optional `BitVector` validity bitmap + `shared_ptr<ColumnDescriptor>`), and `ChunkStats` (t_min, t_max, row_count, per-column `ColumnStats`).

- **Column** (`TopicChunk::Column`): The physical unit. `EncodedData` is a variant of 5 encoding types (see section 5). `ColumnDescriptor` carries field_id, logical_type, and fully-qualified field_path (e.g. `"pose.position.x"`).

## 3. Layer Architecture

### Logical Layer

**`DataEngine`** — Central owner of all state. Stores datasets, topics (as `TopicStorage`), time domains, and the global `TypeRegistry`, all in hash map containers (tsl::robin_map internally, std::unordered_map in headers). Provides:
- `createDataset()`, `createTopic()`, `createTimeDomain()` with monotonic ID allocation or explicit caller-requested IDs
- `commitChunks()` — appends sealed chunks to `TopicStorage`, returns deduplicated list of changed `TopicId`s
- `enforceRetention()` — evicts old chunks across all topics
- `evictTopicHistory()` — raises ONE topic's virtual retention floor past everything currently stored (rows vanish from all read paths; the topic, ids, and column layout stay registered). Data-relative on purpose — stream timestamps are recording time, so a wall-clock cutoff could hide future samples. Used by the app to disown a demand-subscription field-discovery preview's fake-interest samples
- Factory methods `createWriter()` and `createReader()`

**`TypeRegistry`** — `registerSchema()` assigns a new `SchemaId`. `registerOrGet()` returns an existing ID if the name matches (for late-discovery schemas). `evolveSchema()` validates additive-only changes (no field removal or type change).

### Storage Layer

**`TopicChunkBuilder`** — Mutable builder that accumulates rows for one topic. Two append paths:
- *Row-at-a-time*: `beginRow(timestamp)` -> `set<T>(col, value)` / `setNull(col)` -> `finishRow()`. `finishRow()` pads unset columns with null.
- *Bulk*: `appendTimestamps(span)` -> `appendColumn<T>(col, span)` per column -> `appendColumnValidity(col, bitspan)` -> `finishBulkAppend()` (computes stats).

Tracks per-column `ColumnStats` incrementally (min, max, null_count, is_constant, run_count); `seal()` adds `all_integral` for float columns (every non-null value a whole number). `TopicStorage::wholeNumbersOnly(field)` ANDs that flag over the retained chunks (incremental on append, rebuilt lazily after clear/evict/restore/moves) and `wholeNumbersSignature()` changes whenever an answer does — the catalog uses both to offer whole-number float columns as discrete state series. `seal()` encodes columns, assigns a monotonic `ChunkId` via `std::atomic<ChunkId>`, and produces an immutable `TopicChunk`.

**`TopicStorage`** — Per-topic container of committed chunks in a `std::deque<TopicChunk>`. `appendSealedChunk()` appends in commit order without rejecting any chunk — out-of-order ingest means chunk time ranges may overlap, so queries merge across them (see §5). `evictBefore()` removes the contiguous prefix of chunks whose `t_max < threshold` and raises a per-topic retention floor. Also stores column descriptors for schemaless (schema_id == 0) topics and per-field array expansion counts.

**`TypedColumnBuffer`** — In-memory typed column buffer used internally by the builder. One buffer per column. Supports per-type single-row append (`appendFloat64`, `appendInt64`, etc.) and bulk append (`appendFloat64Bulk`, etc.). String storage uses a separate offsets buffer (`RawBuffer`) with Arrow-compatible uint32 offset layout. Validity bitmap (`BitVector`) is lazily initialized -- only allocated when the first null is appended.

**`RawBuffer`** — Growable byte vector wrapping `std::vector<uint8_t>`. Used for column value storage and encoded payloads.

**`BitVector`** — Owning packed validity bitmap with Arrow-compatible LSB-first layout. Supports `initValid()`, `setNull()`, `isValid()`, `countNulls()`, and bulk `assignBytes()`.

### Writer Layer

**`DataWriter`** — High-level write facade bound to a `DataEngine`. Manages one `TopicChunkBuilder` and a pending chunk list per topic. Key operations:
- `registerTopic()` / `registerScalarSeries()` — topic creation
- `ensureColumn()` — dynamic column addition for schemaless topics. Rejects if a row is in progress (for new columns only). Seals any pending builder before modifying column layout.
- `expandArray()` — variable-length array expansion. Seals current builder, adds new `ColumnDescriptor`s, updates `TopicStorage`. Clamps to `array_expansion_limit`.
- `appendColumns()` — bulk ingest with auto-chunking: splits batches that exceed `max_chunk_rows`, calling `appendTimestamps` / `appendColumn` / `finishBulkAppend` / `autoSeal` per sub-batch.
- `flush()` / `flushAll()` — seals remaining builders and returns `vector<pair<TopicId, TopicChunk>>` for `commitChunks()`.
- `autoSeal()` — called when builder is full (`rowCount >= max_chunk_rows`). Seals builder, moves chunk to `pending_chunks_`.

### Reader Layer

**`DataReader`** — Read-only facade over committed `DataEngine` storage. Provides `listDatasets()`, `listTopics()`, `getTypeTree()`, `getMetadata()`, `rangeQuery()`, `latestAt()`, `columnType(topic_id, column_index)` (the schema's logical type, before any row is stored), and `series(topic_id, column_index)`.

### Query Layer

**`RangeCursor`** — Iterates rows in `[t_min, t_max]` across the chunk deque. Chunk time ranges may overlap (out-of-order ingest), so the constructor seeds one frontier per intersecting chunk into a min-heap on (timestamp, chunk index). Supports:
- `forEach(callback)` — per-row iteration via `SampleRow` (timestamp + chunk pointer + row index), in globally ascending timestamp order even across overlapping chunks; the heap advance is O(1) while chunks don't actually overlap
- `forEachChunk(callback)` — bulk iteration via `ChunkRowRange` (chunk pointer + row start/end); runs are per-chunk sorted but delivered in commit order, so under overlap they may interleave in time — bulk consumers tolerate that or use `forEach`

**`latestAt(chunks, t)`** — Scans candidate chunks (per-chunk binary search) for the most recent row at or before `t`; later-committed chunks win timestamp ties. Returns `optional<SampleRow>`.

Both are free functions operating on `const std::deque<TopicChunk>&`.

**`SeriesReader`** — Views one numeric/bool topic column as a virtual vector of `(timestamp, value)` samples. It is bound to a topic and column when created through `DataReader::series()`. Null physical rows and chunks where the column does not exist are skipped by definition. Provides:
- `size()` / `empty()` — sample count, not physical row count
- `sampleAt(index)` — lookup by virtual series index
- `sampleAtOrBeforeTime(t)` / `sampleAtOrAfterTime(t)` — lookup by timestamp over value-bearing samples
- `samples(Range<Timestamp>)` — cursor over samples in an inclusive time range
- `bounds()` / `bounds(Range<Timestamp>)` — valid time and value ranges for the series

**`SeriesCursor`** — Iterates value-bearing samples in `[time.min, time.max]` across chunks. It returns `SeriesSample` values with timestamp, double value, chunk pointer, and physical row index for low-level consumers that need provenance.

### Encoding Layer

Seven `StorageKind` values map logical `PrimitiveType` to physical storage:

| StorageKind | Source PrimitiveTypes |
|---|---|
| kFloat32 | kFloat32 |
| kFloat64 | kFloat64 |
| kInt32 | kInt32 |
| kInt64 | kInt8, kInt16, kInt64 |
| kUint64 | kUint8, kUint16, kUint32, kUint64 |
| kBool | kBool |
| kString | kString |

Five `EncodingType` values, selected at seal time per column:

| EncodingType | When used | Representation |
|---|---|---|
| kRaw (`RawBuffer`) | Default for uint64, and for floats with fractional/special values, when not constant | Typed byte buffer |
| kConstant (`ConstantEncoded`) | All non-null values equal (`is_constant && rowCount > 0`) | 8-byte value + count |
| kFrameOfReference (`FrameOfReferenceEncoded`) | kInt32/kInt64, and float32/float64 chunks holding only whole numbers, when the range fits in fewer bytes | int64 reference + packed uint8/16/32 offsets |
| kDictionary (`DictionaryEncoded`) | Always for kString | Unique string list + narrowed uint8/16/32 indices |
| kPackedBool (`PackedBools`) | kBool when not constant | 1 bit per value, LSB first |

`EncodedData = std::variant<RawBuffer, ConstantEncoded, FrameOfReferenceEncoded, DictionaryEncoded, PackedBools>`

Encoding selection in `TopicChunkBuilder::seal()`:
1. **Strings**: always dictionary encoded via `dictionaryEncodeStrings()`.
2. **Bools**: constant if `is_constant`, otherwise packed bits via `packBools()`.
3. **Signed integers** (kInt32/kInt64): recomputes exact int64 min/max from raw buffer (avoids double-precision loss). Constant if all equal, frame-of-reference if `offsetBytesFor(range) < storageKindSize(kind)`, otherwise raw.
4. **Float32/float64**: constant if `is_constant`; otherwise, if every value is a whole number (finite, not `-0.0`, inside the int64 range), frame-of-reference over the int64 values when `offsetBytesFor(range) < storageKindSize(kind)`; otherwise raw. The column keeps its declared float type, and FOR decoding (`reference + offset`) reproduces the original floats exactly — a whole float is an exact integer at any magnitude.
5. **Uint64**: constant if `is_constant`, otherwise raw.

### Derived Layer

**`IncrementalEngine`** — Manages a transform DAG. Uses pimpl (`IncrementalEngineImpl`). Key operations:

- `addSisoTransform()` — registers a single-input/single-output node. Input must be a single-column topic. Creates an output scalar topic. Returns `NodeId`.
- `addMimoTransform()` — registers a multi-input/multi-output node. All inputs must be single-column. Creates N output topics.
- `topologicalOrder()` — Kahn's algorithm. Cycle detection via DFS at registration time.
- `onSourceCommitted(changed_topics)` — marks directly dependent nodes dirty.
- `scheduleAll()` — processes all dirty nodes in topological order (incremental path).
- `scheduleActive(active_nodes)` — processes only specified nodes and their transitive upstream dependencies.
- `materializeStep(node_id, budget)` — time-sliced catch-up: advances one node over its unprocessed input for about `budget` of wall time, commits the rows, and returns whether it caught up. It shares the per-node step (`runNodeLocked`) with the scheduling passes. A node a step leaves behind is *deferred* — it has a `resume_chunk_id`/`resume_row` inside a chunk; `deferredNodes()` lists them in creation order, and the scheduling passes skip them until a step (or a remove/replace/recompute, or a failure) ends the replay. SISO over time-ordered input resumes mid-chunk; per-chunk feeding then equals the merge cursor's order, so the output matches the synchronous replay. MIMO nodes, overlapping input and nodes flagged `needs_full_recompute` finish in one step. `nodeError(node_id)` is the error of the node's last run by any pass, step or recompute (empty once one succeeds). A whole-series snapshot reader (the toolbox `read_series_arrow` bridge, the shell's marker-generator resolver) first calls `DataEngine::awaitCompleteSeries(topic)`, a main-thread barrier the owner of the replays installs (`setCompleteReadBarrier`) to finish a replay still growing that topic.
- `recomputeBatch(node_id)` — resets the transform and replays the node plus its downstream graph. Per node, the current output is detached aside (not cleared) before replay; a successful replay discards the detached data, while any other exit — an error Status *or* an exception out of the transform — restores it from an RAII guard. A failed recompute (including a sticky-failing processor) therefore never leaves a node's output emptier than it was going in.
- `recomputeBatch(root_node_ids)` — performs one topological replay over the union of several roots' downstream graphs, so a converging node runs once against one generation of its inputs.
- `inputBindingState()` / `resolvedInputBindingState()` / `restoreInputBindingState()` — capture, validate against the current input schemas, and restore input-column metadata for a host-managed reload transaction without reminting node or output topic ids.
- `replaceSisoTransform()` / `replaceMimoTransform()` — exchange only a node's operator while preserving topology and topic ids. They transactionally replay the downstream graph and restore the old operator and graph on failure.

**`ISISOTransform`** — Point-at-a-time interface. `calculate(time, input, &out_time, &out_value) -> bool`. Called in strictly ascending timestamp order. State persists across chunk boundaries. `reset()` clears state for batch recompute. `outputKind()` declares output `StorageKind` (default kFloat64).

**`IMIMOTransform`** — N inputs -> M outputs. `calculate(time, inputs_span, &out_time, &outputs_vec) -> bool`. Exact-timestamp inner join: only called when ALL input topics have a sample at the same timestamp. `outputKinds(input_kinds)` declares one `StorageKind` per output topic.

**`VarValue = std::variant<int64_t, uint64_t, double, std::string>`** — Universal value type for transform I/O. Mapping: float32/float64 -> double, int8..int64/bool -> int64_t, uint64 -> uint64_t, string -> std::string.

Incremental scheduling: each node tracks a `last_processed_chunk_id` watermark. `scheduleAll()` iterates only chunks with id > watermark, reads each row, calls `calculate()`, writes output via `beginRow`/`set`/`finishRow`, then flushes and commits.

Idle nodes cost nothing: a streaming tick marks every node dirty, so before reading anything a run asks whether the node's inputs hold a chunk numbered above its watermark (evicted ones included, so a retention gap is still seen). If none does it returns at once, in time linear in the number of inputs rather than in the retained chunks, and leaves its downstream alone. Chunk ids come from one process-wide counter and input becomes new to a node only as a committed chunk (an append, a late write, a refilled topic), so a new chunk is always above the watermark. A MIMO node keeps one newest-id per input (`mimo_input_chunk_ids`) and compares each input with its own, because its node-wide `mimo_last_chunk_id` is the maximum across inputs and a chunk sealed earlier than another input's can be committed after it. A node with `needs_full_recompute`, an unfinished time-sliced replay or an unreported error is never skipped.

Rolled-back nodes never go incremental: a recompute rewinds the watermarks before replaying, so a replay that fails (and rolls the output back) leaves watermarks that no longer describe the retained rows. Because the incremental path APPENDS, running it then would replay history on top of the restored rows and double them. Such a node carries `needs_full_recompute` until a replay succeeds, and both scheduling passes route it through the full detach-replay recompute, which rewrites the output wholesale. `restoreInputBindingState()` sets the same flag for the same reason (it rewinds watermarks without touching the output). Note the flag is *not* what schedules the node — a processed node is always left non-dirty, failure included, so a permanently failing processor still runs only when new input re-dirties it.

Out-of-order input: transforms have a strict ascending-timestamp contract, so before running a node the scheduler checks whether any not-yet-processed input chunk lands at or before the node's timestamp watermarks (`siso_last_ts` for SISO; `mimo_last_chunk_id` + `mimo_last_ts` for MIMO). Such late input triggers `recomputeBatch` — reset transform state, detach outputs aside, fully replay over the time-merged input (the SISO replay feeds rows through the `RangeCursor` heap merge), restoring the detached output on failure — instead of incremental work.

Retention gaps: when retention evicts input chunks a node never consumed, the node resets and replays what is still retained (the same path as out-of-order input) and records the gap `[feed watermark, first retained timestamp)` (`IncrementalEngine::gaps()`, the last 16; the watermark is `Timestamp::min` for a node that had fed nothing). The event is detected where it happens: `TopicStorage::evictBefore` records the highest chunk id it dropped, or cut rows from by raising the retention floor into a straddling chunk (`evictedThroughChunkId()`), and a node has a gap iff that id exceeds its chunk watermark (`last_processed_chunk_id` for SISO, `mimo_last_chunk_id` for MIMO, the largest chunk id seen across all inputs, so a newer chunk on one input can mask an eviction on another: a gap may be missed, never invented). Timestamps are not compared: the feed watermarks move only on a non-null row (SISO) or an exact-timestamp join (MIMO), so a node over an all-null column, an input with no rows or sparse inputs would otherwise look permanently behind. A never-run node (watermark 0) has nothing to fall behind, unless a time-sliced step already stopped inside its first chunk, and a replay raises the watermark past the evicted ids so a fully evicted input is one gap, not one per pass. A node stopped inside a chunk (its resume cursor) has consumed that chunk only up to its resume row: the chunk dropped, or the floor reaching the first unfed row, is a gap, while a floor cutting only the fed prefix is not. A failed node consumes nothing, but chunks arriving for it count as consumed (as a failed single-input node's already did), so retention passing its frozen watermark is no gap. The check runs only when neither `needs_full_recompute` nor a regression already forces the replay, since those reset the very watermark the gap reports. A node over a file dataset never gaps: nothing evicts a file topic. `DataEngine::flushTo` (the resume of a paused stream) hands the paused tail's retention floor and evicted-through id to the primary topic (`max` of both, the tail left with none): a pause longer than the window ends as one gap for a node that had not seen the trimmed rows, which then replays the retained window, and a straddling chunk's sub-floor rows stay hidden. `DataProcessorService::transformRecipeJson` exposes the list as `gaps` in a transform's config (`from_ns` is null for a node that had fed nothing).

### Color Map Layer

**`ColorMapRegistry`** (`colormap_registry.hpp`) — Registry of named colormap callbacks. A plugin registers one or more named `ColorMapEvalFn`s (scalar -> CSS color / `#rrggbb`) via `registerMap()` and selects an active one with `setActive()`; consumers (chart renderers, exporters) evaluate the active map per data point via `evaluate()`. The registry holds raw callback pointers + user contexts and does not own the plugin that supplied them.

**`makeColorMapRegistryHost()`** (`colormap_registry_host.hpp`) — Wraps a `ColorMapRegistry` as a C-ABI `PJ_colormap_registry_t` fat pointer for `bind_colormap_registry`. The fat pointer references the registry by address (the registry must outlive every bound plugin); the vtable is a static singleton, safe to share across plugins and threads.

### Plugin Host Layer

**`DatastoreSourceWriteHost`** / **`DatastoreParserWriteHost`** / **`DatastoreToolboxHost`** — Bridge between the C ABI scalar-write protocol (`PJ_source_write_host_t`, etc.) and the C++ `DataWriter`/`DataEngine`. Each wraps a pimpl state struct. Provides `raw()` to get the C function-pointer table and `flushPending()` to seal/commit accumulated data.

The host translates C ABI calls (ensureTopic, ensureField, appendRecord) into `DataWriter` operations (registerTopic/ensureColumn, beginRow/set/finishRow).

**Object hosts** — `DatastoreSourceObjectWriteHost`, `DatastoreParserObjectWriteHost`, and `DatastoreToolboxObjectReadHost` are the `ObjectStore` peers of the scalar hosts (surfaces `pj.source_object_write` / `pj.parser_object_write` / `pj.toolbox_object_read`). They bridge the C ABI onto an `ObjectStore` rather than a `DataEngine`; see [`OBJECT_STORE_DESIGN.md`](./OBJECT_STORE_DESIGN.md) for their write/read contracts.

**Streaming two-engine swap** — During paused streaming the host can route pushes to a *secondary* engine/store and swap back on resume. `DataEngine::flushTo(dst)` zero-copy-moves committed chunks from the secondary into the primary (topics matched by descriptor, per-topic monotonicity enforced); `DatastoreSourceWriteHost::setTarget()` / `DatastoreParserWriteHost::setTarget()` (and the object hosts' `setTarget()`) flush pending rows then atomically retarget the destination.

For the swap to be transparent to plugins that **cache** `TopicHandle`/`FieldHandle` (e.g. `data_stream_dummy` caches in `onStart()`; `parser_protobuf` caches on first message), both engines must assign the *same* `TopicId` **and** `FieldId` to a given `(topic, field)` — otherwise the cached id resolves to nothing on the post-swap target. The write hosts keep this lockstep themselves once the streaming manager wires a secondary via `setSecondaryEngine()`: every `ensureTopic`/`ensureField` — including the lazy column auto-create inside `append*` — is mirrored onto the secondary via `DataEngine::createTopic(requested_id)` and its field-level counterpart `DataEngine::createTopicField(requested_id)`, forcing matching ids (the latter takes a `std::optional<FieldId>` so a forced id 0, the first field of any topic, is not confused with "auto-assign"). Mirroring is bidirectional, so a cached handle resolves on whichever engine is the active target. `ObjectStore::flushTo()` provides the matching blob-side move; bound `ObjectTopicId`s stay valid because each `registerTopic` is likewise mirrored at registration time.

### Import Adapter

**Arrow IPC** (`PJ::arrow_import` namespace):
- `schemaFromIpc()` — parses Arrow schema from IPC stream bytes via nanoarrow. Maps Arrow fields to `ArrowColumnMapping` (arrow column index -> PJ column index + PrimitiveType). Unsupported Arrow types are skipped.
- `importIpcStream()` — reads record batches, appends via `DataWriter::appendColumns()` with validity bitmaps.

## 4. Data Flow

### Row-at-a-time Ingest (via plugin host)

1. Plugin calls `writeHost.ensureTopic(name)` -> host creates topic via `DataWriter::registerTopic()`
2. Plugin calls `writeHost.ensureField(topic, name, type)` -> host calls `DataWriter::ensureColumn()`
3. Plugin calls `writeHost.appendRecord(topic, timestamp, fields)` -> host calls `DataWriter::beginRow()`, `set()` per field, `finishRow()`
4. `finishRow()` pads unset columns with null, updates per-column stats
5. If builder is full (`rowCount >= max_chunk_rows`): `autoSeal()` seals builder, moves `TopicChunk` to `pending_chunks_`
6. Host calls `flushPending()` -> `DataWriter::flushAll()` seals remaining builders -> `DataEngine::commitChunks()` appends to `TopicStorage`
7. Chunks are now visible to readers and incremental engine

### Bulk Ingest

1. `DataWriter::appendColumns(topic_id, timestamps, column_data_array)`
2. Auto-chunks: splits batch into sub-batches fitting builder's `remainingCapacity()`
3. For each sub-batch: `appendTimestamps()`, `appendColumn<T>()` per column, `finishBulkAppend()` (computes stats), `autoSeal()` if full
4. Flush + commit same as row-at-a-time

### Series Query

1. `DataReader::series(topic_id, column_index)` validates the topic, column bounds, and numeric/bool value type
2. The returned `SeriesReader` treats the column as a field-level time series
3. Null physical rows are skipped; every `SeriesSample` has a value
4. Time lookups return valid series samples and virtual series indices
5. `bounds()` returns min/max over value-bearing samples only

### Row Query

1. `DataReader::rangeQuery(QueryRange{topic_id, t_min, t_max})` -> `RangeCursor`
2. Cursor binary-searches the chunk deque for start position
3. `forEach` callback receives `SampleRow` (chunk pointer + row index)
4. Caller reads values via `chunk->readNumericAsDouble(col, row)`, `readString()`, `readBool()`, `isNull()`, or batch via `readColumnAsDoubles()`

### Derived Scheduling

1. `engine.commitChunks()` returns changed topic IDs
2. Caller invokes `incrementalEngine.onSourceCommitted(changed_topics)` -> marks dependent nodes dirty
3. `incrementalEngine.scheduleAll()` processes dirty nodes in topological order
4. For each dirty SISO node: iterate new input chunks (id > `last_processed_chunk_id`), read each row, call `transform.calculate()`, write output via `beginRow`/`set`/`finishRow`, flush + commit
5. For each dirty MIMO node: iterate primary input's new chunks, for each timestamp check all other inputs via `latestAt()`, call `transform.calculate()` only when all inputs have matching timestamps

## 5. Key Invariants

- **Dense field IDs**: Field IDs within a topic are always 0, 1, 2, ... with no gaps.
- **Per-chunk sorted timestamps**: rows may arrive out of order (`beginRow()` / `appendTimestamps()` accept regressions — rejecting them silently lost multi-publisher data); `seal()` stable-sorts rows, so every sealed chunk is internally non-decreasing and duplicate timestamps keep arrival order.
- **ensureColumn guards**: Rejects new columns after a row is in progress. Invalidates stale 0-row builders when adding columns.
- **expandArray seals first**: `expandArray()` seals the current builder before modifying column layout, preventing mid-chunk schema changes.
- **Chunks may overlap in time**: `TopicStorage::appendSealedChunk()` accepts any chunk (deque stays commit-ordered). Queries merge across overlapping chunks; topic time extrema are scanned, never taken from the front/back chunks.
- **Lazy validity bitmaps**: `TypedColumnBuffer` only allocates a `BitVector` on first `appendNull()`. Sealed chunks include validity only when `hasNulls()` is true.
- **NaN for nulls**: `readColumnAsDoubles()` writes `NaN` at null positions, preventing confusion between null and zero.
- **ChunkId monotonicity**: `TopicChunkBuilder` uses a `static atomic<ChunkId>` counter starting at 1. `kInvalidChunkId` (0) is the sentinel for "no chunk seen yet".

## 6. Threading Model

`DataEngine` is guarded by one `std::recursive_mutex` (`Impl::mutex_`). The store
has exactly one reader thread (the Qt GUI: plot/catalog reads + `IncrementalEngine`
recompute) and one-or-more ingest worker threads, so an exclusive lock is
equivalent to a reader-writer lock here and simpler.
The derived engines' pool workers (`BatchEngine`, `OnDemandService`) keep the one-reader-thread rule by never
touching the `DataEngine`: they work only on copies the GUI thread staged before the run
(`docs/derived_objects/ARCHITECTURE.md` §11). EVERY engine access — read or
write, on any thread — holds the lock via `lockEngine()`. `DataWriter` accumulates
in-memory and is not itself shared; `TopicChunkBuilder::next_chunk_id_` remains a
`std::atomic`.

The mutex is **recursive** so the same thread may re-acquire it without
deadlocking: a held `RangeCursor`/`SeriesReader` whose consumer makes another read,
a compound mutator that calls another, or the write host's `appendRecord` →
`ensureField`. The one thing recursion does NOT make safe: calling a *mutator* from
inside a cursor `forEach` callback — that mutates the deque mid-iteration; don't.

Coordinated mechanisms:

1. **Stable addresses.** `topics` is `robin_map<TopicId, unique_ptr<TopicStorage>>`,
   so a `createTopic` rehash moves only pointers — a `TopicStorage` (and its
   `sealed_chunks_` deque) keeps its address for the engine's lifetime (topics are
   never erased, only retired). A cached `TopicChunk*` survives a concurrent
   `createTopic`.
2. **Cursors adopt the lock.** `DataReader::rangeQuery`/`series` take the lock
   *before* the `getTopicStorage` lookup (rehash protection) and **move it into**
   the returned `RangeCursor`/`SeriesReader`, which hold it for their whole (lazy
   `forEach`) lifetime. `SeriesCursor` borrows its parent's lock. `latestAt`
   materializes the row's values under the lock (no escaping `TopicChunk*`). The raw
   `getTopicStorage()` accessor does **not** lock — a caller must hold `lockEngine()`
   for the returned pointer's lifetime.
3. **The worker write path holds the lock.** The C-ABI write host (`WriteCore` /
   `DataWriter`, driven on the ingest worker) is a full engine client: each
   write-host method (`ensureTopic`/`ensureField`/`appendRecord`/`appendBoundRecord`/
   `appendArrowStream`/`createDataSource`, and `flushPending`) takes `lockEngine()`
   at entry, so its `getTopicStorage` reads and its `setColumnDescriptors` /
   builder mutations serialize against GUI reads. (`TopicStorage` has no internal
   lock; the engine lock is its mutual exclusion.) `TypeRegistry` likewise has no
   internal lock and is covered by the engine lock — every `lookup`/`registerOrGet`
   site (`IncrementalEngine::addSisoTransform`/`addMimoTransform`, the write host, the
   reader) holds `lockEngine()`. The GUI bypass readers
   `CatalogModel::rebuildFromDatastore` / `PointSeriesXY` take it too.
4. **Append vs. free/move asymmetry.** `commitChunks` only `push_back`s, so a
   worker append never invalidates a live cursor or cached pointer.
   `clearDatasetChunks`/`retireTopic`/`flushTo`/`replaceDatasetFrom`/
   `enforceRetention` (eviction `erase`s chunks) free or move chunk memory — these
   stay GUI-thread-only and a consumer must drop cached `TopicChunk*` (the
   drop-before-mutate drain) before they run.
5. **Two-engine (staging) lock order.** During pause/resume the write host mirrors
   topic/field structure to a **staging** `DataEngine`, and `flushTo`/
   `replaceDatasetFrom` move chunks between the two. Code touching both mutexes
   acquires them in ONE `std::lock` (the write host via `lockEngineDeferred()` ×2;
   `flushTo`/`replaceDatasetFrom` already do). Never hold one engine's lock while
   plain-locking the other — that ABBA-deadlocks against `flushTo`.

Public compound mutators keep a locking wrapper + a private `*Locked` worker (e.g.
`createTopic`/`createTopicLocked`, `commitChunks`/`commitChunksLocked`) so an
internal call reuses the primitive without a redundant re-lock; with the recursive
mutex this is an efficiency detail, not a deadlock guard.

| Operation | Thread | Lock |
|---|---|---|
| `commitChunks` (append) | worker | `lockEngine()`; never frees/moves chunks → no drain |
| write host `ensureTopic`/`ensureField`/`append*`/`flushPending` | worker | `lockEngine()` per call (dual `std::lock` with staging when mirroring) |
| `createTopic`/`createTopicField` | worker + GUI | `lockEngine()` (rehash; addresses stay stable) |
| read cursors (`rangeQuery`/`series`/`forEach`) | GUI | `lockEngine()`, held for the view's lifetime |
| `latestAt` | GUI | `lockEngine()`; value materialized before release |
| `getTopicStorage` (raw ptr) | any | none — caller must hold `lockEngine()` for the pointer's lifetime |
| `clearDatasetChunks`/`retireTopic`/`flushTo`/`replaceDatasetFrom`/`enforceRetention` | GUI only | `lockEngine()` + drop-cached-pointers drain first |
| `IncrementalEngine` recompute / `addSisoTransform`/`addMimoTransform` | GUI | `lockEngine()` across the read-modify-write |

The `ObjectStore` half has its own independent `std::shared_mutex` (see
`docs/OBJECT_STORE_DESIGN.md`); the two stores' locks are independent and must
never nest. Plugin sources that receive data on network threads still queue
internally and process in `onPoll()`.

**Pause/resume `WriteCore` swap (host-side, outside the engine lock).** On pause,
`DatastoreSourceWriteHost::setTarget` / `DatastoreParserWriteHost::setTarget`
rebuild the host's `WriteCore` on the GUI thread while the ingest worker may still
be inside the old one — a data race + use-after-free on the `WriteCore` object /
`state_->core`, not on engine state, so `lockEngine()` cannot cover it (both racers
touch the same `WriteCore`). It is made safe by holding `state_->core` as an atomic
shared_ptr — `AtomicSharedPtr<WriteCore>`, a small mutex-backed stand-in for
`std::atomic<std::shared_ptr<WriteCore>>` (libstdc++ ships that specialization only
from GCC 12, and the build/CI baseline is GCC 11): each worker callback `load()`s a strong
reference that pins the core for the call's duration, and `setTarget` builds the
replacement fully, then publishes it with a release `store()`. An in-flight append
finishes on the old core (kept alive by the worker's reference) — the same accepted
semantics as the object-store host's `std::atomic<ObjectStore*>`. Regression-guarded
by `engine_thread_safety_test`'s `SetTargetSwapVsWorkerEnsureTopicRace` under
ThreadSanitizer (`./build.sh --tsan`, and the Linux CI `tsan` job).

## 7. Testing

The tests in `tests/` are built into these runners (the authoritative live set is in `CMakeLists.txt`):

- `pj_datastore_tests`: one gtest runner for the whole module, registered with `gtest_discover_tests`, so ctest
  names are `<runner>.<Suite>.<Case>` and each case runs in its own process. Filter one suite with
  `ctest --test-dir build -R '^pj_datastore_tests\.ObjectStoreTest\.'`.
- `chunk_test`: a separate executable, because it holds death tests.
- `engine_concurrency_test`, `engine_thread_safety_test`: separate executables with a 90-second timeout; they are the
  ThreadSanitizer entry points used by `build.sh` and CI.

Sanitizer self-tests (`sanitizer_selftest` and the MSan and TSan variants) are registered only in their lane.

The suites are grouped by layer: buffers and encoding, schema evolution and topic storage, queries and series
readers, the engine round-trip and out-of-order ingest, `IncrementalEngine` and the data-processor substrate,
`ObjectStore` (owned and lazy payloads, retention, bundles, merge), the plugin host bridges, and the Arrow import
and round-trip.

`plugin_host_write_test.cpp` and `plugin_host_read_test.cpp` exercise the removed v3 write and read ABI. They are not
built (commented out in `CMakeLists.txt`) and have not been rewritten against the v4 Arrow-stream path
(`read_series_arrow`); `arrow_stream_round_trip_test` covers that path.

Build and run from the PJ4 repo root: `./build.sh`, then `ctest --test-dir build -R '^pj_datastore_tests\.'` (the separate executables run by their own names, e.g. `-R chunk_test`).
