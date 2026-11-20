# pj_log technical notes

Measurements of the Log panel. Each entry says what was measured, under which conditions, what the code does about it
and when to measure again.

The numbers come from `log_panel_benchmark` (`pj_log/benchmarks/`, built with `PJ_BUILD_DEMOS`; how to run it is in
[../CLAUDE.md](../CLAUDE.md)). Conditions for every entry below, unless it says otherwise: Intel i7-13700H, Qt 6.12.0
(`PJ_QT_VERSION`), Release build, `QT_QPA_PLATFORM=offscreen` (Qt's raster painting into an image, no compositor and
no GPU, so painting costs differ from a real display), a 1280x720 panel, Qt's Fusion style without the app's
stylesheet (the shell's `Theme` lives in `pj_app`, which the benchmark does not link), synthetic `kLog` entries
(mixed levels, six sources, a few categories, 40-200 character messages) in a real `SessionManager` / `ObjectStore`.
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
grey is the default. Re-test after a change to `LogTableModel::updateRevealed` or `LogView::scrollToTracker`, on a
real display, with the app stylesheet, or with logs past 1e5 entries.

## Streaming: appends are cheap, late entries reload the topic every refresh

Measured: a producer thread pushes entries into the `ObjectStore` for 10 s, a 50 ms GUI timer sets the retention
budget and calls `notifyIngest(live)` as `StreamingSourceManager`'s ingest tick does (its default 5 s window and
64 MiB), the tracker follows the newest stamp at 30 Hz with the list following it, greyed future rows, no filter. A
refresh sample is the CPU of the GUI event in which the model took rows (the `kEntriesLandedIntervalMs` trailing edge,
or the `notifyIngest` of a leading one). "late" stamps each entry up to 50 ms before its arrival, as a merged
`/rosout` stamped with the sender's time does, so nearly every push lands before the newest stamp.

| msgs/s | order | GUI CPU % | refreshes (reloads) | refresh ms median | p95 | max | rows shown at the end |
|-------:|-------|----------:|--------------------:|------------------:|----:|----:|----------------------:|
| 100 | in order | 2.8 | 41 (0) | 0.32 | 0.44 | 0.48 | 1 000 |
| 1 000 | in order | 3.9 | 41 (0) | 0.51 | 0.74 | 0.82 | 10 000 |
| 10 000 | in order | 5.9 | 41 (0) | 3.0 | 5.3 | 6.0 | 100 000 |
| 1 000 | late <= 50 ms | 4.2 | 41 (40) | 3.0 | 6.5 | 7.4 | 4 985 |
| 10 000 | late <= 50 ms | 16.1 | 42 (41) | 33.4 | 57.4 | 75.3 | 49 770 |

In order, a refresh appends what landed after the topic's `drainNewSince` cursor and costs at most a few ms. With late
entries `contentIdentity().rewrite` moves on almost every push, so every refresh re-reads the topic; at 10 000 msgs/s
that reload of about 50 000 retained entries costs 33 ms median and up to 75 ms, past a 30 Hz frame, against 3 ms for
the append at the same rate. The reload also replaces the panel's rows with what the store still retains: with the
5 s window the panel ends with the last 5 s (4 985 and 49 770 rows) instead of the 10 000 and 100 000 the in-order runs
keep.

What the code does: `LogDockWidget::loadTopic` reloads a topic whose rewrite counter moved. Re-test when it gains an
incremental path for late entries, or when the retention defaults change.

## One-off costs at 1e5 entries

- Dropping an already-loaded topic of 1e5 entries on a shown panel (`tryAcceptObjectTopic`: read, decode, sort,
  insert): 49-72 ms wall for the call (medians of 5 in two runs), 76-88 ms CPU with the first paint.
- One filter change over 1e5 rows (what the debounce timer's `applyFilters` runs, plus the paint): 21 ms to set
  "timeout", 12.5 ms to clear it (medians of 5).

What the code does: both re-walk every row on the GUI thread in one go. Re-test with larger logs.

## Not measured

- Logs of 1e6 entries and `LogStore`'s capacity (`LogStore::kDefaultCapacity`): `LogView` constructs its store with
  the default and has no knob to lower it, and the runs stop at 1e5 entries.
- The app stylesheet, a real display and its compositor, and the browser build.
