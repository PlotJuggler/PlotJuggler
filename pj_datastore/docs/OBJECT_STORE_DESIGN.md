# ObjectStore Design

`ObjectStore` stores timestamped opaque byte payloads alongside the columnar
`DataEngine`. It is for data that should be selected by time but should not be
expanded into scalar columns at ingest time.

## Responsibilities

`ObjectStore` owns:

- object topic registration scoped by `DatasetId`
- timestamped entries per topic, kept in non-decreasing order (out-of-order pushes are inserted, not dropped)
- eager payload storage through `pushOwned()`
- lazy payload storage through `pushLazy()`
- at-or-before timestamp lookup
- entry-index lookup and timestamp views
- per-topic retention budgets
- explicit topic eviction, removal, and clear operations

It does not decode payloads, interpret metadata, choose renderers, or own UI
policy. Topic metadata is opaque JSON retained verbatim for callers that need to
interpret object bytes.

## Data Model

```cpp
struct ObjectTopicDescriptor {
  DatasetId dataset_id;
  std::string topic_name;
  std::string metadata_json;
};

// Eager payload: store-owned bytes, counted against the retention budget.
using SharedBuffer = std::shared_ptr<const std::vector<uint8_t>>;
// Lazy payload: idempotent fetcher returning a view + ownership anchor.
// nullopt = the fetch FAILED (source unreadable/truncated/corrupt), surfaced to
// consumers as ResolvedObjectEntry::fetch_failed; an engaged view with zero
// bytes is a legitimately empty payload.
using LazyCallback = std::function<std::optional<sdk::PayloadView>()>;

struct ObjectEntry {
  Timestamp timestamp;
  SequentialUID sequential_uid;
  std::variant<SharedBuffer, LazyCallback> payload;
};

struct RetentionBudget {
  int64_t time_window_ns;
  size_t max_memory_bytes;
};
```

Topic names must be unique within one dataset. The same topic name may appear in
different datasets.

Entries are stored per topic in non-decreasing timestamp order. The common case
is an in-order push (amortized O(1) append). An **out-of-order push is lossless**:
the entry is inserted at its sorted position (O(n) shift) rather than dropped, so
the series stays ordered for at-or-before lookup. Equal timestamps are allowed and
preserve arrival order. This matches the scalar engine's lossless out-of-order
contract and is what lets real multi-publisher streams ingest fully — notably ROS
`/tf`, whose per-message publish timestamps legitimately interleave (~1%). (The
sorted-insert assumes regressions are the exception, not the rule; a pathologically
unsorted stream degrades toward O(n²).)

After an out-of-order insert, array/timestamp order diverges from UID (arrival)
order: the new entry sits at an earlier array slot yet carries the newest UID.
This is reconciled by an internal per-series `uid_order` side index (positions
into `entries` kept sorted by ascending UID), so the UID-cursor APIs stay a valid
binary-search path without `entries` itself being UID-sorted. The public
`entry_timestamps` / `EntryTimestampsView` sorted-vector contract is unchanged.

The three per-series arrays (`entries` + ascending `entry_timestamps` +
`uid_order`) are encapsulated in one internal value type,
`OrderedEntries` (`pj_datastore/ordered_entries.hpp`, which also owns
`ObjectEntry`). It holds the triple privately and enforces the ordering
invariants **by construction**: every mutation goes through a method
(`push`/`evictFront`/`shift`/`reuidAll`/`rebuildUidOrder`/the bulk cross-series
moves) that keeps the three arrays in lockstep, so no ObjectStore call site can
desync them by hand, and a `#ifndef NDEBUG` checker verifies the `uid_order`
permutation from the two O(n) paths plus an O(1) tail check on the in-order
append. `OrderedEntries` owns only the ordered-entry triple: locking (the
per-series `shared_mutex`), the warm `latestAt` cache, retention/memory
accounting, and payload resolution stay in `ObjectStore`/`ObjectSeries`, and the
caller holds the series lock across every call. This is pure encapsulation — the
public read/write semantics and the `entry_timestamps` / `EntryTimestampsView`
sorted-vector contract are unchanged.

## Entry Identity (SequentialUID)

Every entry carries a `SequentialUID` (`pj_datastore/sequential_uid.hpp`): a
stable identity assigned at insert from a process-wide atomic counter (under the
series write lock, so UIDs are strictly increasing within a topic). Value 0 is
the invalid/default sentinel.

Properties consumers can rely on:

- **Stable across eviction.** Unlike a deque index, a UID is never renumbered or
  reused when retention drops front entries. Replay cursors and decoded-payload
  caches key on it safely.
- **Sparse per topic.** Allocation is global across all topics, so consecutive
  entries of one topic are *not* consecutive integers. Never iterate a topic by
  incrementing UID values; a streaming consumer steps them with `drainNewSince()`.
- **A generation marker.** `flushTo()` and `replaceDatasetFrom()` assign fresh
  UIDs to the entries they move into the destination topic. A cursor whose UID
  falls below `firstSequentialUID()` therefore detects both "evicted past me"
  and "dataset replaced" with one comparison (eviction is front-only, so the
  first retained UID passing the cursor is exactly the missed-entry condition).
- **UID order == arrival order, independent of timestamp order.** After an
  out-of-order push the newest entry has the largest UID but sits at an earlier
  array slot; the arrival-cursor reads (`at(uid)`, `drainNewSince`,
  `firstSequentialUID`) stay O(log n) per entry via the `uid_order` side index
  rather than searching `entries` directly.
  `firstSequentialUID` returns the smallest *retained* UID, which need not be
  `entries.front()` (a partial eviction can leave a smaller-UID entry behind an
  out-of-order one).

## Write Paths

`pushOwned(id, timestamp, payload)` moves the caller-provided vector into a
shared buffer owned by the store. Owned entries contribute to `memoryUsage()`.

`pushLazy(id, timestamp, fetch)` stores a callable instead of bytes. The callable
is invoked on resolve and returns a `sdk::PayloadView` — a `Span<const uint8_t>`
paired with a type-erased `BufferAnchor` that keeps those bytes alive for as long
as the resolved view is held. The producer anchors on whatever already owns the
bytes (a decompressed chunk, an mmap, or a fresh allocation via
`sdk::makePayloadView`), so the store never copies on resolve. Lazy entries do not
contribute to `memoryUsage()` because the store retains the callable, not the
fetched bytes. The callable runs on every `at()` read; `latestAt()` repeats of the
same sample are served from a warm cache without re-invoking it (see Read Paths).

Kinds with a registered synchronous ingest tap use this unseeded form for
file-backed entries: their durable row is catalog metadata plus a re-readable
closure, while the tap consumes the transport bytes during the push.

`pushLazyWithSeed(id, timestamp, seed, fetch)` is `pushLazy` plus an ingest-time
seed: the pushing host already holds the payload bytes (its policy fetched them
synchronously inside the producer's push — the "hot path"), so instead of
dropping them it admits the `PayloadView` into the store's
**ResidentPayloadPool** (`setResidentPayloadPool`, one shared pool per app
session). Reads serve the resident bytes with no fetch until the pool's
byte-bounded FIFO evicts the seed; the entry then degrades permanently to the
`fetch` fallback — plain `pushLazy` behavior. Consumers chasing the ingest live
edge (a 3D dock rendering during a progressive file load) therefore never
re-fetch what the import just produced, while total residency stays bounded by
the pool capacity. Seeded bytes are pool-accounted, NOT part of the series'
`memoryUsage()`; entry destruction (retention, topic removal, clear) retires
the seed's pool charge, and entries moved across stores (`flushTo`,
`replaceDatasetFrom`) keep their slots. With no pool configured, an empty seed,
or a pool rejection (zero capacity, payload larger than the whole budget), the
push degrades to plain `pushLazy` up front. A resident-slot hit is deliberately
NOT memoized into the `latestAt` warm cache: it is already cheap, and caching
it would hold a second anchor owner outside the pool's budget. Accounting
charges `payload.bytes.size()`; a zero-copy anchored seed may pin a larger
upstream allocation (e.g. the producer's decompressed chunk) until eviction,
so the budget bounds logical payload bytes, not anchor-backed RSS.

Both write paths apply the topic retention budget after the new entry is
inserted.

## Dataset Merge

`ObjectStore::mergeDatasets(anchor, sources)` is the object-side companion to the
scalar dataset merge. It destructively folds source datasets into an anchor using
caller-supplied raw timestamp shifts.

- Shared object topic names are fused into the anchor topic: entries are
  interleaved by shifted timestamp, equal timestamps keep anchor entries before
  source entries, and the fused series receives fresh ascending
  `SequentialUID`s (via `reuidSeriesLocked`, which also makes `uid_order` the
  identity permutation) so `at(uid)` remains a valid binary-search path.
- Source-only object topics are reparented to the anchor dataset and keep their
  `ObjectTopicId`; source datasets are emptied.
- Duplicate or self sources are rejected before mutation, so that validation is
  atomic. A source with no object topics is accepted as a no-op.
- The fold is purely mechanical. It does not detect canonical object type
  conflicts; pj_runtime owns that preflight policy.
- Topic retention is re-applied after each fold. Lazy entries move by value, and
  their closures keep their original backing data alive.
- **Payload-embedded stamps follow the shift.** The merge moves each shifted
  entry's STORE `timestamp` but never rewrites the payload bytes, so timestamps
  encoded INSIDE a payload (a serialized canonical object, or a wire message a
  parser later decodes) would otherwise stay on the source's original clock. To
  reconcile them without touching the bytes, each shifted entry records the same
  delta in `ObjectEntry::payload_stamp_shift` (carried through `resolveEntry` to
  `ResolvedObjectEntry`). A consumer that keys off payload-embedded stamps adds
  it; one that keys off the store `timestamp` ignores it. It is 0 for unmerged
  data and accumulates across chained merges. The one consumer today is the 3D TF
  buffer, which indexes history by each transform's own inner stamp.

## Read Paths

`latestAt(id, timestamp)` returns the newest entry whose timestamp is less than
or equal to the query timestamp. It returns `std::nullopt` if the topic is
unknown, empty, or has no entry at or before that time.

`at(id, index)` resolves an entry by sequence index (timestamp-positional; the
in-order array slot, unaffected by the `uid_order` index).

`at(id, sequential_uid)` resolves an entry by stable UID (O(log n) via the
`uid_order` side index; nullopt when evicted, invalid, or from another
topic/generation).

`indexAt(id, timestamp)` returns the index that `latestAt()` would resolve.

`latestEntryIdAt(id, timestamp)` returns the `{sequential_uid, timestamp}` identity
of the entry `latestAt()` would resolve, without resolving its payload (metadata
only — never invokes a lazy fetch). Sampling consumers compare it against a
memoized sample identity *before* calling `latestAt()`, so an unchanged sample
never pays a cold refetch after its pool seed is evicted. It is not atomic with a
later resolve — a concurrent insert/eviction can change the pick in between — so a
skip-if-match consumer must re-check the resolved entry's `sequential_uid`.

`firstSequentialUID(id)` returns the smallest retained UID (O(log n) via the
`uid_order` side index — the front of UID order, which after an out-of-order
insert need not be `entries.front()`). `drainNewSince(id, cursor)` returns every
entry with UID strictly greater than `cursor`, in ascending-UID (arrival) order
with each payload resolved, then advances `cursor` — the single arrival-order read
primitive, for a streaming consumer ingesting each new entry exactly once into an
order-independent sink (the TF buffer). It catches late/out-of-order arrivals a
time window would miss, and is eviction-safe (an entry dropped before it resolves
is skipped but still advances the cursor). One binary search per entry.

`maxUidAtOrBefore(id, t)` returns the largest UID among entries with `ts <= t`
(invalid when none). This is the high-water arrival UID of everything at-or-before
`t` — distinct from `latestAt(t)->sequential_uid`, the newest-*timestamp* entry's
UID, which an out-of-order insert (old ts, newest UID) can leave *below* a retained
entry's. A live time-window consumer that tracks "everything consumed up to `t`"
must advance its cursor to this, not to `latestAt`'s UID, or a late out-of-order
entry is re-detected every tick. O(count of entries with `ts <= t`).

`rangeByTime(id, lo, hi)` returns the entries with `lo < ts <= hi` as
`(uid, timestamp)` pairs in ascending-timestamp order — the correct primitive for
a **time-window** consumer (e.g. incremental occupancy-grid updates). Use it
instead of walking the arrival-order UID cursor for a window: an out-of-order
entry (older ts, newest UID) can sit at any UID position, so a UID walk would skip
it. The call itself resolves no payload under the store lock; the caller resolves
each with `at(uid)` afterwards — one entry at a time under `at()`'s own brief lock,
nullopt if evicted in between. `hi <= lo` (and unknown/empty topics) yield an empty
window. O(log n + window).

`entryTimestamps(id)` returns an `EntryTimestampsView` that holds the series read
lock while the timestamp span is inspected.

Resolved entries contain:

```cpp
struct ResolvedObjectEntry {
  Timestamp timestamp;
  SequentialUID sequential_uid;
  sdk::PayloadView payload;  // { Span<const uint8_t> bytes; BufferAnchor anchor; }
};
```

`payload.bytes` is the resolved view; `payload.anchor` keeps those bytes alive
independently of later store mutation (eviction, removal, or `clear()`). For an
owned entry the anchor is the store's `SharedBuffer`; for a lazy entry it is
whatever the fetcher anchored on. An empty `anchor` means "no bytes".

### Warm cache (latestAt)

A ~60 Hz scene renderer calls `latestAt(topic, t)` every frame, but object topics
publish far slower, so consecutive calls usually resolve the *same* sample. To
avoid re-invoking a lazy fetcher (which, for the MCAP source, re-decompresses a
chunk and re-copies the payload) on every frame, each series keeps a one-slot warm
cache: the most-recently-resolved `ResolvedObjectEntry`, keyed by its
`sequential_uid`.

- **Scope.** Only `latestAt()` consults and populates the cache. `at(index)` and
  `at(uid)` always resolve fresh, preserving their per-read semantics (a prefetch
  or replay walk through `at()` therefore cannot evict the renderer's current
  sample).
- **Hit/miss.** A hit (cached `sequential_uid` matches the looked-up entry)
  returns the cached entry without invoking the fetcher. A miss resolves once and
  caches the result — but only if the payload is non-empty, so a failed/empty
  resolve is retried on the next read rather than latched.
- **Identity is exact.** `SequentialUID` is never reused, so a hit is always the
  same entry; no validation against the underlying bytes is needed.
- **Invalidation.** The slot is dropped when its entry is evicted (`evictFront`
  matching the cached UID) and when the series is replaced or flushed
  (`replaceDatasetFrom`, `flushTo`). An **in-order** push never needs to
  invalidate (the new entry has a new UID, so a query mapping to it simply
  misses), but an **out-of-order** push does — it can change which entry an
  at-or-before lookup lands on, so the cached `latestAt` slot is dropped. The
  `OrderedEntries::push()` return value (`kInOrderAppend` vs `kOutOfOrderInsert`)
  is what tells the caller which case to reset.
- **Residency.** For a lazy topic this keeps at most one materialized payload
  resident per topic (the current sample) — a deliberate relaxation of "lazy
  entries are never resident". It never holds more than the current entry, so the
  whole series (e.g. a full video) is never brought into memory. The warm payload
  is not counted by `memoryUsage()` (which tracks owned buffers only).

## Retention

Retention is configured per topic:

- `time_window_ns > 0`: drop entries older than `newest_push_ts -
  time_window_ns`.
- `max_memory_bytes > 0`: drop oldest entries until owned-payload memory is at
  or below the cap.

Either axis can be zero to disable that axis. Both zero disables automatic
retention.

Automatic retention runs only during `pushOwned()` and `pushLazy()`. Explicit
eviction is available through `evictBefore(id, threshold)` and
`evictAllBefore(threshold)`.

Memory accounting includes only owned payloads. Lazy entries are counted as zero
bytes because the store retains a fetch callable, not the fetched payload.

## Threading

The store has one global shared mutex for topic lookup and one shared mutex per
topic series. Reads can proceed concurrently with reads on the same or different
topics. Writes take the target topic's exclusive lock. Topic registration,
removal, and `clear()` take the global exclusive lock.

The per-series `uid_order` side index (inside `OrderedEntries`) is maintained
under that same per-series mutex as `entries`, written exclusively in lockstep
with them, and is never handed out through a view — so it introduces no new drain
contract. `OrderedEntries` is not itself locked; the caller holds the series lock
across every call to it.

The warm cache (above) has its own small per-series mutex, distinct from the
series shared mutex. Resolution is **snapshot-then-resolve**: `latestAt`/`at`
copy the target entry (cheap — the payload variant copies as refcount bumps /
a closure copy) under the series shared lock, RELEASE that lock, and only then
invoke `resolveEntry()`. A slow lazy fetch (file re-read + decompress) therefore
never blocks writers pushing to the same series, and never holds the cache
mutex. Only the global store shared lock stays held across the resolve — it
keeps the series object alive and conflicts only with registration-level
exclusive operations, never with pushes. An entry evicted mid-resolve is
harmless (the snapshot owns its captures); the warm cache may then briefly
memoize that already-evicted payload, bounded at one per topic, until the next
differing read replaces it.

The ResidentPayloadPool has its own mutex, taken briefly by seed admission and
by slot retirement (which entry destruction may trigger under store locks —
store→pool nesting only; pool code never takes store locks). Payload anchors of
evicted seeds are always released outside the pool mutex.

### Deferred: off-thread prefetch

A background worker that warms upcoming samples ahead of the playhead was
considered, to move genuine new-frame decompression off the GUI thread entirely.
It is **deferred**: for the MCAP source, all cold fetches of a source serialize on
a single per-source mutex (`DataSourceRuntimeHost::lazy_fetch_mutex_`) plus
`ColdChunkStore`'s own mutex, because the cold reader is a single-cursor
`FileReader` that is not concurrent-safe. A background decompress would hold that
mutex and block a concurrent GUI fetch — even for an already-warm chunk — for the
decompress duration, relocating the stall rather than removing it. Making prefetch
genuinely off-load the work requires concurrent-izing that single-cursor reader
(finer `ColdChunkStore` locking, or a second reader sharing the chunk cache),
which is the real cost of this feature; the prefetch controller itself is the easy
part.

## Plugin ABI Bridge

Plugin access to `ObjectStore` is provided by three optional v4 services:

| Service | Host implementation | Purpose |
|---|---|---|
| `pj.source_object_write.v1` | `DatastoreSourceObjectWriteHost` | DataSource plugins register object topics and push owned or lazy entries. Re-registering an existing (dataset, name) with identical metadata reuses the topic (the replace-refill contract — the id and its bound curves survive); different metadata still fails. |
| `pj.parser_object_write.v1` | `DatastoreParserObjectWriteHost` | MessageParser plugins push entries to a host-bound object topic. |
| `pj.toolbox_object_read.v1` | `DatastoreToolboxObjectReadHost` | Toolbox plugins look up topics and read entries as owning byte handles. |

The raw ABI lives in `pj_base/include/pj_base/plugin_data_api.h` of the
`plotjuggler_sdk` package; the C++ SDK views live next to it in
`pj_base/include/pj_base/sdk/plugin_data_api.hpp` (the in-code `#include` paths
are `pj_base/plugin_data_api.h` and `pj_base/sdk/plugin_data_api.hpp`).

The toolbox read ABI allocates one owning handle per successful read. The handle
keeps bytes alive until the plugin releases it, even if the store evicts or
removes the underlying topic.

## Tests

Core behavior is covered by:

- `pj_datastore/tests/ordered_entries_test.cpp` (the `OrderedEntries` invariant
  at the value-type boundary — in/out-of-order push, evict, re-UID, rebuild,
  shift, and the cross-series bulk moves)
- `pj_datastore/tests/object_store_test.cpp`
- `pj_datastore/tests/plugin_data_host_object_test.cpp`
- `pj_datastore/tests/plugin_data_host_object_read_test.cpp`
- `pj_datastore/tests/plugin_parser_object_write_test.cpp`

The `uid_order` side index (encapsulated in `OrderedEntries`) that keeps the UID
cursor correct under out-of-order ingest is pinned by, in `object_store_test.cpp`:
`OutOfOrderPushRemainsReachableByUidCursor` (the guard),
`NextUIDAfterWalkVisitsOutOfOrderEntryLast`,
`NextUIDAfterWalkAfterMultipleOutOfOrderInserts`,
`EvictionOfOutOfOrderEntryKeepsUidCursorConsistent`,
`FirstUidNotFrontEntryAfterPartialEvict`,
`EqualTimestampArrivalOrderPreservedByUidCursor`,
`SrcUidOrderClearedAfterFlushAllowsSubsequentPushes`, and
`UidWalkAfterAppendingFlushVisitsAllEntriesInOrder`; in `object_merge_test.cpp`:
`UidWalkAfterMergeVisitsEveryEntryAscending`; and in `clear_dataset_test.cpp`:
`ReattachPreservesUidCursorAfterOutOfOrder`.

### Drain lock discipline

`drainNewSince` snapshots the unconsumed entries under a **single** acquisition
of `store_mutex_` (shared) + the series mutex (shared), then resolves payloads
with **no lock held** — a lazy resolve may re-read and decompress from a file
and must stall neither the series' writer nor, via the store-wide mutex, every
other topic. The cursor advances only as each snapshotted entry is resolved, so
a drain never reports past what it returns.

Waiting for the series lock is acceptable here because no drain runs on a
latency-critical thread: FrameTransforms are decoded by the worker-side ingest
tap (see `pj_scene3D`), and the remaining drains are cold rebuilds. A writer's
exclusive hold does have a real worst case — `OrderedEntries::push`'s
out-of-order branch is O(n) under the exclusive series lock (~1.1 ms/push at
200k entries) — so a future GUI-thread consumer would need a non-blocking
variant rather than this one.
