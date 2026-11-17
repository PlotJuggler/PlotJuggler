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
- **Timestamps** are absolute int64 nanoseconds. Nothing in this module subtracts a display base.
- **Types.** Each logical `PrimitiveType` maps to a storage kind (narrow integers are widened); 64-bit integers are
  never stored as double.
- **Columns are append-only.** Field ids are dense from 0 and never change once given, so adding a column never
  invalidates a `FieldId` or a column index. The app keys curves by (topic id, column index); reload and merge keep
  both (see "Reload, merge and removal").
- **Chunks** are immutable once sealed. Chunk ids come from one process-wide counter at seal, so a chunk sealed later
  has a larger id; derived-series watermarks and gap detection rely on that. A writer holds sealed chunks until it
  flushes, so chunks from different writers can be committed out of id order.

## Ingest and schema evolution

A plugin's write call reaches a write host ("Plugin host bridges"), which drives a `DataWriter`. The writer accumulates
rows in one chunk builder per topic, seals full builders, and hands the sealed chunks to `DataEngine::commitChunks()`.
Only then do readers see them. `commitChunks()` returns the topics that changed, which is what
`IncrementalEngine::onSourceCommitted()` takes.

- Rows arrive one at a time (columns not set become null) or as column batches; Arrow C streams go through
  `arrow_import` into the batch path. The two forms must not mix within a row.
- **Columns appear at any time.** This is the normal case: variable-length sequences flatten to `data[0]`, `data[1]`,
  … and grow with the message, and dynamic-schema formats (JSON) add keys per message. A new column seals the pending
  builder, so earlier chunks have fewer columns: readers bounds-check the column index and treat a missing column as
  null. A variable-length array grows its element columns up to the topic's expansion limit; past it the extra
  elements are dropped and counted.
- **A column keeps its first type.** The write host stores a value of another type when the conversion is exact;
  otherwise that field is null for the row, the rest of the row is written, and the field is reported once through the
  ingest-warning handler. Registering an existing field with another type is an error.
- **Out-of-order timestamps are kept.** Rows are stable-sorted within a chunk at seal, so every sealed chunk is
  ascending, but a topic's chunks can overlap in time; the reads merge them ("Reading").
- Pre-registering fields is optional. It enables the bound-record write path and avoids sealing mid-stream.

## Storage, encoding and retention

Each topic's `TopicStorage` holds its committed chunks in commit order, its column layout and its array-expansion
state. The engine owns each `TopicStorage` through a `unique_ptr`, so its address never changes while the topic
exists.

**Encoding** is chosen per column at seal from the builder's statistics: constant when every value is equal,
frame-of-reference for signed integer columns and for float columns that hold only whole numbers, when the offsets fit
in fewer bytes than the values (decoding reproduces those floats exactly), dictionary for strings, packed bits for
bools, raw otherwise. A column has a validity bitmap (Arrow layout, LSB first) only when it holds a null.

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
- **Appends never move anything.** `commitChunks()` only appends, so a worker commit leaves every GUI cursor and cached
  `TopicChunk*` valid. Everything that frees or moves chunks (retention, clearing, retiring, detach and reattach,
  `flushTo()`, `replaceDatasetFrom()`, `mergeDatasets()`, `removeDataset()`) runs on the GUI thread, after the
  consumers bound to that data dropped their cached chunk pointers.
- **Two engines are locked together.** While a stream is paused a second engine exists ("Streaming pause and resume").
  Code that holds both locks takes them in one `std::lock()` (`lockEnginePair()`); holding one engine's lock while
  plain-locking the other can deadlock against `flushTo()`.
- **The engine lock and the ObjectStore locks never nest.** The toolbox catalog reads the two stores one after the
  other.
- **The write-target swap is outside the engine lock.** `setTarget()` on a write host replaces its write core on the GUI
  thread while the worker may be inside the old one; the race is on the core object, not on engine state. The core is
  held in an atomic shared pointer: each worker call takes a strong reference for its duration, so the swap never frees
  a core in use (`engine_thread_safety_test` covers it under ThreadSanitizer). Rows an in-flight call stages in the old
  core after `setTarget()` flushed it are not committed (see "Known gaps").

## Streaming pause and resume

While a stream is paused the GUI keeps showing the primary engine and store, and ingest continues into a staging
`DataEngine` and `ObjectStore` owned by the streaming manager in `pj_app`. The write hosts switch between them with
`setTarget()`.

- **Ids stay in lockstep.** A write host given the other engine (`setSecondaryEngine()`) creates every topic and field
  on both, with the same ids, so a plugin that cached a topic or field handle keeps writing to the right column after
  a swap. Mirroring names the dataset by id, so the streaming manager creates the dataset on both engines with the
  same id. Object topics are registered on both stores with the same id.
- **Resume** swaps the targets back first, so a push already in flight finishes on the staging side, then moves the
  staged data into the primary with `flushTo()`: zero-copy, topics matched by (dataset, name). Each `flushTo()` checks
  everything before it moves anything, so a failure changes neither side. The paused tail's retention floor and
  evicted-through chunk id move with it: a pause longer than the window ends as one gap for a derived node that had not
  seen the trimmed rows. `ObjectStore::flushTo()` gives the moved entries fresh UIDs and applies the destination's
  retention; the scalar side is re-trimmed by the caller.
- Both `flushTo()`s require each moved topic to start at or after the destination's newest timestamp (see "Known
  gaps").

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
  refill fails, and topics the failed refill added are retired or removed.
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
the node is added.

- **Sequential contract.** A transform sees its input in ascending timestamp order, its state carries across chunks,
  and only `reset()` clears it. Anything that would break the order (late input, input lost to retention, a failed
  replay) therefore resets the node and replays its retained input in time order, never an incremental step.
- **Scheduling.** `onSourceCommitted()` marks the nodes that read the changed topics dirty; `scheduleAll()` runs the
  dirty ones, `scheduleActive()` only the given nodes and what they depend on. A SISO node reads the input chunks
  newer than the newest it consumed; a MIMO node gathers the rows after its last joined timestamp. A dirty node with no
  input chunk newer than its watermarks returns at once, in time linear in its inputs, and leaves its downstream alone,
  so a streaming tick that dirties every node costs little for idle ones.
- **Late input.** An unread chunk that lands at or before the node's last fed timestamp makes the node recompute. A
  MIMO node tracks the newest chunk id it consumed per input, because a chunk sealed before another input's can be
  committed after it.
- **Recompute is transactional.** The node's current output is set aside, not cleared, and the graph below it is
  replayed. A successful replay drops the old output; an error or an exception restores it, so a failed recompute never
  leaves an output emptier than before. A node whose replay failed is fully recomputed on its next run, because its
  watermarks no longer describe its output and an incremental step would duplicate rows; restoring saved input
  bindings does the same. A failed node runs again only when new input arrives.
- **Retention gaps.** A node has a gap when retention dropped, or cut into, an input chunk newer than the newest chunk
  the node consumed. Detection compares chunk ids, not timestamps, because feed timestamps advance only on non-null
  rows and exact joins, so an all-null or sparse input would look permanently behind. A MIMO node compares each input
  with the newest chunk it consumed from that input. The node then replays what is retained and records the gap (a
  bounded recent list, `gaps()`); `pj_runtime` shows it in the transform's configuration.
- **Time-sliced replay.** `materializeStep()` advances one node for about a given wall-time budget and commits what it
  produced. A node left part-way is deferred: the scheduling passes skip it until a step finishes it or a remove,
  replace, recompute or failure ends the replay. A SISO node over time-ordered input resumes mid-chunk and produces the
  same output as an uninterrupted replay; MIMO nodes and overlapping input finish in one step. Whole-series snapshot
  reads wait for an unfinished replay through the engine's complete-read barrier, which `pj_runtime` installs.
- **Replacing and reloading.** A node's operator can be swapped in place, keeping its topology and output topic ids;
  the downstream graph is replayed and the old operator restored on failure. The input bindings can be saved, checked
  against the reloaded schemas and restored, so a reload keeps node and output ids.

## ObjectStore

`ObjectStore` keeps timestamped opaque payloads (images, point clouds, annotations, serialized canonical objects) per
topic, to be selected by time and never expanded into columns. It does not decode payloads or read topic metadata: the
metadata JSON is kept verbatim. The one exception: registering a legacy `__markers__/<key>` name stamps the plot-marker
metadata on it.

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
| "Did the topic change?" | `contentIdentity()` |

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

- Each host turns ABI calls into `DataWriter`, `DataEngine` and `ObjectStore` operations. The host seals and commits
  (`flushPending()`); plugins never do.
- An exception must not cross the ABI: entry points catch it and return an error status.
- The write hosts collapse repeated slashes in topic and field names (and drop a field's leading slash); parser-binding
  and object topic names are not normalized.
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

- A stream resume fails as a whole when any topic's first paused row is older than the primary's newest timestamp,
  which out-of-order ingest or a commit landing between the target swap and the flush can cause; the paused data stays
  in the staging engine and store, and every later resume fails the same way.
- `SeriesReader`'s index-based lookups (`sampleAt()`, `indexAtOrBeforeTime()`, `indexAtOrAfterTime()`) assume
  time-ordered chunks and are wrong when chunks overlap. The plots use them only to size buffers.
- A column added to a topic that has a schema exists only in the physical layout: `getTypeTree()` does not show it.
- Rows a worker call stages in a write core after `setTarget()` flushed it are dropped with that core.
