# pj_log technical notes

Measurements of the Log panel. Each entry says what was measured, under which conditions, what the code does about it
and when to measure again.

The numbers come from `log_panel_benchmark` (`pj_log/benchmarks/`, built with `PJ_BUILD_DEMOS`; how to run it is in
[../CLAUDE.md](../CLAUDE.md)). Conditions for every entry below, unless it says otherwise: Intel i7-13700H, Qt 6.12.0
(`PJ_QT_VERSION`), Release build, `QT_QPA_PLATFORM=offscreen` (Qt's raster painting into an image, no compositor and
no GPU, so painting costs differ from a real display), a 1280x720 panel, Qt's Fusion style without the app's
stylesheet (the shell's `Theme` lives in `pj_app`, which the benchmark does not link), synthetic `kLog` entries
(mixed levels, six sources, a few categories, 40-200 character messages) in a real `SessionManager` / `ObjectStore` /
`CatalogModel` (the catalog is never rebuilt during a run).
All costs are GUI-thread CPU time. Run-to-run spread: a second run of the playback cases moved individual medians by
up to 2x (5.5 ms to 10.1 ms for one hide-mode case), so read the numbers as magnitudes.

## Playback: hiding future rows costs 4-5x greying them

Measured: the tracker moved at 1x by a 33 ms timer (`PlaybackEngine`'s tick) while the list follows it (playing), over
10 windows of 1 s spread across a 10-minute log; one sample is the CPU of one tick to the next (tracker update,
auto-scroll, paint). 290 ticks per case. Filter "timeout" keeps about 5% of the rows.

| entries | topics | future rows | filter | tick ms median | p95 | max | % of a core at 30 Hz |
|--------:|-------:|-------------|--------|---------------:|----:|----:|---------------------:|
| 1e4 | 1 | grey | none | 1.0 | 2.2 | 2.3 | 2.7 |
| 1e4 | 1 | grey | timeout | 0.06 | 0.09 | 2.5 | 0.4 |
| 1e4 | 1 | hide | none | 5.3 | 12.6 | 15.2 | 15.0 |
| 1e4 | 1 | hide | timeout | 0.06 | 0.09 | 12.2 | 1.0 |
| 1e4 | 3 | grey | none | 1.5 | 2.9 | 3.6 | 3.6 |
| 1e4 | 3 | grey | timeout | 0.06 | 0.09 | 2.6 | 0.4 |
| 1e4 | 3 | hide | none | 5.1 | 15.9 | 18.6 | 16.7 |
| 1e4 | 3 | hide | timeout | 0.07 | 0.11 | 17.2 | 1.3 |
| 1e5 | 1 | grey | none | 1.3 | 1.7 | 2.9 | 4.0 |
| 1e5 | 1 | grey | timeout | 0.06 | 1.4 | 1.8 | 1.0 |
| 1e5 | 1 | hide | none | 5.5 | 11.6 | 15.0 | 18.4 |
| 1e5 | 1 | hide | timeout | 0.07 | 10.1 | 16.8 | 6.5 |
| 1e5 | 3 | grey | none | 1.8 | 3.3 | 4.0 | 5.9 |
| 1e5 | 3 | grey | timeout | 0.06 | 1.8 | 2.7 | 1.3 |
| 1e5 | 3 | hide | none | 6.4 | 12.4 | 14.9 | 21.7 |
| 1e5 | 3 | hide | timeout | 0.06 | 8.7 | 12.9 | 5.3 |

A rerun of the hide-mode cases on 2026-11-20, with the severity colour on the Level cell only (it coloured the whole
row before) and a `CatalogModel` in the fixture, load average under 1.5:

| entries | topics | filter | tick ms median (before → after) | p95 | max | % of a core at 30 Hz |
|--------:|-------:|--------|--------------------------------:|----:|----:|---------------------:|
| 1e4 | 1 | none | 5.3 → 3.5 | 12.6 → 4.0 | 15.2 → 5.1 | 15.0 → 6.2 |
| 1e4 | 1 | timeout | 0.06 → 0.07 | 0.09 → 0.09 | 12.2 → 3.7 | 1.0 → 0.5 |
| 1e4 | 3 | none | 5.1 → 3.7 | 15.9 → 4.3 | 18.6 → 12.4 | 16.7 → 6.9 |
| 1e4 | 3 | timeout | 0.07 → 0.07 | 0.11 → 0.09 | 17.2 → 4.6 | 1.3 → 0.6 |
| 1e5 | 1 | none | 5.5 → 4.3 | 11.6 → 5.1 | 15.0 → 8.9 | 18.4 → 13.1 |
| 1e5 | 1 | timeout | 0.07 → 0.07 | 10.1 → 3.9 | 16.8 → 6.3 | 6.5 → 3.3 |
| 1e5 | 3 | none | 6.4 → 4.5 | 12.4 → 5.4 | 14.9 → 9.9 | 21.7 → 13.9 |
| 1e5 | 3 | timeout | 0.06 → 0.07 | 8.7 → 4.0 | 12.9 → 6.3 | 5.3 → 3.4 |

The rerun is 20-35% lower at the median and about half at p95 and max; the grey cases moved the same way (1e4, one
topic, no filter: 1.0 to 0.67 ms), so this is not attributed to the colouring: it is within the 2x spread above, and
hide mode still costs 3-5x grey.

No tick exceeded 19 ms. Greyed rows stay at 1-2 ms a tick, independent of the log size; hidden rows cost 5-6 ms a tick
and 15-22% of a core (26% in the second run). A perf profile (LBR, the playback cases) shows where: in the hide-mode
cases 70-87% of the samples are `QTreeView::drawRow` (text shaping and eliding under `QCommonStyle::viewItemDrawText`),
the rest mostly `QTreeViewPrivate::layout` under `LogView::scrollToTracker`. A tick that reveals a row inserts it
(`LogTableModel::updateRevealed`), and the inserted row makes `QTreeView` lay out every row again and repaint the
whole viewport; a greying tick emits `dataChanged` for two rows and scrolls, which repaints only those rows and the
strip the scroll exposes. With a filter the median tick reveals nothing (about 0.06 ms), and the ticks that reveal a
filtered row cost the same full repaint (the p95 and max columns). The relayout grows with the revealed rows: in the
1e5-entry, one-topic hide case its samples rose from 3 to 76 per second between the first and the last window (3% to
23% of that case's samples), so the hide-mode tick grows the further playback is into a long log.

What the code does: `LogTableModel` inserts and removes rows per tick in hide mode and only restyles in grey mode;
hide is the default, so a new panel pays the hide-mode cost. Re-test after a change to `LogTableModel::updateRevealed`
or `LogView::scrollToTracker`, on a real display, with the app stylesheet, or with logs past 1e5 entries.

## Streaming: late entries append like in-order ones

Measured: a producer thread pushes entries into the `ObjectStore` for 10 s, a 50 ms GUI timer sets the retention
budget and calls `notifyIngest(live)` as `StreamingSourceManager`'s ingest tick does (its default 5 s window and
64 MiB), the tracker follows the newest stamp at 30 Hz with the list following it, greyed future rows, no filter. A
refresh sample is the CPU of the GUI event in which the model took rows (the `kEntriesLandedIntervalMs` trailing edge,
or the `notifyIngest` of a leading one); a rebuild is a refresh in which the model also removed rows (a reload, or the
tail after a late row). "late" stamps each entry up to 50 ms before its arrival, as a merged `/rosout` stamped with the
sender's time does, so nearly every push lands before the newest stamp.

"Before": `LogDockWidget::loadTopic` re-read a topic whenever `contentIdentity().rewrite` moved, which a late entry
does. "After" (2026-11-20, load average under 1.5): it re-reads only when `removal` moved, and otherwise appends what
`drainNewSince` returns, `LogStore::insert` merging the late rows into place.

| msgs/s | order | GUI CPU % | refreshes (rebuilds) | refresh ms median | p95 | max | rows shown at the end |
|-------:|-------|----------:|---------------------:|------------------:|----:|----:|----------------------:|
| 100 | in order, after | 1.9 | 41 (0) | 0.21 | 0.30 | 0.51 | 1 000 |
| 1 000 | in order, after | 2.5 | 41 (0) | 0.33 | 0.41 | 0.46 | 10 000 |
| 10 000 | in order, before | 5.9 | 41 (0) | 3.0 | 5.3 | 6.0 | 100 000 |
| 10 000 | in order, after | 3.4 | 41 (0) | 1.9 | 2.6 | 2.7 | 100 000 |
| 1 000 | late <= 50 ms, before | 4.2 | 41 (40) | 3.0 | 6.5 | 7.4 | 4 985 |
| 1 000 | late <= 50 ms, after | 2.0 | 41 (40) | 0.31 | 0.39 | 0.54 | 10 000 |
| 10 000 | late <= 50 ms, before | 16.1 | 42 (41) | 33.4 | 57.4 | 75.3 | 49 770 |
| 10 000 | late <= 50 ms, after | 3.2 | 41 (40) | 1.9 | 2.6 | 3.5 | 100 000 |

Before, every late refresh re-read the topic: at 10 000 msgs/s a reload of about 50 000 retained entries cost 33 ms
median and up to 75 ms, past a 30 Hz frame, and replaced the panel's rows with what the 5 s window still retained.
After, a late refresh costs what an in-order one does (1.9 ms median at 10 000 msgs/s): it decodes only the new
entries, and the rebuild is the model re-filtering the tail from the earliest late row, about 50 ms of rows. The panel
keeps every row, like the in-order runs.

Re-measured 2026-11-27 against the index (the panel reads the store, own stamps 0-5 ms before the store stamp), with
the copying panel of the same day for comparison:

| msgs/s | order | panel | GUI CPU % | refreshes (rebuilds) | refresh ms median | p95 | max | rows shown at the end |
|-------:|-------|-------|----------:|---------------------:|------------------:|----:|----:|----------------------:|
| 10 000 | in order | copy | 3.5 | 41 (0) | 2.0 | 2.7 | 2.7 | 100 000 |
| 10 000 | in order | index | 3.2 | 41 (21) | 1.1 | 1.6 | 1.8 | 49 999 |
| 10 000 | late <= 50 ms | copy | 3.2 | 41 (40) | 2.0 | 2.7 | 2.8 | 100 000 |
| 10 000 | late <= 50 ms | index | 1.4 | 41 (39) | 1.2 | 1.7 | 1.9 | 49 779 |

The index shows what the 5 s window retains (the copy kept every row), and its in-order rebuilds are the rows that
window evicts leaving the model.

What the code does now: `LogDockWidget::loadTopic`
indexes the new rows, re-indexes a topic from its oldest late entry on when `rewrite` moved, and the whole topic only
on a `removal` move or a display-offset change; the panel holds no rows, so rows the store evicts leave it. Re-test
when the retention defaults change, or with lateness of seconds (the re-indexed tail grows with it).

## One-off costs at 1e5 entries

- Dropping an already-loaded topic of 1e5 entries on a shown panel (`tryAcceptObjectTopic`: read, decode, sort,
  insert): 49-72 ms wall for the call (medians of 5 in two runs), 76-88 ms CPU with the first paint.
- One filter change over 1e5 rows (what the debounce timer's `applyFilters` runs, plus the paint): 21 ms to set
  "timeout", 12.5 ms to clear it (medians of 5).

Measured when the panel copied each entry. What the code does now: an accept indexes every row on the GUI thread in
one go (reading times and ids, not messages); a filter change over more than `LogTableModel::kFilterSliceRows` rows
runs one slice per event-loop turn. Re-test with larger logs.

## Large logs

Measured 2026-11-27 (`log_panel_benchmark --only bigload`, Release, offscreen, one topic, own stamps 0-5 ms before the
store stamp; the "copy" build is the same benchmark against the panel that copied every row, capped at 1'000'000):

| rows | panel | panel RSS | accept | level toggle | filter "timeout" | clear | 10 screens scrolled | longest GUI event |
|-----:|-------|----------:|-------:|-------------:|-----------------:|------:|--------------------:|------------------:|
| 1e6 | copy | 265 MiB | 641 ms | 21 ms | 73 ms | 23 ms | 32.6 ms | the whole filter, in one call |
| 1e6 | index | 74 MiB | 111 ms | 47 ms | 186 ms | 51 ms | 33.6 ms | 12 ms filtering, 18 ms on clear |
| 1e7 | index | 594 MiB | 1229 ms | 502 ms | 1894 ms | 581 ms | 35.3 ms | 18 ms filtering, 106-165 ms on completion |

Wall times of a filter run until the list shows its result; the index filters in slices of
`LogTableModel::kFilterSliceRows`, so the GUI stays responsive meanwhile. 100 000-row slices took up to 22 ms with a
message needle, 50 000-row slices 12 ms. Scrolling costs what the copy did: the rows shown read from the store's
decoded-chunk cache, one decompression per chunk scrolled into view.

The completion event grows with the rows the result holds (18 ms for 5 % of 1e7, 165 ms for all of them), as does the
panel's RSS beyond the index (16 bytes a row) and the filtered list (4): about 42 bytes a row. Reasoning, not
profiled: `QTreeView` keeps one layout item per row and rebuilds them on each model reset; a `QTableView` or a
`QListView` with uniform rows keeps none.

What the code does: slices of 50 000 rows. Re-test when the list widget changes, or with logs whose messages are much
longer.

## Not measured

- The app stylesheet, a real display and its compositor, and the browser build.
