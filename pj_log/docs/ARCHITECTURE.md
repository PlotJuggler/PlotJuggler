# pj_log architecture

## Components

```
pj_app (MainWindow, TopicDemandController, PendingDisplayBinder)
   │ creates, wires demand and time format
   ▼
LogDockWidget (IDataWidget) ── SessionManager / CatalogModel / PlaybackEngine (pj_runtime)
   │ a LogRowSource per topic id (over ObjectStore::logView), tracker, seek answers
   ▼
LogView (+ LogView.ui) ── LogTableModel ── LogStore + CompiledFilter (core, Qt-free) ── LogRowSource per topic
```

- **`LogRowSource`** is one topic's rows, read on demand: size, a stable key per row (`find(key, hint)` gives its
  index now, `indicesAfter(key)` the rows added since), time, level, source/category dictionary ids, own stamp, and
  messages in batches. `batch()` pins
  one state for a run of reads (the dock's source holds one `LogSeriesView` for it). `VectorLogRowSource` owns rows,
  for hosts that hand `LogEntry` lists and for tests.
- **`LogStore`** is the merged index: one `RowRef` (time, topic id, key) per row, ordered by (time, topic id, key).
  `update` drops the rows the source evicted (they sort at or before its oldest row, so only that prefix is checked)
  and merges in the rows keyed above the newest it indexed; `prune`, `removeWire` and `replaceWire` complete it. Each
  returns a `StoreChange` that says which rows were evicted, kept and added, so the model updates without a reset.
  Source and category ids map to panel string ids once per new id. Rows are read from the source by key on demand
  (the store's `LogEntries` caches what it decodes; a sequential reader passes the next index as a hint, so a filter
  pass looks nothing up); a row its source evicted before the dock's next update reads as absent (no text).
- **`CompiledFilter`** resolves a `LogFilter` (enabled levels, case-insensitive source/category/message substrings,
  hidden topics) against one store: the source and category needles are evaluated once per panel string, and the
  rows are read one topic at a time in one batch, messages in one call over the topic's rows in range.
  `LogLevel::kUnknown` rows always pass the level test. `collect` takes a row budget and returns where it stopped.
- **`LogTableModel`** shows the filtered rows; a filter over more than `kFilterSliceRows` rows runs one slice per
  event-loop turn, the previous result shown until the new one is complete (`filterApplied`). The last row at or before the tracker is tinted, later ones are greyed
  or hidden (`FutureRows`). A Warn, Error or Fatal level colours the Level cell only. Colours come from
  `FrameworkTokens`.
- **`LogView`** is the list, the toolbar (level toggles, filters, Topics menu, future-rows choice) and the detail pane
  with Copy. The Source and Category columns and their filter fields show while a row in the store fills them
  (`LogStore::hasSources` / `hasCategories`), the Topic column while several topics are shown. It is runtime-agnostic: the host hands it a `LogRowSource` per topic (`setTopicRows`, `updateTopicRows`) or
  `LogEntry` rows, sets the tracker and the `TimeLabelFormat`, and answers `seekRequested`. It keeps the entry the
  user selected (its `RowKey`: time, topic, rank among the topic's rows of that time, so a re-index finds it) and
  selects it whenever it is shown; while it is not (filtered out, hidden after the tracker, absent while a reload
  refills its topic) nothing is selected. Qt moves the selection to a neighbour when the selected row is removed, so
  every model change runs under `adjusting_` and is followed by `showChosen`. The selected row is read from the
  selection model, never from `currentIndex()`: a mouse drag selects a row before it makes it current. While paused,
  a list that shows the tracker's row keeps it in view however many rows land before it (a reload refills over
  several updates), and a topic indexed anew (`setTopicRows`: a merge, a reload, a display-offset change) rebuilds the
  rows under the viewport, so the list finds the tracker again, as on its first rows.
- **`LogDockWidget`** is the panel the dock system hosts (`IDataWidget`, kind and XML tag `log`).

## Data flow

The dock accepts canonical `kLog` topics, which the ObjectStore keeps as `LogEntries` (decoded once at ingest), and
indexes them through a `StoreLogSource`: each read takes an `ObjectStore::logView` (a batch, one for all its reads).
A row's key is its entry's UID, which a late entry or a front eviction leaves alone. Rows carry display-axis seconds
(`rawToDisplaySecondsAtOrAfter` of the entry's store stamp, so a seek to a row reveals it); the detail pane also shows
the log's own stamp (on the store clock, so a merge shift moves it too).

Entries landing (`dataPublished`, `ingestEnded`, `derivedEntriesPublished`) bring an update at once, then at most one
per `kEntriesLandedIntervalMs`. Each update reads the topic's identity; unless `removal` moved (a removal, a reload,
a merge) or the display offset changed, which re-index the whole topic, it drops the rows the store evicted and indexes
the entries with a UID above the newest indexed, late ones in their place. A topic no longer stored as `LogEntries`
leaves the panel and waits as pending. Rows the store's retention evicts leave the panel: a live
log keeps what the streaming window keeps, like every other topic. A topic that leaves the store (removed, reissued)
goes back to pending and binds again by the identity it was added with (its dataset's id, source and path, kept from
when the dataset was loaded). A topic or dataset the user deletes leaves the panel instead (`discardTopic` /
`discardDataset`, called by the shell before the store drops it). A merge moves the identity of the consumed datasets'
topics, shown or pending, to the merged dataset (`followMerge`, called by the shell on `AppSession::datasetsMerged`):
a topic the merge reparented shows again, and one it folded into the merged dataset's topic of the same name binds to
that one. A pending topic that resolves to a topic shown already counts as resolved.

## Tracker and seeking

The tracker tints the row at or before it. While playing the list follows the tracker and ignores scrolling; paused,
a tracker move brings its row into view. A double click on a row while paused emits `seekRequested`, and the dock
seeks the playback. The shell sets the app-wide `TimeLabelFormat` on every panel.

## Persistence

The filters, enabled levels, hidden topics and future-rows choice are attributes and children of `<log>`, with one
`<topic>` per topic carrying `dataset_id`, `dataset_source`, `dataset_path` and `topic`. Future rows are hidden unless `future_rows` says `grey`.
Topics that do not resolve are written back verbatim and retried on catalog and source-path changes; `finishRestore` makes the last attempt.
Advertised topics with no storage yet (a placeholder drop) wait as pending, and the shell holds their demand.

## Known gaps

- The index costs 24 bytes a row (`RowRef`), on top of what the store holds; the `QTreeView` adds about 42 bytes a row
  shown and a relayout of every shown row when a filter completes (164 ms at 1e7 rows, TECHNICAL_NOTES "Large logs").
- A topic evicting rows that sort after another topic's rows makes the model re-filter every row from the first one
  evicted (`StoreChange` expresses only a leading eviction).
- A late entry shows from the dock's next update on (up to `kEntriesLandedIntervalMs`); after a reload, every row reads
  as absent until it.
- A row read with no hint (a painted cell, the detail pane) is a UID lookup, O(log n) in the topic's rows.
- Only canonical kLog topics are accepted: a kLog topic stored with a wire `payload_encoding` (not a LogEntries series)
  is refused.
