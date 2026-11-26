# pj_runtime technical notes

What was observed, measured or tried behind the current code, with the conditions and when to look again. How the
code works is in [ARCHITECTURE.md](./ARCHITECTURE.md).

## Catalog rebuilds during a load

- **Observed:** loading a 30-minute ROS 1 MCAP with 117k series, `CatalogModel` rebuilt the whole catalog on most
  flushes (every 50 ms), and the rebuilds took most of the GUI thread. Deferring a rebuild that a commit triggers during
  an open ingest until four times the last rebuild's cost had passed cut GUI-thread CPU from 14.8 to 8.0 s, process CPU
  from 37.5 to 29.8 s and the load from 21.6 to 20.3 s (Linux x86_64, i7-13700H, two runs each).
- **What the code does:** see ARCHITECTURE "`CatalogModel` rebuild timing". The factor of four (rebuilds take at most a
  fifth of the GUI thread, while one costs under 250 ms) and the one-second cap (a new topic shows within a second)
  are reasoning; no other values were measured.
- **Re-test:** if a rebuild becomes incremental (cost proportional to the change, not the catalog), which would make the
  gap unnecessary.

## Qt's browser network backend keeps every response body

- **Observed (Qt 6.12.0, WebAssembly build):** `QNetworkAccessManager` runs each request through `emscripten_fetch()`
  with `EMSCRIPTEN_FETCH_LOAD_TO_MEMORY`. `QNetworkReplyWasmImplPrivate::downloadSucceeded()` and `downloadFailed()`
  (`qtbase/src/network/access/qnetworkreplywasmimpl.cpp`) copy `fetch->data` into the reply and then set
  `reply->m_fetch = nullptr` without calling `emscripten_fetch_close()`. `QNetworkReplyWasmImpl::close()`, the only
  caller of `emscripten_fetch_close()`, then finds no fetch, so the `emscripten_fetch_t` and its body stay in the wasm
  heap for the rest of the session. A download also peaks at three copies of its body: `fetch->data`, the reply's
  buffer and the caller's `readAll()`.
- **Measured:** opening a preset with a 261 MB recording left the wasm heap at 1084 MiB; a 96 MiB recording downloaded
  and then refused left 96 MiB more in use (`tests/wasm/presets.spec.js`, "a preset recording downloads without
  passing through the wasm heap"); a robot mesh kept its whole body (`tests/wasm/scene3d_url_models.spec.js`, "a URL
  robot mesh leaves no copy in the wasm heap").
- **What the code does:** `browserFetchToFile()` (`BrowserFetch.h`) bypasses Qt: the page's `fetch()` writes each
  chunk into a MEMFS file, which MEMFS keeps as a JavaScript array outside the wasm heap; the SHA-256 is one
  `crypto.subtle.digest` over that array once the body is complete. `pj_app`'s `PresetController` streams preset
  recordings through it (a picked data file is not copied at all: `pj_app`'s `BrowserPickedFile` reads it lazily,
  where Qt's `getOpenFileContent` would hold it whole in a `QByteArray`). Every download that wants the bytes (the
  other preset files; Scene3D's `UrlFetcher` for robot URDFs, meshes and textures) goes through `HttpGet.h`'s
  `fetchBytesWithLimit()`, whose browser side is `browserFetchToBytes()`: no hashing, a scratch file read back into a
  `QByteArray`.
- **Re-test** on every Qt bump: once `downloadSucceeded()` closes the fetch, a `QNetworkAccessManager` download stops
  leaking (it still peaks at the extra copies).

## Browser resident payload pool

- **Observed:** on a 261 MB recording in the browser, playback and seeking cost the same with a 64 MiB pool of resident
  object payloads as with the desktop's 256 MiB.
- **What the code does:** `SessionManager` sizes the browser pool at a quarter of the desktop one, because the wasm
  heap never shrinks and also holds everything else. A single payload larger than the pool is never admitted, so every
  read of it re-fetches it lazily from its source.
- **Re-test** when browser recordings grow well past that size or seeking in the browser is reported slow.

## Time-sliced replay in the browser

- **Observed (Chromium and Firefox):** the browser build runs Qt on the page's main thread (no `PROXY_TO_PTHREAD`). The
  zero-timer between two replay ticks is a clamped nested `setTimeout`, measured at about 5-6 ms after a busy tick, so
  with an 8 ms slice a tick cycle is about 13-14 ms and input and paint run in the gap. A 10 ms slice stalled
  Firefox's page timers for about 100 ms once per replay.
- **Observed (Qt 6.12.0, browser):** a repeating zero-interval `QTimer` stops firing whenever no other timer is
  running: `QEventDispatcherWasm::sendTimerEvents` re-arms the native `setTimeout` only when `activateTimers()` counted
  a fired timer, and that count skips zero-interval timers. On GitHub's ubuntu-22.04 runners in Firefox the replay of a
  30,000-row Luau transform stopped after about 7,000 rows (5 ticks), with the page's own timers still running, in 8 of
  8 runs; on faster machines other timers kept waking it.
- **What the code does:** `DataProcessorService::kMaterializationSliceBudget` is half a 60 Hz frame on every platform;
  a new persistent transform replays one slice inside the create call and the rest from a single-shot zero-timer that
  each unfinished slice starts again (every `start()` registers the timer, which arms the native one).
- **Re-test** when the browser build moves Qt off the main thread, or a browser changes its nested-timer clamping; the
  single-shot form can go once `sendTimerEvents` re-arms after any activation (still unchanged on Qt's `dev` branch).
