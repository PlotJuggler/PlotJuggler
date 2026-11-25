# pj_datastore technical notes

What was observed, measured or considered behind the current code, with the conditions and the change that would make
it worth looking again. How the code works is in [ARCHITECTURE.md](./ARCHITECTURE.md).

## DataEngine lock

- **One exclusive recursive mutex, not a reader-writer lock.** When it was chosen the GUI thread was the only reader,
  so a shared mode had nothing to share; that was reasoning, not a measurement. `benchmarks/concurrency_benchmark`
  times a writer commit against a GUI reader walk under the lock. Re-test when reads run regularly off the GUI thread
  (a toolbox or MCP read on its own thread) or the GUI is seen waiting on ingest.
- **The write host's core is swapped through a mutex-backed atomic shared pointer.** libstdc++ ships
  `std::atomic<std::shared_ptr<T>>` only from GCC 12, and the build and CI baseline was GCC 11. Re-test when
  every supported toolchain is GCC 12 or newer: the standard type can replace the stand-in.

## Series bounds

- **Measured (2026-11-18, i7-13700H, offscreen, `UndoRedoLatencyBenchmark` in `pj_app`'s history tests,
  `PJ_BENCH=1`):** `SeriesReader::bounds` walked every sample through the merging cursor, about 40 ns each. A plot
  recomputes its zoom limits from it on every curve change, so it was 69% of an undo of 16 plots of 5 curves of 100k
  samples (1.74 s).
- **What the code does:** order does not matter for a min and max, so `bounds` skips the cursor; a chunk wholly inside
  the window answers from its `ColumnStats`, any other is scanned. The same undo takes 0.46 s.
  `SeriesReaderBounds.MatchesAFullScan` pins the result against the full scan.
- **Why some chunks are still scanned:** the statistics keep infinities, a NaN first value poisons them (the scalar
  path's first comparison), an all-NaN chunk leaves min > max, and `t_min`/`t_max` count null rows.
- **Re-test:** when the builder's statistics change (excluding non-finite values would let every whole chunk skip the
  scan).

## Out-of-order ingest

- **Observed:** ROS recordings interleave publishers whose message stamps disagree; on `/tf` about 1% of messages
  arrive out of order, and a localizer can future-date its transforms. When the store rejected timestamp regressions,
  those rows were lost.
- **What the code does:** every row is kept. Scalar rows sort within a chunk at seal and the reads merge overlapping
  chunks; ObjectStore entries are inserted at their sorted position.
- **Cost:** an ObjectStore out-of-order insert shifts the topic's entries, O(n) under the topic's exclusive lock: about
  1.1 ms per push at 200k entries (machine not recorded). A stream that is mostly unsorted degrades toward O(n²).
  Re-test when a source delivers mostly unsorted stamps, or a GUI-thread consumer has to wait on that lock.

## Sparse rows

- **Observed:** a topic whose rows set few of its columns (variable-length arrays flattened into up to 13k columns, most
  rows shorter) spent most of its write time padding every unset column with a null at each row. Loading a 30-minute
  ROS 1 MCAP with 117k series: 26.4 → 21.6 s and 44.4 → 37.5 CPU-s once a column was padded only when next set, or
  before bulk appends, `stats()` and `seal()` (Linux x86_64, i7-13700H, two runs each). CSV exports of eight sparse
  topics were byte-identical before and after.
- **What the code does:** `TopicChunkBuilder` pads a column lazily (`padColumn`, `padAllColumns`), writing its owed
  nulls in one step (`TypedColumnBuffer::appendNulls`, `BitVector::setNullRange`).
- **Re-test:** if rows become dense (most columns set per row), where the lazy path saves nothing.

## ObjectStore costs

- **Retention jumps.** Evicting the expired prefix in one pass instead of one entry at a time: 100k expired entries
  take about 8 ms instead of 1.3 s under the topic's write lock (`ObjectStoreBundleBench.RetentionJump` in
  `object_store_test`, run with `PJ_BENCH=1`). A jump happens when a window shrinks or a resume lands far past a
  paused tail.
- **Warm latestAt cache.** A profiler flagged the lazy fetch repeated every frame: a scene renderer at about 60 Hz asked
  for the same sample of a slower topic, and each call re-read and decompressed an MCAP chunk. No numbers were kept.
  The code keeps the last resolved entry per topic.
- **Resident pool classes.** With a single FIFO over one budget, a 373 MB MCAP's 598 point clouds (about 500 KB
  each) filled a 256 MB pool and evicted 19,817 of 29,360 admissions: 32% of the GUI's TF reads (about 200 B each)
  missed the pool and paid a file re-read plus a whole-chunk decompress, about 2.7 ms of GUI-thread CPU per message.
  All 28,761 TF messages together were under 6 MB. The pool therefore reserves a small share of its budget for small
  payloads, which only other small payloads can evict.
- **Removal compacts the whole series.** A bundle's `drop` shifts every survivor toward the front, so a sliding-window
  removal at the front moves every entry: about 7 ns per entry with the predicate (machine and size not recorded).
  Re-test when windows grow large: erasing a contiguous run with `deque::erase` moves only the shorter side.
- **FIFO, not LRU** (reasoning, not measured): the producer appends in time order and consumers chase the live edge,
  and FIFO keeps reads free of shared bookkeeping.

## Toolbox Arrow reads

Appending a numeric column to an Arrow array one row at a time cost about 36 ns a sample on a 7.2M-sample float64
series (machine not recorded). Whole-series toolbox reads therefore append each null-free numeric run of a chunk as one
block.

## Ingest field-key hashing

The write host probes a (topic id, field id) map once per field per record. `std::hash` of an integer is the identity
and `robin_map` masks the low bits, so xor-combining the two small ids left about 2k distinct hashes for about 200k
fields. The key is mixed with the splitmix64 finalizer instead; `ingest_benchmark`'s many-topics bound-append case
measures this path.

## Off-thread prefetch

Considered, not tried: a worker that resolves the samples ahead of the playhead, to take cold decompression off the GUI
thread. For an MCAP source every cold fetch serializes on one per-source mutex plus the cold chunk store's mutex,
because the cold reader is a single-cursor file reader; a background decompress would hold that mutex and block a GUI
fetch, even of a warm chunk, for the length of the decompress. Re-test when the cold reader can serve concurrent reads
(finer locking in the chunk store, or a second reader sharing its cache).
