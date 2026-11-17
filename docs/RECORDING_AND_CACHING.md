# Recording and caching

How PlotJuggler 4 keeps the bytes that entered it and replays them later: the **session recorder** (Record/Stop on live
sources) and the **source cache** (reusable artifacts for cloud-connector downloads). It covers the intent, the
architecture, the accepted trade-offs, the known gaps and, at the end, the evidence behind the choices.

## Intent

Two user needs share one mechanism:

1. **Keep what I am streaming.** A live source (ROS 2, Foxglove bridge, MQTT, UDP, ZMQ, pj_bridge, …) is
   memory-bounded eager data. Users want to keep a session as a file, reopen it later, hand it to a colleague.
2. **Do not download the same thing twice.** A cloud connector fetches a reproducible request. A saved layout should
   restore it offline, instantly, from a local artifact.

Both reduce to *"store the messages that entered PlotJuggler, replay them with the same machinery that opens a bag."*
The host is the only component that sees every plugin's ingest, so the host owns the mechanism; plugins own only what
is genuinely theirs (transport, request identity, trust, credentials).

## Goals and non-goals

**Goals**

- A recording or cache artifact is a **standard MCAP file** that any MCAP tool, and PlotJuggler's stock loader, opens.
  There is no PlotJuggler-native format.
- **No plugin change for recording**; **one plugin call for caching**. Every delegated-ingest source is recordable and
  cacheable without knowing it.
- Recording never slows ingest. Caching never slows a hit.
- Failure containment: a slow disk, a full budget, a crash mid-write never damages the live session or another
  source's file.

**Non-goals**

- Recording decoded samples or PlotJuggler-internal state (transforms, plots): that is the layout's job.
- A "record everything on the wire" mode: recordings contain what the session subscribed to.
- Format-level fidelity for parser *policy* (timestamp field, array limits): policy is interpretation and lives in the
  layout ("Accepted trade-offs").
- Browser (WebAssembly) recording. The core compiles there; only a sink is missing.

## Architecture

### The seam

`pj_runtime`'s `DataSourceRuntimeHost` implements the SDK's **delegated-ingest** ABI for one source:
`ensureParserBinding(topic, encoding, type_name, schema_bytes, parser_config)` then
`pushMessage(binding, log_time, bytes)`. That seam *is* the MCAP data model: binding ↔ `Schema` + `Channel`, push ↔
`Message`. The recorder is a mirror of the seam into an MCAP writer, and **replay is `data_load_mcap` plus the
installed parsers**, indistinguishable from opening a rosbag. A parser fix therefore applies to old recordings.

```
                       pushMessage(binding, t, bytes)
plugin ──────────────► DataSourceRuntimeHost ──► parser ──► datastore      (the live path)
                               │
                               └── RecordTap ──► Recorder ──► McapRecordingWriter ──► *.mcap
                                   (per source)  byte-budgeted queue,  1 MiB chunks, large messages
                                                 writer thread         isolated in their own chunk
```

### Components

| Component | Module | Role |
|---|---|---|
| `RecordTap` | `pj_runtime` | One callback on the push path. Non-owning views, call-lifetime. A relaxed atomic makes the no-tap case free. |
| `Recorder` | `pj_runtime` | Per-source queue + writer thread. Overflow policy is a mode: drop-largest for live streams ("Recording (Record/Stop)"); blocking for captures ("Source cache"). |
| `McapRecordingWriter` | `pj_runtime` | Checked file sink (exclusive create, errno latched, fsync before close), schema dedup, per-channel sequences, `pj.recording` metadata at open and close. |
| `RecordingService` | `pj_runtime` | One Record press = N recorders, one file per source in one batch folder; transactional start, synchronous stop, one result per press. |
| `SourceCacheStore` | `pj_runtime` | Cache root, index (identity → path, size, hash, last hit), cross-process lock, LRU eviction that skips pinned artifacts. |
| `SourceCaptureService` | `pj_runtime` | Decides to capture a descriptor import, runs a lossless `Recorder` into a partial, publishes on clean completion. |
| Shell wiring | `pj_app` | Record/Stop in the streaming strip, Preferences (folders, budgets, toggle), the one ABI vtable entry, the cache-first hook in layout restore. |

### Recording (Record/Stop)

- Record captures **every active delegated-ingest source**; each gets its own MCAP in
  `<recordings dir>/pj_<UTC yyyyMMdd_HHmmss>/<sanitized source>.mcap`. The folder is created atomically; all files of
  one press share a `capture_id`.
- Only **subscribed** topics are recorded (demand-driven subscriptions make a recording "what I was looking at").
- The queue is **byte-budgeted per source** and never blocks ingest: when full, the **largest** queued message is
  dropped (or the incoming one, if nothing queued is bigger; ties go against the newest). The recording continues with
  holes; only a sink error ends it.
- Stop finalizes synchronously (bounded by the queue budget plus one `fsync` per file) and reports one outcome per
  file: `clean`, `lossy` (drops), `truncated` (sink error), `incomplete` (could not finalize; `mcap recover` rebuilds
  the index).
- The file at Stop **is the product**: the live source keeps running; the recording is opened like any MCAP.

### Source cache

- **Host-driven and transparent.** The provider states once, at download start, "this dataset is request X"
  (`attach_source_record(dataset, identity, descriptor_json)`, the one SDK call a provider makes). The host does the
  rest: it captures every descriptor import (behind a Preferences toggle and a budget), publishes the artifact into its
  index on a clean completion, and on layout restore **checks the index before asking the provider anything**.
- A **hit** is a stock `data_load_mcap` load: no network, no trust prompt, and the provider plugin need not be
  installed. A **miss** is the ordinary trust-gated import, silently captured.
- Captures are **lossless**: the push thread blocks at budget instead of dropping (a download is the one case where a
  little speed is worth a guaranteed artifact). `RecorderOptions::OverflowPolicy::kBlock` admits one oversized message
  only when no other message is queued or being copied; the writer's in-flight message is outside the queue budget.
  Cancellation calls `requestStop()` before joining producers, then `stop()` drains and finalizes. Sink failures also
  release waiting producers.
- **Storage and capture are split:** `SourceCacheStore` (leases, pin-then-validate, publish, quarantine, budget) and
  `SourceCaptureService` (the counting tap over a lossless recorder, the publication gate, the `pj.capture` completion
  manifest embedded in the artifact, and cache-first `resolve()`). The SDK's completion contract supplies the signals:
  `attach_source_record` and `complete_ingest` on `DataSourceRuntimeHost`, `discard_parser_ingest` on
  `ToolboxRuntimeHost`.
- **The publication gate.** A structurally valid MCAP footer does not establish completion. Publication requires an
  explicit COMPLETED terminal with declared-topic coverage (zero-message topics only under the empty-topic attestation
  flag), a committed transaction, no cancellation, no latched callback failures, no pure-lazy skips, and a clean
  recorder close.
- **Owner wiring.** `ToolboxRuntimeHost`'s parser-ingest life cycle drives the capture service: every context created
  with a configured `capture_service` and provider manifest id (`ParserIngestDeps`) is armed at creation; RELEASE is
  the commit (the publication gate runs on the release caller's thread, then the store's budget cleanup); DISCARD and
  host teardown abort the capture, and a host-side stop refuses publication. A record-worthy release (attached
  descriptor and a clean COMPLETED terminal, whether or not the capture published) reports through
  `ParserIngestDeps::on_capture_finalized` with the published artifact path; the shell attaches the dataset's
  `SourceRecord` from it and, when published, registers the artifact as the dataset's saveable backing file (source
  path and loadedSources entry), so the layout save emits a `<fileInfo>` + `<materialize>` for the download
  (`wireSourceCapture` in `ToolboxHostWiring.h`).
- **Cache-first restore.** `LayoutImportBatch` consults `SourceCaptureService::resolve()` before any provider contact.
  A hit loads the pinned artifact through the stock ticketed loader; the produced datasets hold the artifact's read pin
  for their lifetime (`SessionManager::pinDatasetResource`, released on dataset removal and merge); a hit whose stock
  load fails is quarantined and re-planned through the live provider.
- **Validation on a hit is cheap** (present, size matches, footer intact). The **index is authoritative**; the
  artifact stays a plain recording.
- **Contended misses.** A miss is contended (`SourceCacheStore::MissKind::kContended`, surfaced through
  `SourceCaptureService::resolve`) only on filesystem evidence of an active publisher: a fresh partial file for the
  identity (mtime within about a minute), which also detects contention on a first publication where no artifact
  exists yet. A held lock or the SDK's retry hint alone never classifies as contended, so an unreadable lock file
  cannot tell the user to wait forever. A contended miss fails that one source with a user-actionable diagnostic
  ("another PlotJuggler instance is currently downloading this request"), with no retry loop and no duplicate
  download; an ordinary absent miss falls back to the provider.
- **Pins are host-internal** (an artifact is never evicted while a dataset reads it). No lease ids cross the ABI, so no
  artifact lifetime depends on a plugin DSO staying loaded.
- **Its own folder and budget,** separate from the recordings folder: recordings are user files and are never evicted;
  the cache is disposable. Preferences expose the cache folder and budget on the Recording page
  (`SourceCacheStore::loadSettings`/`saveSettings`; applied at the next launch) and the "Capture downloads for offline
  restore" toggle (`Settings::capture_enabled` → `SourceCaptureService::setCaptureEnabled`), which gates arming only
  and applies at once: while off, new parser-ingest contexts do not arm a capture, armed captures finish normally, and
  existing cache hits keep resolving.

## Accepted trade-offs

| Choice | Accepted cost |
|---|---|
| **Raw messages only**, never decoded samples. | Sources that write decoded data directly (LSL, dummy) are not recordable until they speak messages. |
| **Subscribed topics only.** | No "record all" mode; stated in the UI. |
| **One file per source**, grouped per press. | N writer threads and N queue budgets per press; `mcap merge` when one shareable file is wanted. |
| **Streams drop-largest, never block.** | A slow disk produces holes in the biggest topics; users are told, not slowed. |
| **Captures block, never drop.** | A slow disk slows a download; in exchange the artifact is always complete. |
| **File at Stop; no live-to-file switch.** | Plots stay bound to the live source; opening the recording is a separate, ordinary action. |
| **Parser policy in the layout, not in the file.** | A recording opened directly uses the loader dialog's (remembered) settings; the layout persists the choice. |
| **Provenance minimal; index authoritative.** | Losing the cache index costs a re-download, not data. |
| **Host-driven cache, one plugin call.** | Providers cannot customize what is cached; the toggle is the only user control. |
| **Capture all imports by default.** | Disk churn from one-off explorations, bounded by budget, LRU and the capture toggle. |
| **Contended miss fails, never re-downloads.** | Two instances restoring the same layout concurrently: the second fails that source with "wait and reload" instead of downloading the same bytes twice or spinning on a retry loop. |
| **Cheap hit validation, heal on failure.** | A same-size corrupt file reaches the loader before being evicted. |
| **Recordings never evicted; cache LRU-evicted.** | Two folders and two budgets to explain. |

## Known gaps

- **Recordable = speaks messages.** A direct-write connector must split transport from parsing (for example Arrow IPC
  messages decoded by an installed parser) to become recordable and cacheable.
- **Pure-lazy pushes leave a hole.** A file-backed source that pushes an object lazily (bytes fetched later) is
  counted, not recorded.
- **Peak memory during a capture is unchanged.** Objects are eager until the artifact is published. Serving lazy
  fetches from the file being written would remove that, and would bound a live stream's object history by disk
  instead of RAM; it is not built.
- **Prefixes are not recorded.** The host applies source prefixes below the seam; a replayed file gets the file
  source's own naming.
- **A stalled download looks absent.** A live download whose partial goes quiet past the freshness window classifies
  as an absent miss and falls back to the provider.
- **Secrets.** The recorder stores bytes verbatim; a connector must never push authentication material as a message
  (a rule for plugin authors).

## Where to look

- On-disk contract (`pj.recording` and `pj.capture` metadata names, versions, terminal causes):
  `pj_runtime/include/pj_runtime/RecordingFormat.h`.
- Code: `pj_runtime/include/pj_runtime/{RecordTap,Recorder,McapRecordingWriter,RecordingService}.h`,
  `SourceCacheStore.h`, `SourceCaptureService.h`; shell wiring in `pj_app/src/MainWindow.cpp`,
  `pj_app/src/StreamingSourceManager.cpp` and `pj_app/src/LayoutImportBatch.h`.
- Request identity and descriptor types: SDK `pj_base/sdk/source/`.

## Evidence

- **Plugin-side caching was tried and dropped** (a capture tee, a plugin-specific file format, a loader plugin,
  DSO-lifetime leases, a budget): every connector would re-implement format, loader, eviction and locking; the leases'
  lifetime was tied to the plugin DSO rather than to the datasets reading the files; and a proprietary format would
  have to stay replayable forever. Kept from it: the typed request descriptor and identity scheme (pinned by a
  byte-for-byte vectors file), the trust allowlist, the credential-origin guard (including the grpc/grpc+tls
  storage-key aliasing rule that prevents a credential downgrade), the presentation hygiene for layout-supplied text,
  the headless import-job shape, and its test corpus (descriptor vectors, credential matrix, a three-leg end-to-end
  test whose warm leg is zero-network by construction).
- **A provider-driven cache ABI** (lookup / adopt / attach / complete plus lease ids) was drafted: five calls for every
  provider to get right, and a hit still required the provider to run. One host-driven call covers it.
- **Tapping the datastore write bridge** (decoded samples) contradicts raw-only, needs a PlotJuggler-native schema,
  and a replay could never re-parse.
- **Parser configuration inside the MCAP** was rejected as mixing data with interpretation: two people may read one
  recording with different policies, and a policy change must not rewrite a multi-GB file.
- **Promote-at-stop** (switching the live source onto its recording) was dropped: a one-moment convenience costing a
  new affordance, a clean/lossy policy and a stop-then-replace sequence with its own failure modes.
- **Promotion after cache publish** (objects go lazy) is a real memory win but costs a second parse of every download;
  it is the known remedy if capture RAM becomes the problem.
- **`.partial` + rename + directory fsync** for recordings was not adopted: MCAP is recoverable by design (`mcap
  recover`).
