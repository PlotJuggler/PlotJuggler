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
- **Why some chunks are still scanned:** the statistics keep infinities, a chunk of nothing but NaNs has no extremes,
  and `t_min`/`t_max` count null rows. A NaN is in neither extreme, so a chunk that holds some answers from its
  statistics.
- **Re-test:** when the builder's statistics change (excluding infinities too would let every whole chunk without
  nulls skip the scan).

## Out-of-order ingest

- **Observed:** ROS recordings interleave publishers whose message stamps disagree; on `/tf` about 1% of messages
  arrive out of order, and a localizer can future-date its transforms. When the store rejected timestamp regressions,
  those rows were lost.
- **What the code does:** every row is kept. Scalar rows sort within a commit and the reads merge overlapping
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

## Chunks cut by rows, not by commits

- **Observed:** when every commit sealed a chunk per topic, a topic's chunk count followed how often its writer
  committed, and each chunk costs about 330 bytes per column whatever its rows (the column entry, its stats, a
  descriptor and its path). Loading a 30-minute ROS 1 MCAP with 117k series and topics of up to 13k columns (Linux
  x86_64, i7-13700H, runs alternated in one session):

  | Commit every | Stored | Chunks | Chunk-columns | Undersized | Load | CPU | RSS |
  |---|---|---|---|---|---|---|---|
  | 250 ms | a chunk per commit | 7,615 | 4.75 M | 6,804 | 18.6 s | 26.1 s | 3.68 GB |
  | 50 ms | a chunk per commit | 25,712 | 11.48 M | 25,458 | 20.7 s | 30.2 s | 5.72 GB |
  | 250 ms | last chunk grows | 2,105 | 1.19 M | 193 | 19.4 s | 27.0 s | 2.64 GB |
  | 50 ms | last chunk grows | 2,105 | 1.19 M | 193 | 21.5 s | 30.8 s | 2.64 GB |

  The 193 chunks that are not full are each cut by a new column (a growing array); 1,920 would be the fewest.

- **What was tried, in order:** the first version re-encoded a full chunk by rebuilding it through a builder,
  recomputed the stats of every appended row and destroyed each builder at commit: 26.4 s (a profile gave 4.0 s to the
  row copy and 4.8 s to the re-encoding). Encoding in place with the function `seal()` uses, one step for a column no
  row or every row of the batch set, the builder's own stats for a batch appended whole, and builders kept across
  commits brought it to about 19 s. Giving back the spare capacity of the buffers a full chunk keeps saved about
  0.1 GB, and so did one descriptor per topic column for all the chunks a writer makes of it, where every chunk
  allocated its own, name included (2.76 to 2.64 GB): a chunk-column now costs the column entry and its stats.
- **Letting the last chunk gain a new column, tried and taken out:** it left no chunk short of full (1,920 chunks,
  0.66 M chunk-columns, 0.13 GB less), but the rows written before the column existed became nulls of it, back to the
  start of the chunk. A reader tells "this chunk lacks the column" from "this cell is null" by the chunk's column
  count, in about a dozen places, and the read tools report the difference: for an array that grows at message 1,550
  of 3,000, `read_series` gave 1,976 samples with 526 invalid where the file holds 1,450, and `export_series` wrote the
  526 NaN rows. Plots, the tracker and filters skip nulls, so only the read tools show it;
  `scripts/qa/regressions/growing_array_field_has_no_rows_before_it.pjqa` covers it. A chunk keeps one layout.
- **Late rows:** with header timestamps, a `/tf` of several publishers had 97 chunks for 89,995 rows (88 is the
  minimum), 17 of them cut by rows older than the last chunk's newest; a live stream would cut such a topic about one
  commit in five. Merging those rows into the last chunk under a new id left 90 chunks. With the loader's default,
  log time, no chunk of four recordings was cut by a late row.
- **A chunk given a new id in place says which id it replaces.** A SISO node reads a chunk it has not seen from its
  first row, and replays only when the chunk starts before its last fed timestamp. A last chunk merged with a late
  row starts where it did, so a node that had fed nothing after the chunk's first timestamp fed those rows twice (a
  topic of two publishers: x at 100, y at 110, then x at 105). The chunk now keeps the id it replaced
  (`TopicChunk::replaced_id`) and such a node replays, so every late merge replays the nodes that read the topic,
  where a late chunk of its own replayed only those that had fed past it. Re-test when derived series over topics
  with late rows are slow: the rows before the first late one keep their positions, so a node that fed nothing after
  it could resume there (reasoning, not tried).
- **Not tried (reasoning):** sealing a snapshot of the growing rows at every commit costs their whole count times the
  columns each time, about 13 M cells a commit for a 13k-column topic near a full chunk. Merging small chunks in tiers
  leaves about ten trailing chunks a topic and renumbers chunks derived nodes have read.
- **Re-test:**
  - When topics with growing arrays matter more than these 193 chunks: the last chunk could gain a column if each
    column of a chunk recorded the row it starts at and every read path took the rows before it as rows the chunk
    lacks.
  - When a wide topic is late often (count the chunks that are not full and whose successor starts earlier): a late
    merge rewrites the whole last chunk, and past `kLateMergeCells` the rows start a chunk of their own; merging only
    the rows behind the first late one would lift that.
  - When the per-commit cost matters: it is one visit per column of every topic with new rows, which a list of the
    columns a batch set would cut for sparse topics.

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
