# Recording and caching — technical specification

**What this is.** The durable reference for how PlotJuggler 4 keeps the bytes that entered it and replays them
later: the **session recorder** (Record/Stop on live sources) and the **source cache** (reusable artifacts for
cloud-connector downloads). It records intent, goals, the architecture, the accepted trade-offs and the
alternatives that were rejected. It is not an implementation plan; the working design with its milestone history
is [`session_recorder_design.md`](./session_recorder_design.md).

**Status.** Recorder: implemented (PJ4 #616). Source cache: designed, decisions taken 2026-09-01, not started.

## 1. Intent

Two user needs share one mechanism:

1. **Keep what I am streaming.** A live source (ROS 2, Foxglove bridge, MQTT, UDP, ZMQ, pj_bridge, …) is
   memory-bounded eager data. Users want to keep a session as a file, reopen it later, hand it to a colleague.
2. **Do not download the same thing twice.** Cloud connectors (Mosaico, mcap_cloud, future ones) fetch a
   reproducible request. A saved layout should restore it offline, instantly, from a local artifact.

Both reduce to *"store the messages that entered PlotJuggler, replay them with the same machinery that opens a
bag."* The host is the only component that sees every plugin's ingest, so the host owns the mechanism; plugins
own only what is genuinely theirs (transport, request identity, trust, credentials).

## 2. Goals and non-goals

**Goals**

- A recording or cache artifact is a **standard MCAP file** that any MCAP tool, and PlotJuggler's stock loader,
  opens. No PlotJuggler-native format, ever.
- **Zero plugin changes for recording**; **one plugin call for caching**. Every delegated-ingest source is
  recordable and cacheable without knowing it.
- Recording never slows ingest. Caching never slows a hit.
- Failure containment: a slow disk, a full budget, a crash mid-write never damages the live session or another
  source's file.

**Non-goals**

- Recording decoded samples or PlotJuggler-internal state (transforms, plots) — that is the layout's job.
- A "record everything on the wire" mode: recordings contain what the session subscribed to.
- Format-level fidelity for parser *policy* (timestamp field, array limits) — policy is interpretation and lives
  in the layout (§4, D9).
- Browser (WebAssembly) recording. The core compiles there; only a sink is missing.

## 3. Architecture

### 3.1 The seam

`pj_runtime`'s `DataSourceRuntimeHost` implements the SDK's **delegated-ingest** ABI for one source:
`ensureParserBinding(topic, encoding, type_name, schema_bytes, parser_config)` then
`pushMessage(binding, log_time, bytes)`. That seam *is* the MCAP data model — binding ↔ `Schema` + `Channel`,
push ↔ `Message`. The recorder is a mirror of the seam into an MCAP writer, and **replay is `data_load_mcap` plus
the installed parsers**, indistinguishable from opening a rosbag. A parser fix therefore applies to old recordings.

```
                       pushMessage(binding, t, bytes)
plugin ──────────────► DataSourceRuntimeHost ──► parser ──► datastore      (unchanged live path)
                               │
                               └── RecordTap ──► Recorder ──► McapRecordingWriter ──► *.mcap
                                   (per source)  byte-budgeted queue,  1 MiB chunks, large messages
                                                 writer thread         isolated in their own chunk
```

### 3.2 Components

| Component | Module | Role |
|---|---|---|
| `RecordTap` | `pj_runtime` | One callback on the push path. Non-owning views, call-lifetime. A relaxed atomic makes the no-tap case free. |
| `Recorder` | `pj_runtime` | Per-source queue + writer thread. Overflow policy is a mode: drop-largest for live streams (§3.3); opt-in blocking for captures (§3.4). |
| `McapRecordingWriter` | `pj_runtime` | Checked file sink (exclusive create, errno latched, fsync before close), schema dedup, per-channel sequences, `pj.recording` metadata at open and close. |
| `RecordingService` | `pj_runtime` | One Record press = N recorders, one file per source in one batch folder; transactional start, synchronous stop, one result per press. |
| `SourceCacheStore` (planned) | `pj_runtime` | Cache root, index (identity → path, size, hash, last hit), cross-process lock, LRU eviction that skips pinned artifacts. |
| `SourceCaptureService` (planned) | `pj_runtime` | Decides to capture a descriptor import, runs a lossless `Recorder` into a partial, publishes on clean completion. |
| Shell wiring | `pj_app` | Record/Stop in the streaming strip, Preferences (folders, budgets, toggle), the one ABI vtable entry, the cache-first hook in layout restore. |

### 3.3 Recording (Record/Stop)

- Record captures **every active delegated-ingest source**; each gets its own MCAP in
  `<recordings dir>/pj_<UTC yyyyMMdd_HHmmss>/<sanitized source>.mcap`. The folder is created atomically; all
  files of one press share a `capture_id`.
- Only **subscribed** topics are recorded (demand-driven subscriptions make a recording "what I was looking at").
- The queue is **byte-budgeted per source** and never blocks ingest: when full, the **largest** queued message
  is dropped (or the incoming one, if nothing queued is bigger; ties go against the newest). The recording
  continues with holes; only a sink error ends it.
- Stop finalizes synchronously (bounded by the queue budget plus one `fsync` per file) and reports one outcome
  per file: `clean`, `lossy` (drops), `truncated` (sink error), `incomplete` (could not finalize; `mcap recover`
  rebuilds the index).
- The file at Stop **is the product**: the live source keeps running; the recording is opened like any MCAP.

### 3.4 Source cache

- **Host-driven and transparent.** The provider states once, at download start, "this dataset is request X"
  (`attach_source_record(dataset, identity, descriptor_json)` — the single SDK addition). The host does the rest:
  it captures every descriptor import (behind a Preferences toggle and a budget), publishes the artifact into its
  index on a clean completion, and on layout restore **checks the index before asking the provider anything**.
- A **hit** is a stock `data_load_mcap` load — no network, no trust prompt, and the provider plugin need not be
  installed. A **miss** is today's trust-gated import, silently captured.
- Captures are **lossless**: the push thread blocks at budget instead of dropping. A download is the one case
  where a little speed is worth a guaranteed artifact.
  `RecorderOptions::OverflowPolicy::kBlock` supplies this queue behavior. It admits one oversized message
  only when no other message is queued or being copied; the writer's in-flight message is outside the queue
  budget. Cancellation calls `requestStop()` before joining producers, then `stop()` drains and finalizes.
  Sink failures also release waiting producers. The capture service must still verify successful transport,
  full topic coverage, no cancellation or lazy skips, and a clean recorder summary before cache publication;
  a structurally valid MCAP footer alone does not establish completion.
- Validation on a hit is cheap (present, size matches, footer intact); a loader failure evicts the entry and falls
  back to a download once. The **index is authoritative**; the artifact stays a plain recording.
- Eviction pins are host-internal (an artifact is never evicted while a dataset reads it). No lease ids cross the
  ABI, so no artifact lifetime can depend on a plugin DSO staying loaded.
- The cache has its **own folder and budget**, separate from the recordings folder: recordings are user files and
  are never evicted; the cache is disposable.
- **As built:** `SourceCacheStore` (storage: leases, pin-then-validate, publish, quarantine, budget) +
  `SourceCaptureService` (capture: the counting tap over a lossless recorder, the publication gate, the
  `pj.capture` completion manifest embedded in the artifact, and cache-first `resolve()`), gated by the SDK 0.30
  completion contract (`attach_source_record` + `complete_ingest` implemented on `DataSourceRuntimeHost`;
  `discard_parser_ingest` on `ToolboxRuntimeHost`). Publication requires the full gate — an explicit COMPLETED
  terminal with declared-topic coverage (zero-message topics only under the empty-topic attestation flag),
  a committed transaction, no cancellation, no latched callback failures, no pure-lazy skips, and a clean
  recorder close. Remaining for M3: layout record + cache-first restore wiring + Preferences (PR 3), and the
  provider-side attachment/completion in Mosaico (PR 4).

## 4. Decisions and accepted trade-offs

| Decision | Accepted cost |
|---|---|
| **Raw messages only**, never decoded samples. | Sources that write decoded data directly (LSL, dummy, Mosaico before its split) are not recordable until they speak messages. |
| **Subscribed topics only.** | No "record all" mode; stated in the UI. |
| **One file per source**, grouped per press. | N writer threads and N queue budgets per press; `mcap merge` when one shareable file is wanted. |
| **Streams drop-largest, never block.** | A slow disk produces holes in the biggest topics; users are told, not slowed. |
| **Captures block, never drop.** | A slow disk slows a download; in exchange the artifact is always complete. |
| **File at Stop; no live-to-file switch.** | Plots stay bound to the live source; opening the recording is a separate, ordinary action. |
| **Parser policy in the layout, not in the file (D9).** | A recording opened directly uses the loader dialog's (remembered) settings; the layout persists the choice. |
| **Provenance minimal; index authoritative.** | Losing the cache index costs a re-download, not data. |
| **Host-driven cache, one plugin call.** | Providers cannot customize what is cached; the toggle is the only user control. |
| **Capture all imports by default.** | Disk churn from one-off explorations, bounded by budget and LRU. |
| **Cheap hit validation, heal on failure.** | A same-size corrupt file reaches the loader before being evicted. |
| **Recordings never evicted; cache LRU-evicted.** | Two folders and two budgets to explain. |

## 5. Alternatives considered

- **Plugin-side caching** (pj-official-plugins #275 as first written: capture tee, `.pjmosaico` format, loader
  plugin, DSO-lifetime leases, budget). Rejected: every connector would re-implement format, loader, eviction
  and locking; the leases' lifetime was tied to the plugin DSO rather than to the datasets reading the files;
  a proprietary format would have to stay replayable forever. What survives from it: the typed request
  descriptor and identity scheme (pinned by a byte-for-byte vectors file), the trust allowlist, the
  credential-origin guard (including the grpc/grpc+tls storage-key aliasing rule that prevents a
  credential downgrade), the presentation hygiene for layout-supplied text, the headless import-job
  shape, and its test corpus (descriptor vectors, credential matrix, the three-leg E2E whose warm leg
  is zero-network by construction). The full harvest ledger — including the operational rules the
  host cache must reproduce (leased-miss-with-retry semantics, commit-can-lose-the-lease recovery,
  quarantine-on-corrupt-load, raw framing preflight, streamed replay with per-batch cancel) — is the
  closing comment on pj-official-plugins #275.
- **Provider-driven cache ABI** (lookup / adopt / attach / complete plus lease ids — the first M3 draft).
  Rejected in favour of the transparent design: five calls for every provider to get right, and a hit that still
  required the provider to run. One call now covers it.
- **Tapping the datastore write bridge** (decoded samples). Rejected: contradicts raw-only, needs a PJ-native
  schema, and replay could never re-parse.
- **Parser configuration inside the MCAP** (pj-official-plugins #280). Rejected: data vs. interpretation — two
  people may read one recording with different policies, and a policy change must not rewrite a multi-GB file.
- **Promote-at-stop** (switch the live source onto its recording). Dropped: a one-moment convenience costing a
  new affordance, a clean/lossy policy and a stop-then-replace sequence with its own failure modes.
- **Promotion after cache publish** (objects go lazy). Deferred: real memory win, but a second parse of every
  download at the end; kept as the known remedy if RAM becomes the problem.
- **`.partial` + rename + fsync of the directory.** Dropped: MCAP is recoverable by design and no other recorder
  does it.

## 6. Consequences and known limits

- **Recordable = speaks messages.** Direct-write connectors must split transport from parsing (Mosaico:
  `parser_arrow` + a transport-only plugin) to become recordable and cacheable — which is also what lets the
  host route them through installed parsers.
- **Pure-lazy pushes leave a hole.** A file-backed source that pushes an object lazily (bytes fetched later) is
  counted, not recorded. Only matters once file-backed sources are recordable.
- **Peak memory during a capture is unchanged.** Objects are eager until the artifact is published. The
  recorder-as-disk-spill extension (serve lazy fetches from the file being written) would remove that and,
  more importantly, bound a live stream's object history by disk instead of RAM; it is an identified follow-up,
  not a commitment.
- **Prefixes are not recorded.** The host applies source prefixes below the seam; a replayed file gets the file
  source's own naming.
- **Secrets.** The recorder stores bytes verbatim; a connector must never push authentication material as a
  message (rule, documented for plugin authors).

## 7. Where to look

- Working design, decision history (D1–D9), milestones: [`session_recorder_design.md`](./session_recorder_design.md).
- Code: `pj_runtime/include/pj_runtime/{RecordTap,Recorder,McapRecordingWriter,RecordingService}.h`, shell wiring
  in `pj_app/src/MainWindow.cpp` and `pj_app/src/StreamingSourceManager.cpp`.
- Request identity and descriptor types: SDK `pj_plugins/sdk/descriptor_import/`.
