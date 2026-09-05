# Session Recorder — host-side raw capture of ingested messages

**Status:** M1 implemented on branch `feat/session-recorder-m1` (pj_runtime core, app wiring; nothing in pj-official-plugins — replay is the stock `data_load_mcap`, D9); final review passed 2026-08-31; PR #616 open. Decisions approved 2026-08-31, per-source capture layout (D5/D6) approved the same day; promote-at-stop (M2) dropped 2026-09-01 (D7).
**Durable spec:** [`RECORDING_AND_CACHING.md`](./RECORDING_AND_CACHING.md) — intent, architecture, trade-offs and rejected alternatives for recording *and* caching; this document is the working design and decision history behind it.
**Scope:** `pj_runtime` (recorder service + ingest tap), `pj_app` (Record UI, Preferences); `data_load_mcap` is untouched (D9); later milestones touch the SDK.

## 1. Problem

Two needs share one mechanism:

1. **Record what I'm streaming.** A live source (ROS 2, Foxglove bridge, MQTT, UDP, ZMQ, pj_bridge, …) is
   eager data bounded by the memory retention buffer. There is no way to keep a session as a file, reopen it
   later, or hand it to a colleague. PlotJuggler 3 never had this; it is the most requested "parity-plus"
   capability around live data.
2. **Cache a bounded download so a layout can re-import it.** Cloud connectors (Mosaico, mcap_cloud, future
   ones) download a reproducible request. A saved layout should restore that data instantly from a local
   artifact, or re-download it headlessly elsewhere. Today each connector implements its own capture, artifact
   format, loader plugin, cache, and lease bookkeeping (pj-official-plugins #275) — duplicated per plugin.

Both are "keep the bytes that entered PlotJuggler, and replay them later." The host is the only place that sees
every plugin's ingest, so the host owns the recorder; plugins own only what is genuinely theirs (transport, the
request descriptor, trust, credentials).

## 2. Decisions (2026-08-31)

| # | Decision | Rationale |
|---|---|---|
| D1 | **Raw only.** The recorder captures messages at the delegated-ingest seam (encoding + schema + raw bytes), never decoded samples. | Replay re-parses with current parsers, so a parser fix applies to old recordings; the file is a standard MCAP. Sources that write decoded samples directly (LSL, dummy, Mosaico until split into transport + parser) are not recordable until they expose messages. |
| D2 | **Stream recording first** (M1), the source cache on top of it later (M3). | M1 has no identity, trust, leases or budget in the loop: the recorder core is proven on the simpler product. |
| D3 | **Subscribed topics only.** Recording captures the topics the session is subscribed to; no "record all" mode in v1. | Demand-driven per-topic subscriptions mean a recording is "what I was looking at". Stated plainly in the UI. |
| D4 | **Overflow drops, never blocks** (revised 2026-08-31). `pushMessage` never waits for the recorder. When the write queue is over its byte budget the **largest message loses**: the biggest queued message is dropped, or the incoming one is discarded when nothing queued is bigger. The recording carries on with holes until the user stops it, and a slow writer never ends it — truncation is reserved for real sink errors. | Ingest speed is never the price of recording; a hole in one channel beats losing the rest of the session, and the big topics that caused the backlog pay for it before the scalars. |
| D5 | **One file per source** (revised 2026-08-31). Record/Stop in the streaming strip records every active streaming source, each into its own MCAP file inside one batch folder. "Record this source" is the same mechanism over a subset of the sources, and needs no format change. | A combined file needs a source dimension MCAP has no place for, and un-mixing it afterwards is lossy: two sources may name the same topic, and a truncation or a disconnect would then damage every source's data at once. Per-source files replay as ordinary bags, one source's failure stays that source's, and `mcap merge` produces a single shareable file when that is what the user wants. Accepted cost: N writer threads and N times the per-source queue budget. |
| D6 | **Recordings folder, auto-named** (revised 2026-08-31). Start creates the batch folder `<recordings dir>/pj_<UTC timestamp>/` and writes `<sanitized source name>.mcap` into it, one per recorded source (§4.4). The folder is created atomically and takes `_2`, `_3`… when that second is already claimed; every file of one press carries the same `capture_id`, so a batch survives being moved or renamed. The recordings folder is a Preference (default under the app data dir); Stop reports what was written (Open-folder / Rename are **M1: deferred, see §7.1**). Nothing is ever evicted. | Zero friction to start; recordings are user-owned files, not cache. The folder is what keeps one press together once the files are separate. |
| D7 | **File only at Stop.** The live source stays live; the user opens the recording like any MCAP, in this session or a later one. A "switch the live source to its recording" step (promote-at-stop) was planned as M2 and **dropped on 2026-09-01**. | The file is the product: everything the recorder promises is delivered when Stop returns. The switch bought only convenience (plots stay bound without reopening the file) at the price of a new affordance, a clean/lossy/incomplete policy, a stop-then-replace sequence with its own failure modes, and a live→file parser-config handoff. |
| D8 | pj-official-plugins #275 (plugin-side capture) is **held** until this mechanism exists; a thinner Mosaico PR follows M3/M4. | No throwaway artifact format ships to users. |
| D9 | **Parser policy lives in the layout, not in the file** (decided 2026-08-31). The recording carries decoding facts (encoding, schema, type name) and provenance (the source identity in `pj.recording`); the live session's `parser_config` (timestamp field, array limits, label-keyed arrays) is *interpretation* and is persisted the way every source configuration is — in the layout, once the recording is opened as a file source and the layout is saved. | Data vs. interpretation: two people may open one recording with different policies, and a policy change must not rewrite a multi-GB file. Avoids a second home for the same setting (and the loader-side precedence rules that came with it — pj-official-plugins #280, closed). |

## 3. Where the bytes enter: the delegated-ingest seam

`pj_runtime`'s `DataSourceRuntimeHost` implements the SDK's delegated-ingest C ABI for one source:

- `cbEnsureParserBinding(topic, encoding, type_name, schema_bytes, parser_config)` → a `ParserBinding` whose
  `Signature{encoding, type_name, schema_bytes, parser_config}` identifies the request (an identical signature
  hands back the existing binding; any difference mints a new one).
- `cbPushMessage(binding, log_time, bytes)` → the bound parser writes decoded samples/objects through the
  binding's write hosts into the datastore.

Every streaming plugin that speaks messages goes through this seam (mqtt, udp, zmq, ros2, foxglove_bridge,
pj_bridge; mcap_cloud for progressive display). The seam already **is** the MCAP data model:

| Delegated ingest | MCAP |
|---|---|
| `ensureParserBinding` signature | `Schema{name=type_name, encoding, data=schema_bytes}` + `Channel{topic, message_encoding=encoding, metadata}` |
| `pushMessage(binding, log_time, bytes)` | `Message{channel, log_time, publish_time=log_time, data=bytes}` |
| source identity | the file itself: one recording holds one source, named once in `pj.recording` — `parser_config` is NOT recorded (D9) |

So the recorder is a mirror of the seam into an MCAP writer, and **replay is the existing `data_load_mcap`
loader plus the installed parsers** — indistinguishable from opening a rosbag. M1 needs **no SDK change**: the
tap is host-internal.

### 3.1 Why not the datastore write bridge

The nine write-bridge calls (`sourceEnsureTopic/Field`, `sourceAppendRecord/BoundRecord/ArrowStream`,
`sourceObject*`) see *decoded* data. Tapping there would record every plugin, including direct writers, but
(a) it contradicts D1, (b) it needs a PJ-native file schema PlotJuggler must keep replayable forever, and
(c) replay could never re-parse. Rejected.

## 4. Architecture

```
pj_app                                   pj_runtime                                  plugins
─────────────────────                    ────────────────────────────────────        ──────────────────────
Record / Stop button  ── start/stop ──▶  RecordingService                            data_stream_* (delegated)
status (elapsed, MB,                       │ one capture = N Recorders,               │ ensureParserBinding
 sources, dropped)      ◀── signals ──     │ Preferences, finalization executor       │ pushMessage
Preferences page                           ▼                                          ▼
 (folder, queue budget)                  Recorder (one per source) ◀─ RecordTap ─ DataSourceRuntimeHost
                                           │ byte-budgeted queue, writer thread                (per source)
                                           ▼
                                         McapRecordingWriter ── <recordings dir>/pj_<ts>/<source>.mcap
                                                                            │
                                                              replay: data_load_mcap + parsers
```

### 4.1 `RecordTap` (pj_runtime, internal)

`DataSourceRuntimeHost` gains an optional observer set by `StreamingSourceManager` (or whichever owner holds the
source): `setRecordTap(std::shared_ptr<RecordTap>)`. The tap has one call, invoked on the plugin's push thread,
inline:

- `onMessage(uint32_t binding_id, const RecordedBindingView&, int64_t log_time_ns, Span<const uint8_t>)` — once
  per `pushMessage`, **before** the parser runs, returning `TapVerdict{kContinue | kStopRecording}`. The view is
  non-owning and valid only for the call: it carries the binding's verbatim signature (topic, encoding, type
  name, schema bytes), which the tap copies into a `RecordedBinding` when it opens a channel.

**Bindings travel with messages.** A recording starts long after a live source subscribed its topics, so a
separate announcement would have to be replayed for every already-bound topic or leave it silently unrecorded.
Carrying the binding on the message removes the question: each recording gets a fresh tap whose binding→channel
map is empty, the first message on a binding opens that channel from the view it carries, and an unknown
binding is not a state the tap can be in. Consequences: a binding that never pushes needs no channel and gets
none, and a second recording over the same live source opens its own channels.

`parser_bindings_` is unsynchronized by design (one push thread per source), which is exactly why the tap is
called there — no GUI-thread walk of that map. The tap must not introduce a second thread on that path: it
enqueues (or drops, see §5) and returns without waiting for a live recording. Explicit download captures may
wait for queue space instead. Their owner calls `requestStop()` before joining a cancelled producer; detaching
a tap alone does not unblock an in-flight call. Taps **co-own** the recorder (`shared_ptr`, handed out by `Recorder::tapFor`), so a push thread
already inside `onMessage` can never meet a destroyed recorder; the host holds only the tap, drops it on
`kStopRecording` or on a throw, and releases it outside its own lock so a last-reference `~Recorder` never
joins the writer thread with push threads queued behind it.

### 4.2 `Recorder` (pj_runtime)

One recorder per recorded source (D5); a capture of N sources runs N of them, independently. Owns:

- a **bounded queue** of `{channel_id, log_time, bytes}` (byte-budgeted, default 64 MiB) fed by the tap from
  N push threads, with drop-largest as the live default (§5) and opt-in `kBlock` for lossless downloads;
- one **writer thread** draining into `McapRecordingWriter` (chunked, zstd, chunk index written per chunk);
- the channel table: `binding_id → mcap channel id`, opened by a binding's first message — both for
  a late subscription and for a binding that predates the recording, since neither is distinguishable from the
  tap's side.

Lifecycle: `start()` → `Running` → `stop(terminal_cause)` (drain, write summary/footer) → `Finished`; or
`Truncated{reason}` on a sink error (§5). The cause travels **through** `stop()` into the summary the sink
writes, so the file itself says whether the user ended it (`stopped`), its source went away (`source_ended`),
a failure cut the data short (`truncated`, which always overrides the requested cause) or the app quit
(`shutdown`).

`requestStop()` ends admission and wakes blocked producers without waiting on the sink. `stop()` remains
mandatory to drain and close. Blocking mode admits a message larger than its budget when the queue and copy
reservations are empty, then holds subsequent producers until capacity returns; one in-flight sink write is
outside this queue budget. Only successful source completion plus zero drops/skips and a clean final summary
permits future cache publication. Blocking mode does not itself implement the capture service or prove coverage.

### 4.3 `McapRecordingWriter` (pj_runtime)

Thin wrapper over `mcap::McapWriter` (`mcap/2.1.1`, already a Conan dependency; **lz4/zstd must be direct
requires** of `pj_runtime` — Conan hides transitive headers). File-level `Metadata` record `pj.recording`:

```json
{ "version": 1, "app_version": "…", "capture_id": "…", "source_display_name": "…",
  "source_plugin_id": "…", "capture_ordinal": "1", "started_utc": "…",
  "stopped_utc": "…", "terminal_cause": "stopped|source_ended|truncated|shutdown",
  "truncated": false, "truncated_reason": "", "messages": "…", "payload_bytes": "…",
  "dropped_messages": "…" }
```

`version` stays **1**: no released build has ever shipped a recording, so the per-source layout is the first
published shape of the format, not a break of one (PR artifacts are not a format contract). `capture_id` is a
UUID shared by every file of one Record press, and `capture_ordinal` is that file's 1-based position in the
press, so a batch is reconstructible after the folder is renamed or the files are moved apart.

Per-channel `metadata` is **empty**: one file holds one source, so the identity is a file-level fact rather
than something repeated on every channel, and no parser policy is recorded either (D9). The type name is not
repeated there — the channel's schema record already carries it as its name — and schemas carry
`type_name`/`encoding`/`schema_bytes` verbatim. Nothing PJ-specific is required to *read* the file: the `mcap`
CLI and any MCAP tool see ordinary channels.

MCAP schema and channel ids are `uint16_t`, so `addChannel` refuses past 65535 of either and the recording
truncates with that reason rather than wrapping ids onto live records. `close()` fsyncs before it fcloses:
an ok Status is the service's licence to publish the file under its final name.

### 4.4 `RecordingService` (pj_runtime service, Qt object, no Widgets)

Owns **at most one** active capture — one Record press, N recorded sources, N `Recorder`s — and is the only
seam `pj_app` drives. Built on every platform (Qt Core + `<filesystem>`, no mcap); only the **sink** it defaults
to is platform-bound, so recording is a **runtime capability**: `RecordingService::isSupported()` answers "does
this build have a sink", and that is the single query the shell gates its Record UI and its Preferences page on.
`start()` on a build without one fails with `recording is not available in this build` before it touches the
filesystem — unless a test injected its own sink through `setSinkFactoryForTest`, which is exactly what makes
the service testable where no sink is compiled in.

```cpp
Expected<QString> start(std::vector<RecordingTarget>, const std::string& app_version);  // returns the folder
void stop();                                        // idempotent, synchronous
void onSourceEnded(quint64 target_key, const QString& reason);
bool isRecording() const;
signals:
  void started(QString capture_id, QString directory, QVector<PJ::SourceRecordingStart> sources);
  void stopped(PJ::CaptureResult result);
  void progress(quint64 messages, quint64 payload_bytes, quint64 dropped);
```

- **Targets, not hosts.** A `RecordingTarget` describes one source by an opaque `target_key` (pj_app passes the
  `DatasetId`) plus callbacks — `attach(tap)`, which returns false when the source has vanished and where a null
  tap detaches, and the optional `skipped_lazy()` — supplied by `pj_app`'s streaming manager. The service never
  learns about `DataSourceRuntimeHost`, and eligibility (D1: a direct writer has no delegated ingest to tap) is
  decided by the caller, which is what knows — `pj_app`'s streaming manager filters on the
  `kCapabilityDelegatedIngest` bit and hands back the excluded display names beside the targets, so the shell can
  say what will be missing. Targets are sorted by `target_key`, which fixes the file names, the
  `capture_ordinal`s and every reported order.
- **Settings.** `QSettings` keys `Preferences::recording_directory` (empty = `<AppLocalDataLocation>/recordings`)
  and `Preferences::recording_queue_budget_mib` (64), through `loadSettings()`/`saveSettings()`/`setSettings()`.
  A change takes effect on the next `start()`; a running capture keeps the options it began with. The budget is
  **per source**: recording N sources may hold N × it in flight. That is the accepted cost of per-source files
  (User decision) — there is no service-wide cap and no division, and the Preferences note says so.
- **Naming (D6).** `<recordings dir>/pj_<UTC yyyyMMdd_HHmmss>/` is created with a plain `mkdir` on the exact
  name, which fails if it exists, so creating the folder *is* claiming it (`_2`, `_3`, … then, 100 attempts).
  Inside it each source gets `<sanitized display name>.mcap`: NFC-normalised, path separators and the
  Windows-reserved characters mapped to `_`, `_`/whitespace runs collapsed, trailing dots and spaces trimmed,
  cut to 80 characters, `source` when nothing is left. Names that reduce to the same one — `a/b` vs `a:b`, or a
  case-only difference — take `_2`, `_3`, … compared case-folded, and the writer's exclusive create is the last
  guard behind that. Files are written straight into their final names, the way `rosbag2` and `mcap record`
  write theirs. What a returned `stop()` guarantees is durability, not a rename: each sink fsyncs before it
  closes.
- **Finalization is synchronous.** `stop()`, a source ending, a poll-detected truncation and the destructor all
  detach the tap, freeze the source's `skipped_lazy` delta, and then drain and close that recorder on the GUI
  thread. Each recorder is stopped exactly once (its result's presence is the guard), results are merged in
  started order, and exactly one `stopped()` is emitted per press. The wait is bounded by what is queued — at
  most the per-source budget times the number of sources still recording, over the sinks' write throughput —
  plus one `fsync` per file, since each sink syncs before it closes and the files close serially on the GUI
  thread; drop-largest keeps the queued half small in practice. Moving finalization onto a worker is a
  deliberate deferral (§7.1).
- **Per-source termination.** `onSourceEnded(target_key, reason)` finalizes ONE source with the cause
  `source_ended` and leaves the rest of the capture recording; the batch ends when its last source does. The
  reason is preserved into that source's result, and a non-empty one makes its outcome non-clean, so an orderly
  end passes an empty reason.
- **Poll.** A 500 ms timer emits `progress()` from every live `Recorder::stats()` plus the frozen totals of the
  sources already finalized, so the totals never decrease as recorders retire. Because the `Recorder` contract
  deliberately has no truncation callback (§5), the same tick is what notices `kTruncated` and finalizes that
  one source. Both the stats and the truncated set are read *before* the emit and re-resolved after it: a
  `progress` slot may call `stop()` synchronously. Per-source events do not reach the signals — the capture
  totals do — so a source that truncates mid-batch is reported with a `qCWarning` until there is a per-source UI
  to carry it.
- **What `stopped()` means.** `CaptureResult` carries the `capture_id`, the folder, and one
  `SourceRecordingResult` per source in started order: its path, its `terminal_cause`, its counters, and one
  **exclusive** outcome bucket evaluated in this order — `incomplete` (the sink could not finalize it: no
  footer, or bytes that are not durable, so it opens only by a linear scan and `mcap recover` rebuilds its
  index) → `truncated` (a failure ended the data, but the file closed) → `lossy` (a complete file with holes:
  drops, skips, or a source that ended with a reason) → `clean`. `reason` is non-empty exactly when the outcome
  is not clean. The per-bucket counts sum to the source count, so a caller tests
  `clean_count == sources.size()`, never a boolean. The two loss counts stay different holes:
  `skipped_messages` is that target's `skipped_lazy()` DELTA between `start()` and its finish (§9, pure-lazy
  pushes that never reached the recorder; those counters are per-host and never reset), while
  `dropped_messages` is what the byte budget dropped once they had (§5).
- **Transactional start.** Every folder, file and recorder is created before any tap is attached. Any failure —
  a sink that cannot open, an attach that returns false or throws — detaches every target it touched (including
  the failing one, which may have installed its tap before failing), closes and deletes every file it created
  by path, removes the folder if it empties, and returns an error naming the source that failed; nothing it
  could not remove is left unmentioned in the log. No `started()` is emitted. A half-armed capture would record
  some sources and silently miss others.
- **Test seam.** `setSinkFactoryForTest()` replaces what the next `start()` builds its recorders on: one call
  per recorder, in target order. Production leaves it unset and gets a `McapRecordingWriter` each time.

### 4.5 `pj_app`

- The streaming strip (`comboStreaming`, `buttonStreamingOptions`, `buttonStreamingPause`, `streamingSpinBox`)
  gains `buttonStreamingRecord` (toggle; red dot while recording) and a status label (elapsed, size written,
  and the dropped count once the budget has had to shed anything). Recording a SUBSET of the live sources (the
  dataset context menu) is **M1: deferred, see §7.1**; the mechanism is already the same, only the UI is missing.
- `StreamingSourceManager` keys each target by its `DatasetId` and forwards `streamStopped` to
  `onSourceEnded(dataset_id, reason)`, so one stream disconnecting finalizes only its own file.
- Stop → one toast about the capture: the path when a single source recorded, the folder when several did and
  every one is clean, otherwise the count plus up to two failing sources with their reasons (and `mcap recover`
  named when a file could not be finalized). Per-source detail goes to the log, never into the toast. The
  Open-folder / Rename actions on that toast are **M1: deferred, see §7.1**.
- Preferences → Recording: folder, queue budget. The nav row and the page's wiring are added only
  when `RecordingService::isSupported()`; the page itself always exists in the `.ui`, and a nav row's position
  IS its page index.
- No build flag reaches the shell: the `RecordingService` member, its signal connections and the shutdown `stop()`
  calls are unconditional, so every recording code path reads the same on every platform.

### 4.6 Replay (`data_load_mcap`, plugins repo)

Already binds a parser per channel from the MCAP schema/message encodings and passes a per-channel parser
config from its dialog. **No loader change** (D9): a recording opened on its own replays with the loader's
current parser settings (the loader dialog remembers its last-used parser config), and saving the layout persists
them like any other source configuration. The live session's settings are not carried over automatically (D7).

## 5. Drop policy (D4) — precise contract

`onMessage` enqueues and returns. It never waits for the writer, and it never answers `kStopRecording` because
the queue is full: a drop ends one message, not the recording.

When the incoming message does not fit `queue_budget_bytes` (each message costs its payload plus the item
carrying it), one uniform rule applies to the set {queued messages} ∪ {incoming message}: **discard the largest
member**, and repeat until the incoming message fits or is itself discarded. Concretely — if the incoming
payload is at least as large as the biggest queued one, the incoming message is dropped; otherwise the biggest
queued message is dropped. Ties go against the newest: among equal-size queued candidates the most recently
queued one is discarded, and an incoming message that merely equals the largest queued one is refused — so under
a sustained slow disk the messages already at the front of the backlog are still written and the hole sits at
the live edge until the writer catches up. Channel-adds are never dropped (a handful of strings each, and the
messages that follow reference them). Queued bytes never exceed the budget, so in practice one drop always
suffices; the rule is written as a loop because that is what it says, not because it iterates.

Consequences, all deliberate:

- **A message larger than the whole budget is never recorded.** No queue state can make room for it, so it is
  dropped even against an empty queue. Its only trace is the dropped counters. With the 64 MiB default this
  needs a genuinely enormous message.
- **Drops are holes, never reorderings.** Whatever survives still drains FIFO, so each channel keeps its
  log-time order and a reader sees a gap, not a shuffle.
- **The big topics pay first.** Under a saturated sink one video frame or point cloud is shed before hundreds
  of scalar samples — which is also what keeps the cheap channels continuous.

Accounting is **totals only**, per recorded source, not per topic: `RecordingStats` and `RecordingSummary`
carry `dropped_messages`, each `SourceRecordingResult` carries it and `CaptureResult` sums
them over the capture, `RecordingService::progress` carries the capture total, and each closing `pj.recording`
record carries that file's own count. It stays distinct from `skipped_messages`: skipped never reached the
recorder (pure-lazy pushes, §9), dropped was discarded once it had.

The budget is per source, so the drop policy is per source too: a saturated sink sheds the messages of the
source that saturated it, and the other sources' files are untouched.

**Truncation is reserved for real failures**: a sink `write`/`addChannel`/`close` error, MCAP id exhaustion, or
an exception escaping a tap or the writer thread. The recorder then transitions to `Truncated`, the writer
drains what it already holds and exits, and every tap returns `kStopRecording` and detaches. The writer never
writes the summary — a truncation leaves the sink **open**. The owner notices `Truncated` (it polls `state()`)
and calls `stop()`, which joins the writer and then closes the sink; `close()` is what writes
`pj.recording.truncated=true` with the reason (`disk full`, `write error: …`). The plugin's `pushMessage`
returns normally in every case.

The backlog a `stop()` has to drain is therefore bounded by the queue budget, with nothing to add: no message
larger than it can ever be queued.

## 6. Crash safety

Chunked writing with the chunk index flushed per chunk; a crash leaves a file without a summary section. The
loader must open such a file in linear mode (scan chunks) — verify `data_load_mcap`'s reader path tolerates a
missing summary; if not, that is part of M1. `mcap recover` is the fallback, never the plan.

The writer follows the file discipline proven in `mcap_server`'s `SessionMcapWriter` (the cloud connector's
export tee — same author, different repo, not linked):

- **Checked sink.** mcap's stock `FileWriter` swallows short-write, flush and close failures. The recorder
  writes through its own `mcap::IWritable` that latches the first OS error and keeps `size()` advancing, and
  checks the latch after every write and at close — that is how "disk full" becomes a truncation reason
  instead of a silently corrupt file. The sink opens the `std::filesystem::path` directly (no `string()` on
  Windows) in exclusive-create mode: a pre-existing file or symlink fails the open rather than being
  truncated or followed.
- **Bounded loss window.** Chunk size is explicit (1 MiB; mcap defaults to 768 KiB, `mcap_server` uses 4 MiB for
  offline downloads where crash loss is irrelevant): a crash loses at most the open chunk.
- **Large messages are isolated.** Small messages share those 1 MiB chunks, but a message of 200 KiB or more
  gets a chunk to itself (the writer flushes the open chunk before it and again after it). A lazy reader
  decompresses a whole chunk to reach one message, so co-locating a video frame with hundreds of scalar samples
  makes each pay for the other; isolating it keeps scalar fetches cheap and lets the large message be fetched,
  and compressed, on its own. Messages at or above the chunk size were already isolated by the writer's own
  overflow check — this only brings the same treatment down to a fixed threshold.
- **Written under its final name.** The recording goes straight to `<name>.mcap`, as `rosbag2` and
  `mcap record` write theirs. A recording that was never stopped cleanly therefore differs from a finished one
  only by its missing summary section: readers scan it linearly, `mcap recover` rebuilds its index, and
  nothing has to be renamed for the user to have their file. The sink fsyncs before it closes, so a returned
  `stop()` means the bytes are on stable storage.
- **Metadata placement.** `McapWriter::write(Metadata)` closes the open chunk, so `pj.recording` is written
  only at open and at close — never mid-stream.
- **Per-channel `sequence`** counters, as ordinary MCAP tooling expects.

## 7. Milestones

| Milestone | Contents | Depends on |
|---|---|---|
| **M1 — Stream recording** | `RecordTap`, `Recorder`, `McapRecordingWriter`, `RecordingService`, streaming-strip Record/Stop + status, Preferences, crash-tolerant open, tests (§8); no loader change (D9). | nothing new in the SDK |
| **M3 — Source cache** (decisions 2026-09-01, see `RECORDING_AND_CACHING.md` §3.4) | Host-driven and transparent: the provider states a dataset's request identity once (`attach_source_record`, the single SDK addition); the host captures every descriptor import losslessly behind a Preferences toggle + budget, publishes clean artifacts into its index, and serves layout restores cache-first through the stock loader. Own folder, LRU eviction with host-internal pins, cheap validation with heal-on-failure. | M1; SDK minor |
| **M4a — `parser_arrow`** (in progress, 2026-08-31) | New MessageParser plugin in pj-official-plugins for encoding `arrow-ipc` (one IPC stream per message → `ParserWriteHostView::appendArrowStream`), built on nanoarrow + nanoarrow_ipc (zstd; no libarrow). Independent of #275: it copies the decode pipeline from Mosaico's `arrow_ingest` (timestamp detection, struct flatten, view normalization); canonical-object ontologies come later. Plan: pj-official-plugins `.worktrees/parser-arrow`, `docs/plans/2026-08-31-parser-arrow.md`. | none |
| **M4b — Mosaico transport** | Mosaico becomes transport only (Flight batches → `arrow-ipc` messages via `ensureParserBinding`/`pushMessage`); its capture/format/loader retire; `.pjmosaico` disappears. Sequenced after the #275 decision (stack on it, or trim #275 to its surviving pieces first) because both rewrite `fetch_worker`/`mosaico_dialog`. | M4a, M3 |

### 7.1 M1 follow-ups (deferred at the review gate, 2026-08-31)

Deliberately left out of M1 so the milestone stays reviewable; each needs a product decision or a cross-repo harness:

- **Recording a subset (D5).** Every recorded source already gets its own file; what is missing is the UI to record
  fewer than all of them ("Record this source" in the dataset context menu). The service takes an arbitrary target
  list already, so this is a UI increment, not a mechanism change.
- **Asynchronous finalization.** `stop()` drains and closes on the GUI thread (§4.4). The bound is the queued
  backlog, which drop-largest keeps small, plus one serial `fsync` per file at close, so moving it to a worker
  was judged not worth the machinery until a real capture shows the stall.
- **Combining a capture.** N files are N files: `mcap merge` produces one shareable artifact, and reopening a whole
  capture by hand is N loader dialogs today. A capture-aware open (one dialog for the folder) is a future UX
  item.
- **Stop-toast affordances (D6/D7).** The stop toast is text only: no Open-folder link, no Rename. The toast
  infrastructure supports rich-text links; Rename needs a small completion surface.
- **Replay equality test.** `recording_pipeline_test` proves host → MCAP → reader; replaying a recording through
  `data_load_mcap` into a datastore and comparing decoded values is a cross-repo test (plugins repo) still to write.
- **Throttled-writer stress test** (10k messages against a slowed sink through a real host) — belongs in a bench harness,
  not `ctest`.
- **`RecordingStats::payload_bytes` is payload bytes**, not file bytes (renamed for that reason). The status label
  still shows it as if it were the size on disk; exposing the real file size is the remaining half.
- **Record-aware fetch for pure-lazy topics** (§9): only matters once file-backed/cached sources are recordable (M3).
- **Parser policy is re-entered once.** A recording opened directly replays with the loader dialog's settings (the
  dialog remembers its last-used parser config); the live session's policy is not carried over automatically, since
  promote-at-stop was dropped (D7). Saving the layout afterwards persists the chosen policy (D9).

#### Browser (wasm) recording — later milestone

`Recorder` and `RecordingService` already build for wasm; `McapRecordingWriter` does not (PjWasmDependencies
carries no mcap package) and a browser has no recordings folder. So the browser is missing exactly two things: a
`RecordingSink` and an export affordance — no shell change, since `isSupported()` flips on its own once a sink is
compiled in. The sink options, worst to best:

- **MEMFS + download at Stop.** Simplest: record into Emscripten's in-memory FS and hand the bytes to the user as
  a download when the recording ends. Bounded by the tab's heap, and everything is lost on a tab crash — the
  opposite of the crash tolerance M1 is built around.
- **OPFS via WasmFS (recommended).** Origin Private File System: persistent, streamable, writes land on storage as
  they are produced, so the crash story survives essentially unchanged. Export is a read back out of OPFS into
  a download.
- **File System Access API.** A real user-chosen file handle, but Chromium-only, and it inherits the COOP/COEP
  friction already seen with `showOpenFilePicker` under the cross-origin-isolated headers Qt's threaded wasm build
  requires.

An MCAP writer for that sink is a separate question from the sink itself: either add mcap to the wasm dependency
set, or write a minimal MCAP encoder against the `RecordingSink` interface.

## 8. Testing (M1)

Shipped, five `ctest` binaries in `pj_runtime/tests/`:

| Binary | Covers |
|---|---|
| `recorder_test` | The queue/drop contract against a fake sink: ordering, channels opened by their first message, the largest-message-loses rule in each of its shapes (drop the biggest queued one, discard an even bigger newcomer, never record one bigger than the whole budget) and that it picks by size and not by age, that a push into a full queue never waits for the writer, truncation from a sink error / a sink that throws, `stop()` idempotence, drop accounting under concurrent producers, taps outliving the owner. |
| `mcap_recording_writer_test` | What the writer produces, read back with `mcap::McapReader`: channels (whose metadata is empty), schema dedup, both `pj.recording` records including `capture_id`/`capture_ordinal`/`terminal_cause`/`payload_bytes`, exclusive create, instance reuse, the 16-bit id guard. |
| `recording_service_test` | The capture layout and its lifecycle: the batch folder and one file per source in sorted-key order, the sanitization table and the ordinals that separate colliding names, the transactional rollback in each of its failure shapes (a failed attach, a throwing attach, a recorder that cannot open), duplicate keys and empty batches refused, one source truncating while the batch continues, ended sources plus a global Stop producing exactly one result each and one `stopped`, aggregates that never decrease, a zero-message file kept clean, the destructor finalizing durably, the settings round-trip and clamping, and the incomplete outcome an unclosable sink causes. |
| `data_source_runtime_host_record_tap_test` | The host end of the seam: the binding view carried on every message (bindings minted before the tap included), `kStopRecording`, a throwing tap, pure-lazy accounting. |
| `recording_pipeline_test` | The assembled pipeline — real hosts + real parser plugin → tap → recorder → MCAP, driven by `RecordingService` and read back: recording started mid-stream, no pre-recording leakage, two live sources landing in two files of one capture (same `capture_id`, ordinals 1 and 2, each holding only its own topics), a second recording starting clean, ingest unaffected. |

Deferred (see §7.1): replay through `data_load_mcap` with value equality; the 10k msg/s throttled-writer
stress run; the kill-the-writer crash-file case. Ineligibility (D1) is enforced in
`StreamingSourceManager::recordingTargets()` and surfaced by the shell, with no dedicated test — the check is a
capability bit on a live plugin handle, which no unit fixture can cheaply build.

## 9. Risks and open points

- **Parser drift.** Replay parses with today's parsers; behaviour changes (array policy defaults, timestamp
  detection) alter old recordings. Mitigation (later): record parser plugin id/version in `pj.recording`; the loader
  warns on mismatch.
- **External parser context.** Anything a parser needs that is not in the message or the binding
  (registries fetched by the plugin) breaks replay; the binding signature is the contract — plugins must put
  such context in `schema_bytes` (parser policy itself is layout state, D9).
- **Secrets in data channels.** The recorder stores bytes verbatim; a connector must never push auth
  material as a message (rule, documented in the plugin guides).
- **Prefixes.** Channel topics are recorded as the plugin named them, and nothing records a source prefix: the
  host applies prefixes below the ingest seam, so the recorder never sees one. Replay as a file source gets the
  file source's own naming — acceptable for v1, revisit if it proves confusing.
- **Direct-write sources** stay unrecordable (D1) — Mosaico's split (M4) is the notable case. `parser_arrow` (M4a) is the
  independent half and is being built now; `data_load_parquet` is deliberately NOT routed through it (a file loader re-encoding
  Parquet to IPC only to decode it again gains nothing) — its win is switching to `appendArrowStream` per row group, and a
  host-side `arrow_import` that accepts view types and nested structs natively, which shrinks `parser_arrow`'s shaper too.
- **Pure-lazy topics record a hole.** A `kPureLazy` push fetches no bytes on the push thread, so the tap counts
  it (`DataSourceRuntimeHost::recordTapSkippedLazy()`) instead of recording it. Native streaming has no
  pure-lazy types today; file-backed and cached sources (M3) do (markers, annotations, video). Remedy when
  needed: fetch once on the push thread for the tap only (under `lazy_fetch_mutex_`, released before the tap
  call, the buffer anchored for the duration of that call) and push the object lazily, unchanged. Until then
  the owner must surface a non-zero skipped-lazy count as an incomplete recording.
- **Size.** Raw recording stores the full stream; no eviction (D6). A folder-usage readout on the Preferences page is a follow-up, not part of M1.
- **What M3 may cache.** A `lossy` or `truncated` file is real data with holes, and an `incomplete` one has no index
  until `mcap recover` runs; both open fine by hand. M3's cache path accepts only a `clean` artifact and rejects
  anything else, since a cache entry that silently misses messages would be indistinguishable from a complete one on
  the next replay.
- **A capture is a folder, not a file.** Handing one to a colleague means the folder, or one `mcap merge` away.
  Reopening a whole capture by hand is one loader dialog per file today; a capture-aware open is a future UX item
  (§7.1).

## 10. Relationship to pj-official-plugins #275

#275 implements the cache product plugin-side (capture tee, `.pjmosaico` format, loader plugin, DSO-lifetime
leases, budget). It is held (D8). What survives into M3/M4: the typed `SourceDescriptor` and its vectors, the
trust allowlist, the credential origin guard, the provider job on `ProviderJob`, presentation. What retires:
`ArtifactCapture`, `arrow_cache_artifact`, `artifact_replay`, `data_load_mosaico_cache`, the lease registry
and settlement gate, the plugin-side cache policy. The SDK 0.24 `descriptor_import_support` component stays:
the host reuses `RequestArtifactCache` for M3, and providers keep `Origin`/`SourceDescriptorPolicy`/`ProviderJob`.
