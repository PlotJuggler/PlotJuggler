# pj_app technical notes

Measurements and incidents behind choices in [ARCHITECTURE.md](./ARCHITECTURE.md) and the code. Each entry says what was
observed, under which conditions, what the code does about it and when to measure again.

## File-load flush interval

- **Observed:** a flush copies each topic's new rows into its last chunk, one visit per column, and announces them to
  the GUI, so its cost follows the columns of the topics with new rows and not the rows. Loading a 30-minute ROS 1
  MCAP with 117k series (topics flattening variable-length arrays into up to 13k columns), flushing every 50 ms: 21.5 s,
  31.0 CPU-s (8.1 on the GUI thread), 2.62 GB RSS. Every 250 ms: 19.5 s, 27.2 CPU-s (6.2 on the GUI thread), 2.63 GB
  (Linux x86_64, i7-13700H, two runs each). The stored chunks are the same at both (`pj_datastore` TECHNICAL_NOTES
  "Chunks cut by rows, not by commits"); data shows during a load four times a second instead of twenty.
- **What the code does:** `PJ::kFileLoadFlushInterval` (`FileLoadFlushInterval.h`) sets the interval for file loads and
  fan-out entries; streams and toolbox imports do not use it. The flush runs on the load's worker thread on desktop and
  in the browser. Tests that need a mid-load flush wait longer than this constant.
- **Re-test:** if a flush stops visiting the columns no row set, or if data appearing during a load feels late.

## Browser preferences flush on a timer

- **Observed:** with `syncPreferences()` connected only to `QAbstractEventDispatcher::aboutToBlock` and
  `aboutToQuit`, no preference changed in the UI ever reached `localStorage`, so every one was lost on reload. Qt
  6.12.0 `wasm_multithread`: `QEventDispatcherWasm` emits `awake` but never `aboutToBlock` (its object file references
  no `aboutToBlock`), the WASM platform plugin installs no `pagehide`/`visibilitychange` listener, and a closed or
  reloaded tab never runs `aboutToQuit`. Chromium, the time-tracker button, 5 s idle.
- **What the code does:** `BrowserPersistence` polls `syncPreferences()` every `kPreferenceSyncInterval`; a poll with
  no change returns before touching durable storage. A reload within one interval of a change still loses it.
- **Re-test:** when Qt's WASM dispatcher emits `aboutToBlock` or the platform plugin reports page hide; an event-driven
  flush could then replace the poll.

## Reparenting a toolbox panel re-polishes it

- **Observed:** moving a toolbox panel between presentations by reparenting it cost about 180 ms per move in the
  browser build, because every reparent re-polishes the whole widget tree against the app stylesheet. Browser build,
  Qt 6.11.1; which browser was not recorded.
- **What the code does:** a move reparents the container once, at its destination; releasing it leaves its parent
  alone, and a teardown deletes it in place (ARCHITECTURE "Toolbox presentation").
- **Re-test:** after a Qt upgrade or a large stylesheet change, time a tab-to-floating move in the browser build.

## Lazy browser picks: read round trip and read-ahead

- **Observed:** a read of a lazily picked file (main thread posts a `file.slice()` to a reader Worker and spins until
  it answers) costs about 0.1–0.2 ms per round trip plus the copy. Browser build, Qt 6.12.0, pthreads.
- **What the code does:** each node keeps a read-ahead block (`kBlock` in `browser_picked_file.js`) that absorbs
  small sequential reads; reads of a block or more go straight through.
- **Re-test:** when a loader with a different read pattern (many small random reads) shows slow browser loads.

## Incremental SHA-256 in a Worker

- **Observed:** WebCrypto cannot hash incrementally, so a pick larger than one buffer cannot use it; the hasher
  Worker's incremental JavaScript SHA-256 runs at about 100 MB/s. Browser build, Qt 6.12.0.
- **What the code does:** a normal open hashes in the background (`Fingerprint::kInBackground`); only the paths that
  compare the hash wait for it (`kBeforeSelection`); reads of the mounted file never wait for the hasher.
- **Re-test:** if browsers ship an incremental digest API, or WebAssembly SIMD hashing becomes available to the build.

## MCP `export_series`: row grid and number formatting

- **Observed:** each exported series is already in time order, so building the export's row grid by merging the
  sorted stamps cost about a twentieth of sorting every stamp; printing doubles with `std::to_chars` (shortest
  round trip) was about 2.3–2.5x faster than `QByteArray::number`. Desktop build; the work that measured them
  exported ten series of 7.2M samples, but which case each figure came from was not recorded.
- **What the code does:** `writeExport` in `mcp/McpServerTools.cpp` merges; `std::to_chars` is used where
  `__cpp_lib_to_chars` advertises the floating-point overloads (Apple's libc++ has them only from macOS 13.3, above
  Qt's deployment floor), with `QByteArray::number` as the fallback.
- **Re-test:** when the macOS floor reaches 13.3, or if export time shows up in a profile.

## Layout import: saved paths on Windows

- **Observed:** remapping a layout's `*_dataset_path` qualifiers keyed by the resolved saved path missed on Windows,
  where a drive-less `/saved/x` is not absolute (`QFileInfo::isAbsolute` needs a drive or UNC root), so the
  serialized, resolved and cleaned forms differ. On Linux the three coincide, which hid it.
- **What the code does:** `LayoutImportBatch::registerRewrite` keys all three forms.
- **Re-test:** whenever path resolution in `LayoutXml` changes; run the layout-import suites on Windows.

## `--selftest-python` in packaged builds

- **Observed:** a packaged build shipped a Python backend that could not reach its own stdlib; users saw
  "undefined symbol: PyFloat_Type" when creating a Python Data Processor. No release step launched the packaged
  binary, so nothing caught it.
- **What the code does:** `main.cpp`'s `selftestPython` boots the embedded backend as a Data Processor does; the
  AppImage and macOS release workflows and the conda recipe run `--selftest-python` on the packaged binary.
- **Re-test:** it runs on every release; add it to any new packaging format.

## Browser repaint uploads with QRhi widgets

- **Observed:** in the browser build on Qt 6.12.0, any raster repaint in a window holding `QRhiWidget`s (the playback
  time bar on every tick) made Qt upload close to the whole window: about 35 MB per frame at devicePixelRatio 2.
- **What the code does:** `src/wasm/qt612_rhi_flush_override.cpp` replaces two Qt functions at link time
  (`PJ_WASM_QT_FLUSH_OVERRIDE`); its header says which part is a backport and which is local.
- **Re-test:** on every Qt bump (the file refuses to build on another Qt version); drop the backported part from
  6.12.1 and the rest once Qt carries an equivalent fix.

## Raster repaints during playback

- **Observed:** on Qt 6.12.0 (Linux, OpenGL, Iris Xe, a 1920x1080 window), a widget drawn into the window's backing
  store that repaints in the same frame as `QRhiWidget`s makes `QWidgetRepaintManager` repaint the backing store
  under every one of those views (their parents' backgrounds and whatever overlaps them) and makes
  `QPlatformBackingStore::rhiFlush` copy the whole window image and upload it again. With the playback slider and the
  time readout repainting on every 30 Hz tick, moving them to one tick in three removed 19% to 27% of the GUI thread's
  CPU in three layouts of plots, cameras and 3D views, and 50% in one of six cameras and a 3D view (`perf`, looping
  playback; the slider and the readout were the only backing-store widgets marked dirty). A frame in which only `QRhiWidget`s update skips all of
  it.
- **What the code does:** the slider, the readout and the Source Timeline needle follow
  `PlaybackEngine::coarseTimeChanged`, every second tick (about 15 Hz) while playing; a seek or a step still reaches
  them at once. One tick in three measured the same within noise, so the smoother handle was kept.
- **Known gap:** backing-store views that follow every tick while visible bring the cost back: a State Transitions
  dock, a Log panel revealing rows, a Custom Widget on a changing signal, and the curve list's Values column (its
  own 10 Hz timer, out of phase with the coarse tick). A live stream moves the cursor with every ingest, which is not
  a playback tick, so the three controls follow the ingest rate there.
- **Re-test:** when another backing-store widget starts to follow the playback tick (count the paint events per
  widget during playback: only `QRhiWidget`s should repaint between two coarse ticks), and on a Qt bump that changes
  `QWidgetRepaintManager::paintAndFlush` or `QBackingStoreDefaultCompositor::toTexture`.

## Exported static-archive symbols on Linux

- **Observed:** with Conan's static libpng exported from the executable, the dynamic linker interposed it ahead of
  the system copy GTK loads; the ABI skew (system libpng 1.6.43, Conan 1.6.58) crashed `QGtk3Interface::fileIcon`
  inside `QFileDialog` with an "invalid pointer" free.
- **What the code does:** `pj_app` links with `--exclude-libs,ALL` (GNU/LLD, Linux only), and `ExportedSymbolsGuard`
  checks that third-party archive symbols stay out of the dynamic table.
- **Re-test:** when the linker or the Conan image libraries change, run `ExportedSymbolsGuard`.

## Timeline slider seek rate

- **Observed:** a seek decodes images synchronously on the GUI thread, about 30–120 ms per image topic (conditions
  not recorded). Without a throttle each drag tick chained a fresh decode and the image lagged arbitrarily far
  behind the slider.
- **What the code does:** `TimelineWidget` coalesces slider seeks in a single-shot window (`seek_throttle_timer_`,
  about 30 Hz).
- **Re-test:** when image decode moves off the GUI thread; measure seek latency on an image-heavy dataset.

## Streaming defaults chosen by reasoning

- `kPollPeriodMs` in `StreamingSourceManager.cpp` (20 Hz poll) trades latency against per-poll host overhead;
  reasoning, not measured.
- `kStreamingObjectMemoryBudget` (per object topic) was set low so tail-buffer growth shows quickly in testing with
  raw 1080p images (about 10 frames at 6 MB); not a tuned production value. Re-test when images arrive compressed or
  users report evicted frames.
