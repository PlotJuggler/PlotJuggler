# pj_datastore architecture

How the module works today. Per-method contracts live in the headers; measurements and tried alternatives live in
[TECHNICAL_NOTES.md](./TECHNICAL_NOTES.md); the plugin-facing write and read patterns are in
[USER_GUIDE.md](./USER_GUIDE.md).

The module holds five pieces: `DataEngine`, the columnar store for scalar time series; `ObjectStore`, timestamped
opaque payloads next to it; `IncrementalEngine`, the derived-series graph over the engine; the host side of the plugin
C ABI (`plugin_data_host.hpp`); and the `PJ::proc` data-processor substrate. It is pure C++20 and links only `pj_base`
from the SDK, so it knows nothing of Qt, plugins being loaded, rendering or time display.

## Data model

- **Dataset → topic → chunk → column.** A dataset is one source (a file, a live connection) bound to a time domain. A
  topic is one stream: typed columns sharing one timestamp column. A topic either has a schema (a type tree registered
  in the `TypeRegistry`) or is schemaless and gets columns as they appear.
- **Labels.** Each dataset carries a label (`DataEngine::datasetLabel`) beside the SDK's `DatasetInfo`: its source
  name (`/` read as `_`, `Dataset` when empty) or a display name (`setDatasetLabel`), with the lowest free ` (N)` when
  another dataset holds it. Assigned at creation and held until `removeDataset()`, so another dataset coming or going
  never renames one, and a freed label names a later dataset, never a survivor; a relabel from the base the label was
  made from (a reload naming the dataset after its file again) keeps it. A merge source keeps its label but is listed
  nowhere (`isMergedAway()`), and `datasetByLabel()` maps that label to the anchor holding its data. The app's tree,
  the plugin catalog snapshot and every `label:topic/field` qualifier read it. Only the primary engine's labels are
  read: a staging or streaming mirror labels its datasets on its own. A label is a session name: layouts persist the
  id and `source_name`.
- **Timestamps** are absolute int64 nanoseconds. Nothing in this module subtracts a display base.
- **Types.** Each logical `PrimitiveType` maps to a storage kind (narrow integers are widened); 64-bit integers are
  never stored as double.
- **Columns are append-only.** Field ids are dense from 0 and never change once given, so adding a column never
  invalidates a `FieldId` or a column index. The app keys curves by (topic id, column index); reload and merge keep
  both (see "Reload, merge and removal").
- **Chunks** hold a topic's rows sorted by timestamp, at most `max_chunk_rows` each, in one column layout. A row never
  changes under its chunk's id: its values and its position stay. Only a topic's last chunk changes: it gains rows at
  its end as writers commit, and it is re-encoded when it fills or its source ends ("Ingest and schema evolution").
  When late rows have to go between its rows it is replaced, under a new id. Ids come from one process-wide counter,
  and within a topic they ascend in commit order: derived-series watermarks and gap detection rely on that, together
  with the count of rows read from the newest chunk. A chunk sealed whole can be committed after a newer one;
  `TopicStorage::appendSealedChunk()` then gives it a fresh id.

## Ingest and schema evolution

A plugin's write call reaches a write host ("Plugin host bridges"), which drives a `DataWriter`. The writer accumulates
rows in one chunk builder per topic and commits them with `DataWriter::commitRows()`. Only then do readers see them.
`commitRows()` returns the topics that changed, which is what `IncrementalEngine::onSourceCommitted()` takes.

- **A commit does not cut a chunk.** The committed rows extend the topic's last chunk (`TopicStorage::appendRows`)
  while it has room and their layout, so rows written in timestamp order give the same chunks at any commit rate.
  The last chunk stays unencoded while it grows. `DataWriter` sizes each builder to end where that chunk fills, so
  the builders after it hold whole chunks, sealed in the writer and committed as they are. `commitChunks(flushAll())`
  is the other path: one chunk per builder, stored as given.

- Rows arrive one at a time (columns not set become null) or as column batches; Arrow C streams go through
  `arrow_import` into the batch path. The two forms must not mix within a row.
- **Columns appear at any time.** This is the normal case: variable-length sequences flatten to `data[0]`, `data[1]`,
  … and grow with the message, and dynamic-schema formats (JSON) add keys per message. A new column goes at the end
  of the layout and starts a new chunk: a chunk has one layout, so earlier chunks have fewer columns. Readers
  bounds-check the column index, and tell a column a chunk lacks (the field did not exist when its rows were written:
  they are not rows of it) from a null in a chunk that has it. A variable-length array grows its element columns up
  to the topic's expansion limit; past it the extra elements are dropped and counted.
- **A column keeps its first type.** The write host stores a value of another type when the conversion is exact;
  otherwise that field is null for the row, the rest of the row is written, and the field is reported once through the
  ingest-warning handler. Registering an existing field with another type is an error.
- **Out-of-order timestamps are kept.** The rows of one commit are stable-sorted, so every chunk is ascending. Rows
  older than the last chunk's newest (a topic of several publishers) are merged into it in timestamp order, under a
  new id (`TopicChunk::replaced_id` keeps the first); readers rebuild and a derived node that read it replays ("Late
  input"). When the merge does not fit, is too large or has another layout, the rows start a chunk that overlaps the
  last one; the reads merge them ("Reading").
- Pre-registering fields is optional. It enables the bound-record write path.

## Storage, encoding and retention

Each topic's `TopicStorage` holds its committed chunks in commit order, its column layout and its array-expansion
state. The engine owns each `TopicStorage` through a `unique_ptr`, so its address never changes while the topic
exists.

**Encoding** is chosen per column when a chunk fills, or is sealed whole, from its statistics: constant when every
value is equal, frame-of-reference for signed integer columns and for float columns that hold only whole numbers, when
the offsets fit in fewer bytes than the values (decoding reproduces those floats exactly), dictionary for strings,
packed bits for bools, raw otherwise. A column has a validity bitmap (Arrow layout, LSB first) only when it holds a
null. The last chunk, while it grows, stores raw numbers, packed bools and a dictionary with one entry per run of
equal strings; `TopicChunk::compact()` gives it the encodings above with the function `seal()` uses, so a chunk that
grew is stored as one sealed with the same rows. A last chunk that will not fill is encoded when another chunk
follows it, and when its source ends: the write hosts' `finish()` (`TopicStorage::compactLastChunk()`). Skipping that
costs memory only, and rows committed later still extend the chunk.

**Nulls on read.** The per-cell accessors do not check: `readNumericAsDouble()` returns 0.0 at a null. The batch
`readColumnAsDoubles()` writes NaN, and series views skip null rows.

**Retention** runs only when called. The streaming manager trims its own dataset with `enforceRetention()`; nothing
trims a file dataset. Retention drops the chunks at the front of a topic whose newest row is older than the topic's
newest row minus the window, and raises the topic's retention floor: rows under the floor that sit in a chunk straddling
it stay in memory, but no read shows them. `evictTopicHistory()` raises the floor past everything the topic holds now.
Each topic also records the newest chunk id that retention dropped or cut into, which is how a derived node detects
input it never saw ("Derived series (IncrementalEngine)").

## Reading

`DataReader` is the read facade. The cursors it returns keep the engine lock for their whole life ("Threading and
locks").

- **Rows.** `rangeQuery()` gives a `RangeCursor`. Its `forEach()` yields rows in timestamp order across overlapping
  chunks (a heap merge whose step costs O(1) while chunks do not overlap). Its `forEachChunk()` yields each chunk's
  sorted run in commit order, so under overlap the runs interleave in time.
- **Latest at.** `latestAt()` returns the newest row at or before `t`; on a timestamp tie the later-committed chunk
  wins. The values are copied out under the lock, so no chunk pointer escapes.
- **Series.** `series(topic, column)` gives a `SeriesReader` over one numeric or bool column, as the samples that hold a
  value: null rows and chunks without the column are skipped. The time-based calls merge across overlapping chunks.
  The index-based ones do not (see "Known gaps").
- A topic's time range is computed over all its chunks, never read from the first and last chunk.
- **Whole-series snapshots** (the toolbox `read_series_arrow`, its windowed variant and the value scan, which the
  exporters and the MCP tools use) first call `DataEngine::awaitCompleteSeries()`, so a derived series still being
  replayed is finished before it is read. Plots read without that barrier.

## Threading and locks

The engine starts no threads. Its clients are the GUI thread (plots, catalog, derived recompute, every operation that
frees chunks) and the ingest worker threads (the plugin write hosts).

- **One recursive mutex per engine, held for every access**, read or write, on any thread, through `lockEngine()`. The
  raw `getTopicStorage()` and `typeRegistry()` do not lock: the caller holds the lock for as long as it uses what they
  return. `TopicStorage` and `TypeRegistry` have no locks of their own.
- **Cursors keep the lock.** `DataReader` moves the lock into the `RangeCursor` or `SeriesReader` it returns, because
  they read chunks lazily; a `SeriesCursor` relies on its parent's.
- **Recursive**, so a thread may lock again: a held cursor that reads more, one mutator calling another, a write host's
  append creating a field. Mutating from inside a `forEach` callback is still unsafe.
- **A commit changes only a topic's last chunk**, under the engine lock, on the writer's thread, reallocating its
  column buffers. A `TopicChunk*` survives that (the chunks live in a deque); a value's address, a `readString()` view
  or a column pointer lasts only as long as the lock it was read under. Everything that frees or moves chunks
  (retention, clearing, retiring, detach and reattach, `flushTo()`, `replaceDatasetFrom()`, `mergeDatasets()`,
  `removeDataset()`) runs on the GUI thread, after the consumers bound to that data dropped their cached chunk
  pointers.
- **Two engines are locked together.** While a stream is paused a second engine exists ("Streaming pause and resume").
  Code that holds both locks takes them in one `std::lock()` (`lockEnginePair()`); holding one engine's lock while
  plain-locking the other can deadlock against `flushTo()`.
- **The engine lock and the ObjectStore locks never nest.** The toolbox catalog reads the two stores one after the
  other.
- **The write-target swap runs under the engine locks.** `setTarget()` on a scalar write host replaces its write core
  (on the GUI thread for a pause or resume) while the worker may be inside the old one. The core is held in an atomic shared pointer: each
  worker call takes a strong reference and the core's engine locks, then checks that the core is still current, and
  retries with the new one if not. `setTarget()` flushes the old core and publishes the new one under the same locks,
  so the swap never frees a core in use and every row a call stages is either committed by that flush or written
  through the new core (`engine_thread_safety_test`).

## Streaming pause and resume

While a stream is paused the GUI keeps showing the primary engine and store, and ingest continues into a staging
`DataEngine` and `ObjectStore` owned by the streaming manager in `pj_app`. The write hosts switch between them with
`setTarget()`.

- **Ids stay in lockstep.** A write host given the other engine (`setSecondaryEngine()`) creates every topic and field
  on both, with the same ids, so a plugin that cached a topic or field handle keeps writing to the right column after
  a swap. Mirroring names the dataset by id, so the streaming manager creates the dataset on both engines with the
  same id. Object topics are registered on both stores with the same id.
- **Resume** swaps the targets back first, so a push already in flight finishes on the staging side, then moves the
  staged data into the primary with `flushTo()`: zero-copy, topics matched by (dataset, name). Each `flushTo()`
  checks every topic before it moves anything, so a failure changes neither side. The paused tail's retention floor and
  evicted-through chunk id move with it: a pause longer than the window ends as one gap for a derived node that had not
  seen the trimmed rows. `ObjectStore::flushTo()` gives the moved entries fresh UIDs and applies the destination's
  retention; the scalar side is re-trimmed by the caller.
- **Late paused rows are kept.** A paused row can be older than the primary's newest: a late sample, or a live row
  committed between the target swap and the flush. The scalar side appends the paused chunks as overlapping ones, as
  out-of-order ingest does, and derived nodes replay their input when they see rows older than they consumed. The
  object side merges the moved entries into timestamp order, after entries with an equal timestamp, as an
  out-of-order push does.

## Reload, merge and removal

These operations free or move data, so they run on the GUI thread after every reader bound to the dataset has been
invalidated (consumers drop their cached chunk pointers on `datasetAboutToBeReplaced`), with no event loop run in
between.

- **Reload.** The file is ingested again into a staging engine and store; `replaceDatasetFrom()` then moves that data
  into the primary dataset, matching topics by name. Dataset and topic ids, and the curve keys built on them, stay.
  Staged-only topics are created; primary-only topics are retired (scalar) or removed (objects); matched object topics
  take the reloaded metadata.
- **Refill in place.** `clearDatasetChunks()` and `ObjectStore::clearDataset()` empty a dataset but keep its topic ids.
  The detach and reattach pairs do the same transactionally: the data moves into a snapshot and goes back if the
  refill fails, and topics the failed refill added are retired or removed. The refill keeps each topic's column layout
  (field ids are dense positions that curve keys address), so a column the new file lacks comes back all-null;
  `TopicStorage::retireValuelessColumns()` (run by the runtime's prune after a complete refill) retires it. A retired
  column keeps its field id and stays stored as nulls; the listings (curve list, plugin catalog, name resolution of
  new processor inputs, the undo data universe) skip it, while an existing processor binding to it holds, as one to a
  retired topic does. Clearing, detaching and merging keep the retired set and a reattach restores it, so the column
  stays hidden through later refills; only a committed chunk with a value in the column un-retires it. A file whose
  column is entirely null is therefore hidden after a reload, though a fresh load lists it.
- **Merge.** `DataEngine::mergeDatasets()` and `ObjectStore::mergeDatasets()` fold source datasets into an anchor, each
  shifted by a raw delta the caller supplies; `pj_runtime` computes the deltas and owns the type-conflict policy.
  Scalar topics unite by name and columns by field path; the anchor keeps its ids and column indices, a conflicting
  field type is skipped and reported, and the rebuilt chunks are ascending and non-overlapping. Object topics with a
  shared name fuse by shifted timestamp with fresh UIDs; source-only ones move under the anchor and keep their id.
  Sources end empty but registered.
- **Merged payload stamps.** A merge moves each entry's store timestamp but never rewrites its payload, so it records
  the delta in the entry's `payload_stamp_shift`. A consumer that reads timestamps embedded in a payload (the TF
  buffer, the scene layers, the marker and script engines) adds it; a consumer of the store timestamp ignores it.
- **Removal.** `removeDataset()` erases the dataset and its topics; ids are never reused. `retireTopic()` frees a
  topic's chunks and hides it but keeps the `TopicStorage`, so a cached reader sees an empty topic rather than freed
  memory. `clearTopicHistory()` frees the chunks and resets the floor, for a replay that writes the same timestamps
  again.

## Derived series (IncrementalEngine)

`IncrementalEngine` runs a graph of transforms over engine topics and writes their outputs as engine topics. A SISO
node maps one single-column input to one output; a MIMO node maps N single-column inputs to M outputs and fires only
where every input has a sample at exactly the same timestamp. Nodes run in topological order; a cycle is refused when
the node is added. Each output column takes the kind its operator reports when the node is added (`outputKind(s)`),
numbers or text (`kString`); a value of the other family is coerced (a string into a number column writes 0, a number
into a text column leaves the cell empty).

- **Sequential contract.** A transform sees its input in ascending timestamp order, its state carries across chunks,
  and only `reset()` clears it. Anything that would break the order (late input, input lost to retention, a failed
  replay) therefore resets the node and replays its retained input in time order, never an incremental step.
- **Scheduling.** `onSourceCommitted()` marks the nodes that read the changed topics dirty; `scheduleAll()` runs the
  dirty ones, `scheduleActive()` only the given nodes and what they depend on. A SISO node reads the rows after its
  watermark: the input chunks newer than the newest it consumed, and the rows that chunk gained since. A MIMO node
  gathers the rows after its last joined timestamp. A dirty node whose inputs hold no newer chunk and no new row in
  the newest one returns at once, in time linear in its inputs, and leaves its downstream alone, so a streaming tick
  that dirties every node costs little for idle ones.
- **Late input.** An unread chunk that starts before a SISO node's last fed timestamp makes the node recompute (for a
  MIMO node: at or before its last joined one), and so do rows its newest chunk gained behind that timestamp (the
  chunk can sit behind an overlapping one). A chunk that took the place of one the SISO node read
  (`TopicChunk::replaced_id` at or below the node's watermark id) also does when it starts exactly at that timestamp:
  it holds rows the node fed, under an id the node has not seen. A MIMO node tracks the newest chunk id it consumed per
  input, and its rows, because a chunk sealed before another input's can be committed after it.
- **Recompute is transactional.** The node's current output is set aside, not cleared, and the graph below it is
  replayed. A successful replay drops the old output; an error or an exception restores it, so a failed recompute never
  leaves an output emptier than before. A node whose replay failed is fully recomputed on its next run, because its
  watermarks no longer describe its output and an incremental step would duplicate rows; restoring saved input
  bindings does the same. A failed node runs again only when new input arrives.
- **Retention gaps.** A node has a gap when retention dropped, or cut into, an input chunk newer than the newest chunk
  the node consumed, or hid rows that chunk gained before the node read them. Detection compares chunk ids, not
  timestamps, because feed timestamps advance only on non-null rows and exact joins, so an all-null or sparse input
  would look permanently behind. A MIMO node compares each input
  with the newest chunk it consumed from that input. The node then replays what is retained and records the gap (a
  bounded recent list, `gaps()`); `pj_runtime` shows it in the transform's configuration.
- **Time-sliced replay.** `materializeStep()` advances one node for about a given wall-time budget and commits what it
  produced. A node left part-way is deferred: the scheduling passes skip it until a step finishes it or a remove,
  replace, recompute or failure ends the replay. A SISO node over time-ordered input resumes mid-chunk and produces the
  same output as an uninterrupted replay; MIMO nodes and overlapping input finish in one step. Whole-series snapshot
  reads wait for an unfinished replay through the engine's complete-read barrier, which `pj_runtime` installs.
- **Replacing and reloading.** A node's operator can be swapped in place, keeping its topology, output topic ids and
  output kinds; the downstream graph is replayed and the old operator restored on failure. An operator whose output
  switches between text and numbers is refused, since the coercion would lose every value. The input bindings can be saved, checked
  against the reloaded schemas and restored, so a reload keeps node and output ids.

## ObjectStore

`ObjectStore` keeps timestamped opaque payloads (images, point clouds, annotations, serialized canonical objects) per
topic, to be selected by time and never expanded into columns. It does not decode payloads or read topic metadata: the
metadata JSON is kept verbatim. The one exception: registering a legacy `__markers__/<key>` name stamps the plot-marker
metadata on it.

**kLog series.** A topic whose metadata is canonical kLog (`isCanonicalLogMetadata`: `builtin_object_type` "kLog" and
no wire `payload_encoding`) is stored as a `LogEntries` series instead of payloads: each log is decoded once at push
(`pushOwned` and bundles decode before taking the lock; `pushLazy` is refused) into columns. Stamps, UIDs and the UID
order are whole-series vectors; the rest lives in chunks of `LogEntries::kChunkRows` rows. The last two chunks are
open (plain rows), so a late entry close to the newest reseals nothing; the others are sealed: narrowed source/category
ids into one dictionary, a level column (a constant when the chunk's rows agree), and the own-stamp offsets and the
messages in one zstd frame (level 3). A late entry inside a sealed chunk reseals that chunk only; a front eviction
drops whole chunks and only moves the first row of a sealed one. A read of a sealed chunk decompresses it once into a
two-chunk cache of decoded chunks per series (its own mutex, keyed by a seal generation, so a reseal never reads stale
rows): reads of nearby rows, the table view's among them, do not decompress again. A row's UID is its stable key (a
late entry or a front eviction moves its index, never its UID): `indicesAfterUid(newest)` gives the rows pushed after
a reader last looked, and `indexOfUid` finds a row again. The reads below serve a log series like any other, re-serializing canonical bytes per entry; typed reads go
through `logView()`, a `LogSeriesView` that holds the topic's shared lock for its lifetime (one view at a time, and no
store call while it lives, or a writer waiting for the lock deadlocks the thread). Retention applies as to any topic,
the streaming window included.
`flushTo()`, `replaceDatasetFrom()`, the merge and detach move `LogEntries` like entries, and refuse to mix a log series
with a payload series.

**Entries** are kept in non-decreasing timestamp order. An out-of-order push is inserted at its sorted position; equal
timestamps keep arrival order.

**Identity.** Each entry gets a `SequentialUID` from a process-wide counter, minted under the topic's write lock, so
UIDs rise strictly within a topic, are sparse (the counter is shared by all topics), are never reused and survive
eviction. UID order is arrival order, which an out-of-order insert separates from timestamp order; `OrderedEntries`
keeps the entries, their timestamps and a UID-order permutation in step. `flushTo()`, `replaceDatasetFrom()` and the
merge give the entries they move fresh UIDs, so a cursor below `firstSequentialUID()` detects with one comparison that
it missed evicted entries or that the dataset was replaced.

**Which read for which consumer:**

| Consumer | Read |
|---|---|
| The state at time `t` (a renderer every frame) | `latestAt()`, warm-cached; `latestEntryIdAt()` to skip an unchanged sample without fetching |
| Every entry exactly once, late arrivals included (the TF buffer) | `drainNewSince()` with a UID cursor |
| A time window | `rangeByTime()`, then `at(uid)` per entry; a UID walk would skip an out-of-order entry |
| A time-window cursor's high-water mark | `maxUidAtOrBefore()`, not `latestAt()`'s UID |
| "Did anything arrive behind what I evaluated?" | `oldestArrivalAfter()` |
| "Did the topic change?" | `contentIdentity()`: `content` on any change; `rewrite` unless the change was an append or a front eviction (time-ordered caches rebuild); `removal` only when entries were removed, replaced, shifted or re-UIDed, not on any insert nor a front eviction (an arrival-order reader keeps its cursor; one that must drop evicted entries checks them with `indexOf()`) |

**Payloads.** An owned payload is a shared buffer charged to the topic's memory budget. A lazy payload is a closure the
store runs on read, returning bytes plus an anchor that keeps them alive; the producer anchors on whatever already owns
them (a decompressed chunk, an mmap), so a read copies nothing. A lazy push can carry a seed, bytes the host already
holds: the shared `ResidentPayloadPool` keeps them, byte-bounded, and reads use them until the pool evicts them, after
which the entry fetches like any lazy entry. The pool charges a seed's payload size, but a seed anchored on a larger
allocation (the decompressed chunk it came from) keeps all of it alive, so the budget bounds payload bytes, not memory.
A resolved entry's bytes stay valid while its anchor is held, whatever the
store does next. A failed fetch is reported as such, not as an empty payload.

**Warm cache.** Only `latestAt()` uses it: one resolved entry per topic, keyed by UID, so a renderer asking for the same
sample every frame does not fetch again. It is dropped when that entry is evicted, when an out-of-order push may change
what an at-or-before lookup finds, and when the topic is flushed, replaced, emptied or detached (a detach snapshot does
not keep it). Failed or empty resolves and pool hits are not cached. For a lazy topic it keeps at most one payload
resident.

**Retention** is per topic: a time window behind the newest entry, a memory cap and an entry cap, each off at 0. It
runs after every insert and after `flushTo()` and the merge. Explicit eviction trims the oldest entries; removing
entries anywhere is a bundle that only drops. Each limit counts what expires, then evicts it in one pass.

**Bundles.** `pushOwnedBundle()` publishes one entry to each of several topics as one transaction, optionally dropping
entries first. The derived-object result rings in `pj_runtime` use it to keep a node's outputs consistent; they evict,
never reject.

**Threading.** One shared mutex for the store, then one per topic, always in that order; several topics lock in
ascending id order. Registration, removal, `clear()` and bundles take the store lock exclusively, so a bundle blocks
every reader and writer while it runs. Reads copy the entry under the topic's shared lock and resolve it with no lock
held, so a slow fetch blocks no writer; `latestAt()` re-locks only to store the cache entry, after checking it is still
retained. Nothing is destroyed under a store lock: dropping an entry can run plugin code (a context destructor, an
anchor release, a library unload) that calls back into the store, so each mutator collects what it unlinks and destroys
it after releasing the locks. The pool has its own mutex and releases anchors outside it. `drainNewSince()` waits for
the topic's lock, which a writer holds exclusively through an out-of-order insert (cost in TECHNICAL_NOTES.md
"Out-of-order ingest"), so keep it off latency-critical threads.

## Plugin host bridges

Plugins never link this module. They reach it through the C-ABI tables in the SDK's `pj_base/plugin_data_api.h`
(C++ views in `pj_base/sdk/plugin_data_api.hpp`), served by the host classes in `plugin_data_host.hpp`: source write,
parser write (bound to one topic the host chose), toolbox (scalar reads and writes, object writes, the catalog), source
and parser object write, and toolbox object read.

- Each host turns ABI calls into `DataWriter`, `DataEngine` and `ObjectStore` operations. The host commits
  (`flushPending()`) and, when its source ends, encodes the last chunks of the topics it wrote (`finish()`); plugins
  never do.
- An exception must not cross the ABI: entry points catch it and return an error status.
- The catalog snapshot names each data source by its label, not by the name it was created with: the handle is the
  identity, `PJ_data_source_info_t.name` a session label unique within one snapshot.
- The write hosts collapse repeated slashes in topic and field names (and drop a field's leading slash). A parser
  binding names its scalar topic through the same rule (`normalizeTopicName()`), so it reaches the topic the
  direct-write path made; object topic names are not normalized.
- The write hosts take the engine lock on every call, both engines when mirroring.
- Registering an object topic whose (dataset, name) exists with identical metadata reuses it, so a refill keeps the id
  and the curves bound to it; different metadata is an error.
- The store owns a lazy push's `fetch_ctx` only once the push succeeds, and then destroys it exactly once, when the
  entry is dropped; a rejected push leaves it with the plugin. Calls into one context are serialized, because the ABI
  keeps the returned buffer valid only until the next call.
- A toolbox object read returns an owning handle per entry, valid until the plugin releases it even if the store drops
  the entry.

## Data processors

`PJ::proc::DataProcessor` is the sample-in, sample-out processor interface (reset, per-sample and batch evaluation,
output kinds, saved parameters, failure state). It stays free of Qt and of any script engine, so `pj_scripting` builds
the Luau and Python filter classes on it ([FILTER_CLASS.md](../../pj_scripting/docs/FILTER_CLASS.md)) and
`pj_runtime`'s `DataProcessorService` instantiates processors. `ProcessorSisoAdapter` runs one as a SISO node of the
`IncrementalEngine`, so processors get the scheduling, recompute and gap behavior above.

## Known gaps

- `SeriesReader`'s index-based lookups (`sampleAt()`, `indexAtOrBeforeTime()`, `indexAtOrAfterTime()`) assume
  time-ordered chunks and are wrong when chunks overlap. The plots use them only to size buffers.
- A column added to a topic that has a schema exists only in the physical layout: `getTypeTree()` does not show it.
- A merge of late rows rewrites the whole last chunk, not only the rows behind the first late one, so it is bounded:
  a wide topic that is late past the bound gets a chunk per late commit.
- A new column cuts the topic's last chunk, however few rows it holds: a topic whose arrays keep growing gets a chunk
  per growth (TECHNICAL_NOTES "Chunks cut by rows, not by commits" has what letting the chunk gain the column broke).
- `finish()` encodes on its caller's thread, which is the GUI thread for a file load's last flush. A toolbox's own
  writes and a derived series have no `finish()`: their last chunk stays unencoded until another follows it.
- A chunk dropped whole by retention does not say how many rows it held: the rows it gained after a node last read
  it are not reported as a gap.
- A kLog topic pushed lazily (a plugin's `push_lazy`) is refused: the store decodes logs at push, and a lazy entry has
  no bytes yet. The plugin gets the store's error.
- A row begun and never finished (an exception between `beginRow()` and `finishRow()`) keeps its topic's uncommitted
  rows out of every later commit, `finish()` included.
