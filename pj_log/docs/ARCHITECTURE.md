# pj_log architecture

## Components

```
pj_app (MainWindow, TopicDemandController, PendingDisplayBinder)
   │ creates, wires demand and time format
   ▼
LogDockWidget (IDataWidget) ── SessionManager / CatalogModel / PlaybackEngine (pj_runtime)
   │ LogEntry rows per topic id, tracker, seek answers
   ▼
LogView (+ LogView.ui) ── LogTableModel ── LogStore + CompiledFilter (core, Qt-free)
```

- **`LogStore`** holds every row of every topic in one sequence ordered by (time, topic id, arrival). Repeated strings
  (source, category) are interned. Mutations (`insert`, `removeWire`, `replaceWire`) return a `StoreChange` that says
  which rows were evicted, kept and added, so the model updates without a reset. Beyond its capacity the oldest rows
  are evicted.
- **`CompiledFilter`** resolves a `LogFilter` (enabled levels, case-insensitive source/category/message substrings,
  hidden topics) against one store; `LogLevel::kUnknown` rows always pass the level test.
- **`LogTableModel`** shows the filtered rows; the last row at or before the tracker is tinted, later ones are greyed
  or hidden (`FutureRows`). A Warn, Error or Fatal level colours the Level cell only. Colours come from
  `FrameworkTokens`.
- **`LogView`** is the list, the toolbar (level toggles, filters, Topics menu, future-rows choice) and the detail pane
  with Copy. The Source and Category columns and their filter fields show while a row in the store fills them
  (`LogStore::hasSources` / `hasCategories`), the Topic column while several topics are shown. It is runtime-agnostic: the host hands it `LogEntry` rows, sets the tracker and the `TimeLabelFormat`,
  and answers `seekRequested`. It keeps the selection across every data call that leaves the selected row in the
  store.
- **`LogDockWidget`** is the panel the dock system hosts (`IDataWidget`, kind and XML tag `log`).

## Data flow

The dock reads each `kLog` topic from the session's ObjectStore and decodes it through `decodeStoredObject`
(`pj_runtime/CanonicalObjectPayload.h`), which applies the entry's stamp shift. Rows carry display-axis seconds
(`rawToDisplaySecondsAtOrAfter` of the entry's store stamp, so a seek to a row reveals it); the detail pane also shows
the log's own stamp.

Entries landing (`dataPublished`, `ingestEnded`, `derivedEntriesPublished`) bring an update at once, then at most one
per `kEntriesLandedIntervalMs`: each topic appends what `drainNewSince` returns after its cursor, late entries
included (`LogStore::insert` merges them into place), or is re-read when an entry the panel may hold was removed or
changed (`contentIdentity().removal`: a reload, a merge). A display-offset change re-reads every topic. The panel keeps
the rows the store's retention evicted: its own `LogStore` capacity bounds it. A topic that leaves
the store (removed, reparented by a merge) goes back to pending and binds again by identity.

## Tracker and seeking

The tracker tints the row at or before it. While playing the list follows the tracker and ignores scrolling; paused,
a tracker move brings its row into view. A double click on a row while paused emits `seekRequested`, and the dock
seeks the playback. The shell sets the app-wide `TimeLabelFormat` on every panel.

## Persistence

The filters, enabled levels, hidden topics and future-rows choice are attributes and children of `<log>`, with one
`<topic>` per topic carrying `dataset_id`, `dataset_source`, `dataset_path` and `topic`. Topics that do not resolve
are written back verbatim and retried on catalog and source-path changes; `finishRestore` makes the last attempt.
Advertised topics with no storage yet (a placeholder drop) wait as pending, and the shell holds their demand.

## Known gaps

- A re-read (a removal in the store, a display-offset change) replaces a topic's rows with what the store still
  retains: the rows retention evicted are lost then.
- Every row is held in memory up to `LogStore`'s capacity; the oldest are dropped beyond it.
