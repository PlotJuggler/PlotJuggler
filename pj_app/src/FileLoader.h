#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>

#include "LoadInput.h"
#include "pj_base/types.hpp"
#include "pj_runtime/SessionManager.h"

QT_BEGIN_NAMESPACE
class QWidget;
class QThread;
class QDialog;
QT_END_NAMESPACE

namespace pj::scene3d {
class TransformService;
}  // namespace pj::scene3d

namespace PJ::sdk {
class ObjectIngestPolicyResolver;
}  // namespace PJ::sdk

namespace PJ {

class CatalogModel;
class BrowserFileStore;
class DataSourceRuntimeHost;
class ExtensionCatalogService;

// Worker↔GUI rendezvous for the synchronous plugin message-box ABI (defined in
// FileLoader.cpp). Heap-shared so the queued GUI lambda and a blocked worker
// can safely outlive both a shutdown and the FileLoader itself.
struct PluginMessageGate;

// How a load resolves its plugin configuration relative to the data-source
// dialog (LoadHints::dialog_policy).
enum class DialogPolicy {
  // Plain interactive load: the dialog drives, pre-filled from QSettings.
  kInteractive,
  // Apply the saved preset and skip the dialog when the preset names the
  // SELECTED plugin and applies cleanly; anything else (missing or
  // mismatched hint, rejected preset) falls back to the interactive dialog.
  kPreferPreset,
  // Automated (layout-driven) replay: dialogs are PROHIBITED and the saved
  // preset is AUTHORITATIVE — even an intentionally empty one (a plugin
  // whose saveConfig returned nothing is legitimate; it gets the minimal
  // fresh-filepath config). A preset the plugin rejects FAILS the source
  // (kFailed). Also skips the QSettings persist an interactive load performs.
  kNever,
};

// Hints supplied by callers that already know what plugin to use and what
// config to apply (e.g. layout-driven reload). dialog_policy decides how the
// saved preset_config_json and the data-source dialog interact — see
// DialogPolicy above.
struct LoadHints {
  // Stable embedded-manifest id of the plugin the layout saved (new layouts'
  // manifest_id attribute). Non-empty -> the manifest-id tier of the
  // require_expected_plugin scan applies, swept across ALL extension matches
  // before any display-name comparison. Never matched against display names.
  QString expected_manifest_id;
  // Display name of the plugin the layout saved (the ID attribute — the only
  // identity legacy layouts carry). Matched against candidate display names
  // ONLY, as the fallback tier after expected_manifest_id: an ID-only legacy
  // layout therefore starts at the name tier and can never silently select a
  // plugin whose MANIFEST id merely collides with the saved display name.
  QString expected_plugin_id;
  QString preset_config_json;  // Empty -> no hint; QSettings pre-fill is used.
  DialogPolicy dialog_policy = DialogPolicy::kInteractive;
  // Layout replay reuses matching DatasetIds; normal load/reload replaces them.
  bool prefer_reuse = false;
  // A layout that NAMES a plugin (browser source-bound replay, and desktop
  // layout replay since manifest ids landed) must use that plugin. If it is
  // not among the extension matches — by manifest id or display name — fail
  // the load visibly instead of silently choosing the first plugin. Default
  // false preserves plain interactive selection.
  bool require_expected_plugin = false;
  // A saved browser preset contains the old upload's vanished MEMFS filepath.
  // Rewrite only that field to LoadInput::backing_path before loadConfig().
  // Honored by the kNever policy (automated replay is the only rewrite
  // consumer). Default false preserves desktop preset bytes exactly.
  bool rewrite_preset_filepath = false;
  // Non-zero -> this load REPLACES the given dataset regardless of source
  // identity (the dataset "Replace" action: the incoming file is usually a
  // different path). Rides the transactional reload machinery — the in-place
  // refill keeps DatasetId/TopicIds (and so curve keys) stable where topic
  // names match, cancel/failure restores the prior data, and the dataset is
  // renamed to the new source only on commit. A vanished target degrades to a
  // plain fresh load unless require_replacement forbids it.
  DatasetId replace_dataset_id = 0;
  // With replace_dataset_id: the target must still exist when the request is
  // dequeued — a vanished target FAILS the load (kFailed, naming the target)
  // instead of degrading to a plain fresh load. No effect without
  // replace_dataset_id. Default false preserves the degrade.
  bool require_replacement = false;
};

// Ticket naming one ACCEPTED load request, minted at enqueue time by
// FileLoader::loadFileTicketed. Monotonic per loader; 0 means invalid/none.
using LoadRequestId = quint64;

// Terminal classification of one accepted load request (FileLoader::loadFinished).
enum class LoadOutcome {
  kLoaded,     // data committed; fileLoaded fired for this request
  kFailed,     // the load errored out; fileLoadFailed carried the reason
  kCancelled,  // user stop (keep or discard), config-dialog reject, queued cancel, or shutdown
};

// Drives the file-import path: pick a file, find the matching DataSource
// plugin via ExtensionCatalogService, ingest into the SessionManager's data
// engine, and refresh the curve catalog. Lives in pj_app because it talks to
// QFileDialog/QMessageBox and to pj_marketplace's plugin handles.
class FileLoader : public QObject {
  Q_OBJECT
 public:
  FileLoader(
      SessionManager& session, ExtensionCatalogService& extensions, CatalogModel& catalog, QObject* parent = nullptr);
  ~FileLoader() override;

  FileLoader(const FileLoader&) = delete;
  FileLoader& operator=(const FileLoader&) = delete;

  // Opens a multi-select file dialog filtered by every installed file-import
  // plugin's extensions. On accept, runs loadFile() for EACH chosen path in
  // order (so you can populate several datasets in one go). The chosen directory
  // is persisted in QSettings under "FileLoader/lastDir".
  void openFromDialog(QWidget* dialog_parent);

  // The dataset "Replace" entry point: same plugin-extension filter and lastDir
  // persistence as openFromDialog, but single-select, and the chosen file loads
  // with replace_dataset_id set so it transactionally replaces `dataset_id`'s
  // data (see LoadHints::replace_dataset_id).
  void replaceFromDialog(DatasetId dataset_id, QWidget* dialog_parent);

#ifdef PJ_TARGET_WASM
  struct BrowserSelectionResult {
    QString browser_name;
    std::optional<LoadInput> input;
    QString error;
  };
  using BrowserSelectionCallback = std::function<void(BrowserSelectionResult)>;

  // Opens the same browser picker/staging path used by openFromDialog, but
  // returns the staged LoadInput without enqueuing it. Source-bound layout
  // replay uses this to finish every required selection before importing any
  // data. Cancellation is an empty input/error pair.
  void selectBrowserInput(QWidget* dialog_parent, BrowserSelectionCallback callback);

  // Fingerprint captured while BrowserFileStore staged this opaque upload.
  // Empty for native paths, unknown identities, and failed pre-staging calls.
  // Kept outside SessionManager because it is browser-session metadata, not a
  // durable source path or plugin concern.
  [[nodiscard]] QString browserContentSha256(const QString& source_identity) const;
#endif

  // Resolves the "pick file(s)" interaction inside openFromDialog() /
  // replaceFromDialog(). Returns the selected paths (empty on cancel; at most
  // one entry when `multi` is false). The shell injects one that threads
  // MainWindow's chrome metrics into PJ::FileDialog, keeping FileLoader free of a
  // MainWindow link (which also makes it testable headlessly). Unset -> plain
  // PJ::FileDialog::getOpenFileName(s), no metrics.
  using FilePicker = std::function<QStringList(
      QWidget* parent, const QString& caption, const QString& dir, const QString& filter, bool multi)>;
  void setFilePicker(FilePicker picker) {
    file_picker_ = std::move(picker);
  }

  // Programmatic entry point. ENQUEUES the load and returns immediately; the
  // bool now means "accepted/enqueued", NOT "loaded" — a single-instance load
  // runs progressively on a worker thread, so completion is asynchronous and
  // signalled by fileLoaded()/fileLoadFailed(). Loads run sequentially; a
  // second call while one is in flight queues behind it. Fanout control slots
  // run on the GUI thread, while each blocking import step runs on a worker.
  bool loadFile(const QString& path, QWidget* dialog_parent = nullptr);
  bool loadFile(const QString& path, QWidget* dialog_parent, const LoadHints& hints);
  bool loadFile(LoadInput input, QWidget* dialog_parent = nullptr, const LoadHints& hints = {});

  // Ticketed enqueue: identical semantics to loadFile (the overloads above
  // delegate here), but returns the request's LoadRequestId — 0 when the input
  // is rejected outright (nothing enqueued, no loadFinished for it). Every
  // ACCEPTED request emits loadFinished EXACTLY ONCE, on every path: success,
  // failure, cancel (queued or active), config-dialog reject, and shutdown.
  // Terminals are delivered through the event loop — even a request that
  // resolves synchronously inside this call cannot see its loadFinished before
  // the caller has recorded the returned ticket — with ONE deliberate
  // exception: joinForShutdown (and so destruction) flushes undelivered
  // terminals synchronously, because teardown must never lose them.
  LoadRequestId loadFileTicketed(LoadInput input, QWidget* dialog_parent = nullptr, const LoadHints& hints = {});

  // Cancel the in-progress worker load. keep_partial=true keeps the rows parsed
  // so far (Primary/"Cancel"); false discards them (Secondary/"Discard"),
  // leaving whatever preceded this load: a first load's dataset is deleted, a
  // replacing reload rolls back to its pre-reload data. No-op when no worker
  // load is running.
  void cancelCurrent(bool keep_partial);

  // Generation-guarded cancel: no-op unless `generation` equals the load
  // currently processing. A stop dialog captures the generation of the load it
  // was opened for (loadGeneration()) and passes it here, so a confirmation left
  // up after that load finished and a NEXT load began cannot cancel the wrong
  // load. keep_partial as above.
  void cancelCurrent(std::uint64_t generation, bool keep_partial);

  // Cancel ONE request by ticket. A still-QUEUED request is removed at once
  // (its loadFinished(kCancelled) is scheduled before returning and arrives
  // via the event loop); the ACTIVE request is cancelled through the
  // generation machinery (equivalent to cancelCurrent(loadGeneration(),
  // keep_partial) — its terminal arrives when the load winds down). Returns
  // false — and resolves nothing — for an unknown or already-terminal ticket,
  // so repeated cancels are idempotent. On the loader's (GUI) thread the
  // queue/ticket state is single-threaded by design (workers only marshal
  // back via queued calls), so a queued→active transition cannot race this
  // call. Called from ANY OTHER thread it degrades to best-effort: the cancel
  // is marshalled (see cancelLoadAsync) and the return value is always false.
  bool cancelLoad(quint64 ticket, bool keep_partial = false);

  // Fire-and-forget cancel-by-ticket, callable from any thread (the one entry
  // a batch worker may hit off-thread): queues the cancel onto the loader's
  // thread, where it runs with cancelLoad's exact semantics. The result is
  // observable only through loadFinished.
  void cancelLoadAsync(quint64 ticket, bool keep_partial = false);

  // Monotonic id of the load currently being processed (worker or suspended
  // prologue), or 0 when idle. Bumped each time a queued request begins. The
  // stop-dialog uses it to bind its cancel/auto-dismiss to one specific load.
  [[nodiscard]] std::uint64_t loadGeneration() const {
    return load_generation_;
  }

  // Ticket of the request currently being processed — the one loadGeneration()
  // belongs to — or 0 when idle. Advanced together with the generation in
  // startNext, so reading both from a loadGenerationAdvanced slot yields a
  // consistent (generation, ticket) pair for the stop-dialog; a ticket can
  // never be attributed to a later load's generation.
  [[nodiscard]] quint64 currentLoadTicket() const {
    return current_load_ticket_;
  }

  // True while a load is running (worker active or mid-prologue) or queued.
  [[nodiscard]] bool isBusy() const;

  // DatasetId of the single-instance load currently filling on the worker, or 0
  // when none. Lets the shell grow the playback range as that dataset fills.
  // GUI-thread only.
  [[nodiscard]] DatasetId activeLoadDatasetId() const;

  // Stop the worker (discard) and drain the queue. Call from MainWindow::closeEvent
  // BEFORE tearing down the session/datastore; also invoked by the destructor.
  void joinForShutdown();

  // 3D TF ingest is triggered at load time through this service (owned by the
  // app shell, not the domain-neutral runtime). When unset, TF ingest is
  // skipped — non-3D builds simply never set it.
#ifdef PJ_WITH_SCENE3D
  void setTransformService(pj::scene3d::TransformService* service) {
    transform_service_ = service;
  }
#endif

  // Normalized full filesystem path the given dataset was loaded from, or empty
  // if this loader did not create it (e.g. a streaming or test dataset, or an id
  // it has since forgotten). Reads through SessionManager's source-path registry
  // (the single owner of dataset->path identity). The shell uses this to
  // translate a DatasetId back to the loaded-source entry when a dataset is
  // removed.
  [[nodiscard]] QString sourcePathForDataset(DatasetId dataset_id) const;
  // Drop the session's dataset->path association after dataset removal. Safe to
  // call for unknown ids.
  void untrackDataset(DatasetId dataset_id);

  // True when two source strings name the same loaded source: exact match for
  // opaque browser-upload identities, canonical filesystem-path identity
  // otherwise. The single comparison rule for matching recorded sources —
  // callers must use this instead of raw path equality.
  [[nodiscard]] static bool sameSourceIdentity(const QString& lhs, const QString& rhs);

  // Configure the object-ingest policy every load uses: scalars eager, objects
  // lazy-on-pull by default, and the heavy / scalar-less payloads (point clouds,
  // compressed point clouds, video frames, images, depth images, scene entities,
  // image annotations) PURE-LAZY so their bytes are re-fetched on read instead of
  // pinned in RAM at ingest. Static + resolver-typed so it is unit-testable
  // without standing up a full DataSourceRuntimeHost. TF stays eager on purpose:
  // its payload is tiny and its scalar fields are useful.
  static void applyDefaultIngestPolicies(PJ::sdk::ObjectIngestPolicyResolver& resolver);

 signals:
  /// A fan-out replacement is about to retire `dataset_id`. The shell captures
  /// path-qualified workspace state — stamping the retiring dataset with `path`,
  /// which on a dataset "Replace" load is a DIFFERENT file than it was loaded
  /// from — before the old catalog item is hidden, then rebinds it after
  /// fileLoaded exposes the reminted datasets. `ticket` is the arming load's
  /// LoadRequestId: the shell keys the captured state to it so only THIS load's
  /// terminal can disarm the capture — a path match alone would let a stale
  /// same-path terminal from another (queued/cancelled) load kill a live one's.
  void sourceReplacementAboutToCommit(quint64 ticket, const QString& path, DatasetId dataset_id);

  // `plugin_id` is the selected plugin's DISPLAY name (the historical value);
  // `plugin_manifest_id` is its stable embedded-manifest id — the durable
  // identity the layout writer persists alongside the display name. Existing
  // 4-argument slots keep compiling (Qt slots may take fewer arguments).
  void fileLoaded(
      const QString& path, const QString& prefix, const QString& plugin_id, const QString& plugin_config_json,
      const QString& plugin_manifest_id);
  void fileLoadFailed(const QString& path, const QString& reason);

  // Emitted when a load begins ingesting (after the modal dialog). `title` is the
  // filename; `file_index`/`file_total` are 1-based position among queued loads
  // (total<=1 => no N-of-M). `determinate` is false when the plugin reported no
  // step total (the bar should show busy/indeterminate).
  void ingestStarted(const QString& title, int file_index, int file_total, bool determinate);
  // Progress of the current load (maximum 0 => busy). Marshalled to the GUI thread.
  void ingestProgress(int current, int maximum);
  // The active load and the queue are both empty.
  void queueDrained();
  // A queued load took over as the current generation (emitted before its
  // dialog/ingest phases run). UI bound to a previous generation — e.g. the
  // stop-confirmation dialog — dismisses on this: fileLoaded/fileLoadFailed
  // fire while the finished load is still the current generation, so they
  // cannot signal the transition.
  void loadGenerationAdvanced(std::uint64_t generation);

  // A queued request became the CURRENT load (emitted from startNext, right
  // before that generation's loadGenerationAdvanced). Announces the
  // ticket↔generation association while keeping the two id spaces
  // independent: tickets are minted at enqueue, generations at dequeue.
  void requestStarted(quint64 ticket, std::uint64_t generation);

  // Terminal result of one ACCEPTED request (see loadFileTicketed): exactly
  // one emission per ticket, on every path — including exits the legacy
  // signals stay silent on (config-dialog reject, queued cancel, shutdown) —
  // ALWAYS delivered through the event loop, never inside the call that
  // resolved the request. `effective_path` is the request's source identity
  // (what fileLoaded / fileLoadFailed report). `produced_dataset_ids` are ALL
  // datasets holding this load's data (fan-out: every loaded entry);
  // `dataset_id` is the single-id convenience — the primary (first) produced
  // dataset, or 0 when the load left none (failures, discards, rejects).
  void loadFinished(
      quint64 ticket, LoadOutcome outcome, const QString& effective_path, DatasetId dataset_id,
      const QVector<DatasetId>& produced_dataset_ids);

  // Pre-catalog commit seam, SYNCHRONOUS: fires while a fully-loaded request
  // commits through finishLoadOnGui — its dataset, source path, and captured
  // plugin config are installed, and the catalog rebuild that publishes the
  // load has NOT yet run (it follows immediately after, then fileLoaded and
  // the queued loadFinished). A promotion service uses this to attach source
  // records atomically with catalog publication. Emitted only for loads that
  // finish kLoaded via the single-instance commit path — never for failures,
  // discards, keep-partial stops, nor the prefer_reuse and fan-out epilogues
  // (which do not commit through finishLoadOnGui).
  void loadCommitting(
      quint64 ticket, const QVector<DatasetId>& produced_ids, const QString& plugin_id,
      const QString& captured_config_json);

 private:
#ifdef PJ_TARGET_WASM
  // Shared browser-picker completion for openFromDialog / replaceFromDialog:
  // stages the selection via selectBrowserInput, reports failures, and enqueues
  // the staged input through loadFile with `hints` (the entry points differ
  // only in the hints they pass). Must be called from a user-activation turn.
  void loadFromBrowserPicker(QWidget* dialog_parent, LoadHints hints);
#endif

  // One queued load request (a single loadFile call).
  struct LoadRequest {
    LoadInput input;
    QPointer<QWidget> dialog_parent;
    LoadHints hints;
    int file_index = 1;
    int file_total = 1;
    quint64 ticket = 0;  // minted at enqueue (loadFileTicketed); never 0 once queued
  };
  // Per-load state that must outlive the GUI prologue into the worker and back.
  // Defined in the .cpp (holds a DataSourceHandle + DataSourceRuntimeHost).
  struct LoadContext;
  // Owned coroutine frame for the GUI-side load prologue. It keeps the staged
  // file lease, plugin handle, rollback state, and dialog inputs alive while a
  // browser-safe plugin dialog is open. Defined in the .cpp.
  struct BeginLoadTask;

  // Tears down one dataset created by an abandoned/rolled-back load: TF
  // invalidation (if a transform_service_ is wired), then optionally
  // ObjectStore eviction and/or the catalog-item removal, then the engine-side
  // erase via SessionManager::removeDataset (never dataEngine directly, so the
  // pinned time-origin is invalidated and the global reframe fires if needed).
  // TF is ALWAYS invalidated before eviction — invalidateDataset's cursor
  // cleanup walks listTopics(id), which eviction would otherwise empty first.
  // evict_objects=false skips ObjectStore eviction for a shell that never
  // started ingest (nothing to evict). remove_from_catalog=false skips the
  // catalog call for a dataset already tombstoned by an earlier removeDataset.
  void removeCreatedDataset(DatasetId dataset_id, bool evict_objects = true, bool remove_from_catalog = true);

  // GUI-thread body of one throttled ingest flush, shared by the single-instance
  // and fan-out progress ticks (each keeps its own staleness guard): publish the
  // newly committed rows (through SessionManager's ingest token when the
  // producer started one, directly otherwise), publish the worker-fed TF
  // revision so 3D scenes track the load live, and advance the progress strip.
  // `dataset_id` is the load's own, never `token.dataset_id`: a producer that
  // ticks without a begin carries no dataset in its token.
  void publishIngestProgress(DatasetId dataset_id, IngestToken token, uint64_t current, uint64_t total);

  // One scheduled loadFinished payload awaiting event-loop delivery, or a
  // synchronous flush at shutdown/destruction: delivery-or-flush, never loss.
  struct PendingTerminal {
    quint64 ticket;
    LoadOutcome outcome;
    QString effective_path;
    DatasetId dataset_id;
    QVector<DatasetId> produced;
  };

  // The single terminalize point for loadFinished: no-ops if the current
  // request already resolved, otherwise marks it terminal FIRST — every exit
  // path calls this BEFORE emitting any externally re-entrant legacy signal,
  // so a slot re-entering cancelLoad/joinForShutdown sees the request as
  // resolved — then queues the emission onto the event loop.
  void emitLoadFinished(
      quint64 ticket, LoadOutcome outcome, const QString& effective_path, DatasetId dataset_id,
      QVector<DatasetId> produced_dataset_ids = {});
  // Deliver the oldest pending terminal (queued from emitLoadFinished); no-op
  // when a shutdown flush already emptied the deque.
  void deliverPendingTerminal();
  // Emit every pending terminal NOW, in FIFO order — the shutdown/destruction
  // exception to queued delivery.
  void flushPendingTerminals();
  // End a fan-out entry's ingest on the next event-loop turn instead of here.
  //
  // The fan-out loop runs INSIDE the beginLoad coroutine, and an ingestEnded
  // observer is allowed to react by calling joinForShutdown() — which resets
  // begin_load_task_ and so destroys the frame the loop is executing in. Ending
  // the ingest from a queued call keeps that re-entrancy off the coroutine's
  // stack. The caller must drop its own record of the token FIRST: shutdown
  // terminalizes whatever tokens it still finds (see joinForShutdown), and a
  // token that is both queued here and still recorded would end twice.
  void postIngestTerminal(IngestToken token, IngestOutcome outcome);

  // GUI: dequeue and begin the next load if idle. Emits queueDrained when the
  // queue empties with neither a suspended prologue nor a worker.
  void startNext();
  // GUI coroutine for one request: resolve plugin, await its dialog without a
  // nested event loop, then either hand completion to the ingest worker or
  // finish the prologue and advance the queue.
  [[nodiscard]] BeginLoadTask beginLoad(LoadRequest request);
  // GUI: complete a non-worker prologue and schedule the next request without
  // recursively replacing the currently executing coroutine frame.
  void finishPrologue();
  // WORKER thread body: runs ctx_->handle.start(), driving throttled
  // flushPending()+queued token-routed ingest updates from on_progress_update.
  void runIngestOnWorker(std::uint64_t generation);
  // GUI (queued from the worker): join the worker, finalize or discard, then
  // startNext().
  void onWorkerFinished(std::uint64_t generation);
  // GUI: post-ingest reconciliation for the just-finished single-instance load
  // (saveConfig, source-path tracking, the loadCommitting seam, catalog
  // rebuild, TF ingest, fileLoaded). fully_loaded=false (a user keep-stop)
  // commits identically but skips the loadCommitting seam.
  void finishLoadOnGui(bool fully_loaded);
  // GUI: after a replacing reload's RefillGuard rolls back, rebuild the catalog
  // and discard the private TF generation; the active buffer was never replaced.
  /// Failure exit for a replacing reload: roll the refill back (guard dtor),
  /// refresh the catalog/UI to the restored state, and emit fileLoadFailed.
  void failReplacingLoad(DatasetId dataset_id, const QString& path, const QString& reason);
  void refreshAfterReplacingRollback(DatasetId dataset_id);

  // Surface one load failure to the user, aggregating consecutive failures into
  // a SINGLE dialog: while a warning dialog is still open, each new reason is
  // appended to its body (and the title becomes "N loads failed") instead of
  // opening another overlapping box. Non-blocking (show(), not exec()), so the
  // async load queue is never stalled. `parent` may be null (headless/no-UI
  // load) — then only the count is tracked and no dialog is shown.
  void reportLoadWarning(QWidget* parent, const QString& reason);

  SessionManager& session_;
  ExtensionCatalogService& extensions_;
  CatalogModel& catalog_;
  FilePicker file_picker_;
  std::shared_ptr<BrowserFileStore> browser_file_store_;
#ifdef PJ_TARGET_WASM
  QHash<QString, QString> browser_content_sha256_;
#endif
#ifdef PJ_WITH_SCENE3D
  pj::scene3d::TransformService* transform_service_ = nullptr;
#endif

  // --- Sequential async load queue (single-instance loads run on a worker) ---
  std::deque<LoadRequest> queue_;
  std::unique_ptr<BeginLoadTask> begin_load_task_;
  std::unique_ptr<QThread> worker_;
  // Runtime host of the fan-out entry whose start() is on the worker right now
  // (else nullptr). The host is a coroutine-frame local, so cancelCurrent /
  // joinForShutdown reach its requestStop() through this. GUI-thread only.
  DataSourceRuntimeHost* active_fanout_ingest_ = nullptr;
  // One active SessionManager ingest token per fan-out dataset. Entries are
  // removed as their iterations terminalize; shutdown cancels any survivor.
  std::unordered_map<DatasetId, IngestToken> fanout_ingest_tokens_;
  // GUI-thread pointer to the synchronous-ABI plugin question currently open.
  // Shutdown rejects it before joining a worker that may be waiting for it.
  // Heap-shared because the WA_DeleteOnClose dialog (a child of the main
  // window) can outlive this loader: its finished/destroyed hooks capture this
  // shared state, never FileLoader memory.
  std::shared_ptr<QPointer<QDialog>> active_plugin_message_dialog_;
  // joinForShutdown() shuts the gate (pending questions answer -1, queued ones
  // become no-ops) so a worker parked in the message-box ABI can never deadlock
  // the join; a fresh gate is minted afterwards so the loader stays reusable.
  std::shared_ptr<PluginMessageGate> message_gate_;
  // The single load-warning dialog open right now; V9 aggregation appends new
  // failure lines to it instead of stacking N overlapping dialogs. Cleared when
  // it closes (via its finished/destroyed hook). GUI-thread only.
  QPointer<QDialog> active_load_warning_;
  // Failure reasons shown in active_load_warning_, in order. Repopulates the
  // dialog body on each append; reset when the dialog closes.
  QStringList load_warning_lines_;
  std::unique_ptr<LoadContext> ctx_;  // current single-instance load; null when idle
  // True while a load is being processed (suspended prologue or worker).
  // GUI-thread only; guards startNext re-entrancy.
  bool active_load_ = false;
  // Monotonic id of the load currently processing; bumped each time startNext
  // dequeues a request. 0 while idle. GUI-thread only. Lets a stop-dialog bind
  // to one specific load (see cancelCurrent(generation, ...)).
  std::uint64_t load_generation_ = 0;
  // Counter backing load_generation_. Kept separate so returning to idle or
  // joinForShutdown() never reuses an old token: queued worker/progress calls
  // from an earlier load can therefore be rejected after shutdown + reuse.
  std::uint64_t next_load_generation_ = 0;
  // Ticket counter for loadFileTicketed: minted at ENQUEUE (one per accepted
  // request, unlike generations which are minted at dequeue), monotonic,
  // never 0. GUI-thread only.
  quint64 next_load_ticket_ = 0;
  // Ticket of the request currently processing, advanced together with
  // load_generation_ in startNext; 0 while idle. GUI-thread only.
  quint64 current_load_ticket_ = 0;
  // Source identity of the current request, kept for the shutdown terminal (a
  // prologue suspended at its config dialog has no ctx_ to read a path from).
  QString current_load_identity_;
  // True once the current request resolved (its loadFinished is scheduled or
  // delivered). Set BEFORE the resolving path emits any legacy signal, so a
  // re-entrant cancelLoad/joinForShutdown from such a slot cannot resolve the
  // request a second time, and a cancel landing in the finished-but-not-yet-
  // advanced event-loop hop cannot latch cancel_mode_ against the NEXT load.
  // Cleared when startNext installs the next request.
  bool current_ticket_terminal_ = false;
  // Outcome recorded when the current request terminalized — only meaningful
  // while current_ticket_terminal_ is set. Lets the first-wins guard flag a
  // conflicting second resolution loudly instead of swallowing it silently.
  LoadOutcome current_ticket_outcome_ = LoadOutcome::kLoaded;
  // Terminals scheduled but not yet delivered (FIFO). Normally drained one
  // per queued metacall; joinForShutdown flushes the remainder synchronously.
  std::deque<PendingTerminal> pending_terminals_;
  // Cancellation request for the current worker load: 0=none, 1=keep, 2=discard.
  // Written by cancelCurrent/joinForShutdown (GUI), read by the worker.
  // WHOLE-LOAD scope: it stops the fan-out entry in flight AND every entry
  // after it. The title-bar strip and shutdown are its only writers. A fan-out
  // ROW's stop uses the same encoding but a slot owned by that entry's loop
  // iteration, so it can only ever stop the entry the user aimed at.
  std::atomic<int> cancel_mode_{0};
  // Minimum wall-clock between worker-side flush+notify cycles (test seam).
  int flush_throttle_ms_ = 50;
  // Suppresses queue advancement while joinForShutdown destroys a suspended
  // dialog/prologue coroutine; cleared when shutdown completes so the loader
  // stays usable afterwards.
  bool shutting_down_ = false;
  // Set by the destructor before its joinForShutdown. Unlike a reusable
  // shutdown — whose final flush deliberately lets a loadFinished slot start
  // fresh work on the reset loader — nothing outlives the destructor to
  // resolve a newly accepted request or join its worker, so admission is
  // rejected (loadFileTicketed returns 0) while this is set.
  bool destroying_ = false;
};

}  // namespace PJ
