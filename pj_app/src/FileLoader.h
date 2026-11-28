#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QElapsedTimer>
#include <QHash>
#include <QMimeData>
#include <QObject>
#include <QPointer>
#include <QSet>
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

#include "DialogWatch.h"
#include "LoadInput.h"
#ifdef PJ_TARGET_WASM
#include "FileSelectionService.h"
#endif
#include "pj_base/diagnostic_sink.hpp"
#include "pj_base/types.hpp"
#include "pj_runtime/SessionManager.h"

QT_BEGIN_NAMESPACE
class QWidget;
class QThread;
class QDialog;
QT_END_NAMESPACE

namespace PJ::sdk {
class ObjectIngestPolicyResolver;
}  // namespace PJ::sdk

namespace PJ {

class TransformService;
class CatalogModel;
class BrowserFileStore;
class DataSourceRuntimeHost;
class ExtensionCatalogService;

// Defined in FileLoader.cpp.
struct PluginMessageGate;

// How a load's saved preset and the data-source dialog interact (LoadHints::dialog_policy).
enum class DialogPolicy {
  // The dialog drives, pre-filled from QSettings.
  kInteractive,
  // Skip the dialog when the hint names the selected plugin and its preset applies; otherwise the dialog.
  kPreferPreset,
  // Layout replay: no dialog, the preset is authoritative (an empty one gets a bare filepath config). The preset's
  // `filepath`, and each fan-out child's, become LoadInput::backing_path, so a moved layout replays the file beside
  // it. A non-object or rejected preset fails the source; a preset written for a plugin that is not the selected
  // one (the named plugin is not installed) is not replayed, the source gets a bare filepath config. Does not
  // persist the config to QSettings.
  kNever,
};

// How a batch (files picked or listed together, BatchLoadDialog) treats the loaders' configuration dialogs.
enum class BatchDialogMode {
  kShowAll,    // every file shows its dialog
  kShowFirst,  // the first file of each loader shows it; the others load with the settings accepted there
  kSkip,       // every file loads with its loader's last-used settings; a loader with none shows its dialog
};

// One batch's state, shared by the LoadHints of all its files. Loads run one at a time on the GUI thread, so a later
// file sees what an earlier one recorded.
struct LoadBatch {
  BatchDialogMode mode = BatchDialogMode::kShowFirst;
  QSet<QString> prompted;   // manifest ids of the loaders whose dialog this batch accepted (their settings saved)
  QSet<QString> cancelled;  // manifest ids of the loaders whose dialog was cancelled: their other files are cancelled
};

// For callers that already know the plugin and its config (layout replay, Replace, agents).
struct LoadHints {
  // The layout's manifest_id. Compared only against candidate manifest ids, tried before the name.
  QString expected_manifest_id;
  // The layout's ID attribute: the plugin's display name, the only identity old layouts carry. Compared only against
  // candidate display names, so it never selects a plugin whose manifest id equals it.
  QString expected_plugin_id;
  QString preset_config_json;  // empty: no preset, the QSettings pre-fill is used
  // Layout replay: each saved side-file name (layout_xml::SideFileRef::filename) and the path it now names. Every
  // preset string value equal to a name becomes its path before the filepath rewrite; an empty path means the
  // name beside the loaded data file (the desktop replay); the browser replay names its staged copies.
  QHash<QString, QString> side_file_paths{};
  DialogPolicy dialog_policy = DialogPolicy::kInteractive;
  // Reuse a matching DatasetId instead of reloading it (layout replay).
  bool prefer_reuse = false;
  // The plugin the hints name is selected when it is installed for the extension; this fails the load when it is not,
  // instead of taking the first match.
  bool require_expected_plugin = false;
  // With kNever, native inputs: the plugin also gets the replaced absolute path as `__pj_saved_filepath`, to rebase
  // sidecar files that lived beside the old source.
  bool record_saved_filepath = false;
  // Non-zero: replace this dataset whatever the source identity (the Replace action). An in-place refill keeps
  // DatasetId and TopicIds where topic names match; cancel or failure restores the prior data; the rename happens at
  // commit. A target gone by dequeue degrades to a fresh load unless require_replacement.
  DatasetId replace_dataset_id = 0;
  // With replace_dataset_id: a vanished target, or a fan-out expansion, fails the load instead.
  bool require_replacement = false;
  // Put before the plugin dialog's title (e.g. "An AI agent asked you to choose: "), so the user knows who opened it.
  QString dialog_title_prefix{};
  // Asked when the request leaves the queue and again once its dialog is accepted: false cancels the load
  // (kCancelled, no fileLoaded/fileLoadFailed) before anything is applied. For a caller whose right to load can lapse
  // while it waits (an MCP agent's revoked token).
  std::function<bool()> still_allowed{};
  // Told when this load's plugin dialog opens and when it is gone; closing it cancels the load.
  dialog_presenter::DialogWatch on_dialog{};
  // Set for every file of a batch; null for a single load.
  std::shared_ptr<LoadBatch> batch{};
};

// One accepted load request; 0 means none. Minted at enqueue, monotonic per loader.
using LoadRequestId = quint64;

enum class LoadOutcome {
  kLoaded,     // fileLoaded fired for this request
  kFailed,     // fileLoadFailed carried the reason
  kCancelled,  // user stop (keep or discard), config-dialog reject, queued cancel, or shutdown
};

// Picks the DataSource plugin for a file, runs its dialog and ingests into the SessionManager, one request at a time.
// Lives on the GUI thread; only each import's blocking start() runs on a worker.
class FileLoader : public QObject {
  Q_OBJECT
 public:
  FileLoader(
      SessionManager& session, ExtensionCatalogService& extensions, CatalogModel& catalog, QObject* parent = nullptr);
  ~FileLoader() override;

  FileLoader(const FileLoader&) = delete;
  FileLoader& operator=(const FileLoader&) = delete;

  // Multi-select; loads each chosen path. The directory persists in QSettings "FileLoader/lastDir".
  void openFromDialog(QWidget* dialog_parent);

  // Each file reports its own failures; one bad file does not stop the rest. Two or more files go through the batch
  // question (setBatchAsker) when it can change something (batchRepeatsADialog), and a loader whose dialog is then
  // cancelled cancels its other files.
  void loadFiles(const QStringList& paths, QWidget* dialog_parent);

  // True when a loader with a configuration dialog would get two or more of `paths`, each file's loader being the
  // first match for its extension, as beginLoad picks it without a layout hint. Otherwise the batch question changes
  // nothing.
  [[nodiscard]] bool batchRepeatsADialog(const QStringList& paths) const;

  // The local regular files among a drag's URLs, in order.
  [[nodiscard]] static QStringList droppedFilePaths(const QMimeData& mime);

  // Single-select; loads the pick with LoadHints::replace_dataset_id = `dataset_id`.
  void replaceFromDialog(DatasetId dataset_id, QWidget* dialog_parent);

#ifdef PJ_TARGET_WASM
  struct BrowserSelectionResult {
    QString browser_name;
    std::optional<LoadInput> input;
    QString error;
  };
  using BrowserSelectionCallback = std::function<void(BrowserSelectionResult)>;

  // The browser picker of openFromDialog, returning the staged input without enqueuing it (source-bound layout replay
  // collects every selection before importing). Cancel: no input and no error. A non-empty `name_filter` replaces
  // the catalog's data-file filter.
  void selectBrowserInput(
      QWidget* dialog_parent, BrowserSelectionCallback callback,
      FileSelectionService::Fingerprint fingerprint = FileSelectionService::Fingerprint::kBeforeSelection,
      const QString& name_filter = {});
#endif

  // The content fingerprint of a staged browser upload or preset download. A file picked through openFromDialog is
  // hashed in the background: empty until that finishes, for good if the file changed since the pick, and lost at
  // its first failed read. Empty for native paths and unknown identities.
  [[nodiscard]] QString browserContentSha256(const QString& source_identity) const;

  // Records a staged input's fingerprint and an upload's identity by backing path. Called for every accepted load
  // and every plugin-dialog pick, which a source-bound save looks up for the side files a config names.
  void rememberBrowserUpload(const LoadInput& input);

  // The upload identity staged at `backing_path`, while its file is still
  // trusted (a failed read forgets it); empty otherwise.
  [[nodiscard]] QString browserIdentityForPath(const QString& backing_path) const;
  // The inverse: where the upload `source_identity` is staged; empty when unknown.
  [[nodiscard]] QString browserPathForIdentity(const QString& source_identity) const;

  // The desktop picker of openFromDialog / replaceFromDialog (empty on cancel). The shell injects one carrying
  // MainWindow's chrome metrics, so FileLoader needs no MainWindow link. Unset: plain PJ::FileDialog.
  using FilePicker = std::function<QStringList(
      QWidget* parent, const QString& caption, const QString& dir, const QString& filter, bool multi)>;
  void setFilePicker(FilePicker picker) {
    file_picker_ = std::move(picker);
  }

  // Asked once when loadFiles gets two or more files: `done` receives the batch shared by their requests, or null to
  // load none of them. The shell injects BatchLoadDialog. Unset: each file loads on its own.
  using BatchAsker = std::function<void(
      QWidget* parent, const QStringList& paths, std::function<void(std::shared_ptr<LoadBatch>)> done)>;
  void setBatchAsker(BatchAsker asker) {
    batch_asker_ = std::move(asker);
  }

  // Receives load failures and the plugin's messages; unset logs only. A load in flight keeps the sink it started with.
  void setDiagnosticSink(DiagnosticSink sink) {
    diagnostic_sink_ = std::move(sink);
  }

  // Enqueues; true means accepted, not loaded. Loads run one at a time, completion is signalled.
  bool loadFile(const QString& path, QWidget* dialog_parent = nullptr);
  bool loadFile(const QString& path, QWidget* dialog_parent, const LoadHints& hints);
  bool loadFile(LoadInput input, QWidget* dialog_parent = nullptr, const LoadHints& hints = {});

  // 0 when the input is rejected (nothing enqueued, no loadFinished). Every accepted request emits loadFinished exactly
  // once, on every path, through the event loop: even a request resolved inside this call cannot deliver before the
  // caller holds the ticket. The exception: joinForShutdown, and so the destructor, delivers undelivered terminals
  // synchronously, because teardown must not lose them.
  LoadRequestId loadFileTicketed(LoadInput input, QWidget* dialog_parent = nullptr, const LoadHints& hints = {});

  // Stops the whole active load. keep_partial keeps the rows parsed so far; otherwise what preceded the load is left:
  // a first load's dataset is deleted, a reload rolls back to its prior data. No-op when idle.
  void cancelCurrent(bool keep_partial);

  // No-op unless `generation` is the load processing now, so a stop dialog left open past its load cannot cancel the
  // next one.
  void cancelCurrent(std::uint64_t generation, bool keep_partial);

  // A queued request is removed at once (its terminal still arrives through the event loop); the active one is
  // cancelled as cancelCurrent(loadGeneration(), keep_partial). False, resolving nothing, for an unknown or terminal
  // ticket. From another thread it forwards to cancelLoadAsync and returns false.
  bool cancelLoad(quint64 ticket, bool keep_partial = false);

  // Any thread: queues cancelLoad onto the loader's thread; the result shows only through loadFinished.
  void cancelLoadAsync(quint64 ticket, bool keep_partial = false);

  // The load processing now (worker or suspended prologue), 0 when idle; a new value per dequeued request.
  [[nodiscard]] std::uint64_t loadGeneration() const {
    return load_generation_;
  }

  // The ticket of loadGeneration()'s request, 0 when idle. Both advance before loadGenerationAdvanced, so its slots
  // read a consistent pair.
  [[nodiscard]] quint64 currentLoadTicket() const {
    return current_load_ticket_;
  }

  // A load is processing or queued.
  [[nodiscard]] bool isBusy() const;

  // The single-instance load filling on the worker, or 0. GUI thread.
  [[nodiscard]] DatasetId activeLoadDatasetId() const;

  // Discards the active load and the queue, joins the worker, and delivers every pending terminal before returning.
  // Call before the session or datastore is torn down; the destructor calls it too. The loader stays reusable.
  void joinForShutdown();

  // Unset: no TF ingest.
#ifdef PJ_WITH_SCENE3D
  void setTransformService(PJ::TransformService* service) {
    transform_service_ = service;
  }
#endif

  // The source identity a dataset was loaded from, from SessionManager's registry; empty for datasets no load
  // tracked (streaming, tests, untracked ids).
  [[nodiscard]] QString sourcePathForDataset(DatasetId dataset_id) const;
  // Safe for unknown ids.
  void untrackDataset(DatasetId dataset_id);

  // Exact match for browser-upload identities, canonical path identity otherwise. Use it instead of path equality.
  [[nodiscard]] static bool sameSourceIdentity(const QString& lhs, const QString& rhs);

  // Every load's object-ingest policy: scalars eager, objects lazy; video frames, scene entities and image
  // annotations pure-lazy (and robot descriptions in the browser). Static so it is testable on a bare resolver.
  static void applyDefaultIngestPolicies(PJ::sdk::ObjectIngestPolicyResolver& resolver);

 signals:
  /// A fan-out reload is about to tombstone `dataset_id`; emitted before its catalog item is hidden, so the shell can
  /// capture workspace state under `path` (on Replace, not the file the dataset came from) and rebind it after
  /// fileLoaded. Key the capture to `ticket`: a path match would let another same-path load's terminal disarm it.
  void sourceReplacementAboutToCommit(quint64 ticket, const QString& path, DatasetId dataset_id);

  // `plugin_id` is the plugin's display name, `plugin_manifest_id` its manifest id; layouts persist both. Silent on
  // a rejected config dialog, a queued cancel and shutdown: loadFinished is the terminal for every request.
  void fileLoaded(
      const QString& path, const QString& prefix, const QString& plugin_id, const QString& plugin_config_json,
      const QString& plugin_manifest_id);
  void fileLoadFailed(const QString& path, const QString& reason);

  // Ingest began (after the dialog). `file_index`/`file_total` are 1-based among queued loads (or fan-out entries);
  // `determinate` is false when the plugin gave no step total.
  void ingestStarted(const QString& title, int file_index, int file_total, bool determinate);
  // maximum 0: busy.
  void ingestProgress(int current, int maximum);
  // ingestProgress unnarrowed, with its request; total 0: unknown.
  void loadProgress(quint64 ticket, std::uint64_t current, std::uint64_t total);
  void queueDrained();
  // A queued load became current, before its dialog runs. UI bound to the previous generation (the stop dialog)
  // closes on this: fileLoaded/fileLoadFailed fire while the finished load is still current.
  void loadGenerationAdvanced(std::uint64_t generation);

  // Emitted right before loadGenerationAdvanced. Tickets are minted at enqueue, generations at dequeue.
  void requestStarted(quint64 ticket, std::uint64_t generation);

  // Exactly once per accepted request (see loadFileTicketed). `effective_path` is the source identity fileLoaded /
  // fileLoadFailed report; `produced_dataset_ids` every dataset holding this load's data; `dataset_id` the first of
  // them, or 0. `reason` is fileLoadFailed's reason on kFailed, empty otherwise: a caller that enqueued the load
  // reads it here, since a prologue failure on an idle loader emits fileLoadFailed before loadFileTicketed returns.
  void loadFinished(
      quint64 ticket, LoadOutcome outcome, const QString& effective_path, DatasetId dataset_id,
      const QVector<DatasetId>& produced_dataset_ids, const QString& reason);

  // Synchronous: the dataset, source path and captured config are installed, the catalog rebuild that publishes the
  // load has not run yet (then fileLoaded, then the queued loadFinished). Lets a promotion service attach source
  // records atomically with publication. Only for a single-instance load that completes; never for fan-out,
  // prefer_reuse, keep-stops, failures or discards.
  void loadCommitting(
      quint64 ticket, const QVector<DatasetId>& produced_ids, const QString& plugin_id,
      const QString& captured_config_json);

 private:
#ifdef PJ_TARGET_WASM
  // Must run in a user-activation turn: the browser refuses a picker opened later.
  void loadFromBrowserPicker(QWidget* dialog_parent, LoadHints hints);
#endif

  struct LoadRequest {
    LoadInput input;
    QPointer<QWidget> dialog_parent;
    LoadHints hints;
    int file_index = 1;
    int file_total = 1;
    quint64 ticket = 0;
  };
  struct LoadContext;
  // Owns the GUI prologue's coroutine frame, which holds the file lease, plugin handle, rollback state and dialog
  // inputs while the plugin dialog is open (no nested event loop).
  struct BeginLoadTask;

  // Erases a dataset an abandoned load created: TF first (invalidateDataset walks listTopics, which eviction
  // empties), then objects and the catalog item (no tombstone), then SessionManager::removeDataset, never the engine
  // directly, so the time origin is re-evaluated. remove_from_catalog=false for a dataset already tombstoned;
  // remove_dependent_processors=false keeps the processors reading it for a restore that rebinds them. Rebuilds the
  // catalog when a removed processor's output lived in another dataset.
  void removeCreatedDataset(
      DatasetId dataset_id, bool evict_objects = true, bool remove_from_catalog = true,
      bool remove_dependent_processors = true);

  // GUI thread: publishes the committed rows and TF, and advances the progress signals. `dataset_id` is the load's,
  // never the token's: a producer may tick without a start (the C ABI forwards the two independently), leaving the
  // token empty.
  void publishIngestProgress(DatasetId dataset_id, IngestToken token, uint64_t current, uint64_t total);

  struct PendingTerminal {
    quint64 ticket;
    LoadOutcome outcome;
    QString effective_path;
    DatasetId dataset_id;
    QVector<DatasetId> produced;
    QString reason;
  };

  // The only way to resolve a ticket. Every exit calls it before fileLoaded/fileLoadFailed, so a slot re-entering
  // cancelLoad/joinForShutdown finds the request resolved; a second resolution of the current ticket is ignored.
  // Queues the emission.
  void emitLoadFinished(
      quint64 ticket, LoadOutcome outcome, const QString& effective_path, DatasetId dataset_id,
      QVector<DatasetId> produced_dataset_ids = {}, QString reason = {});
  void deliverPendingTerminal();
  // Synchronous, FIFO: joinForShutdown only.
  void flushPendingTerminals();
  // Ends a fan-out entry's ingest from a queued call: an ingestEnded observer may call joinForShutdown, which
  // destroys the coroutine frame the fan-out loop runs in. Drop the token from fanout_ingest_tokens_ first, or
  // shutdown ends it a second time.
  void postIngestTerminal(IngestToken token, IngestOutcome outcome);

  // Emits queueDrained when the queue empties with no prologue or worker left.
  void startNext();
  // The GUI prologue: resolves the plugin, awaits its dialog without a nested event loop, then hands off to the
  // worker or finishes.
  [[nodiscard]] BeginLoadTask beginLoad(LoadRequest request);
  // Queues startNext rather than calling it: startNext destroys the frame this is called from.
  void finishPrologue();
  // Worker thread.
  void runIngestOnWorker(std::uint64_t generation);
  // Queued from the worker: joins it, commits or rolls back, then startNext().
  void onWorkerFinished(std::uint64_t generation);
  // fully_loaded=false (a keep-stop) commits the same way but skips loadCommitting. Resets ctx_.
  void finishLoadOnGui(bool fully_loaded);
  // Rolls the refill back (guard destructor), refreshes, emits fileLoadFailed. Resets ctx_.
  void failReplacingLoad(DatasetId dataset_id, const QString& path, const QString& reason);
  // After a RefillGuard rollback: rebuilds the catalog and drops the private TF generation.
  void refreshAfterReplacingRollback(DatasetId dataset_id);

  // Non-blocking. While its dialog is open, further failures append to it instead of stacking dialogs. A null
  // parent shows nothing and records nothing.
  void reportLoadWarning(QWidget* parent, const QString& reason);

  // Logs (`log_text`, else `reason`) and reports one data-source-load-failed. Not for a user cancel. GUI thread.
  void logLoadFailure(const QString& identity, const QString& reason, const QString& log_text = {}) const;

  SessionManager& session_;
  ExtensionCatalogService& extensions_;
  CatalogModel& catalog_;
  FilePicker file_picker_;
  BatchAsker batch_asker_;
  DiagnosticSink diagnostic_sink_;
  std::shared_ptr<BrowserFileStore> browser_file_store_;
  QHash<QString, QString> browser_content_sha256_;
  // Per source plugin, the files picked in its dialogs (a DBC, say). Its saved config names their staged paths and
  // pre-fills the next dialog, so they stay staged for the session.
  QHash<QString, std::vector<std::shared_ptr<void>>> last_dialog_files_;
  // Every staged browser upload's identity (loaded or picked in a plugin dialog), by backing path: a failed read
  // reports the path. Removed at its first failed read.
  QHash<QString, QString> browser_identity_by_path_;
#ifdef PJ_WITH_SCENE3D
  PJ::TransformService* transform_service_ = nullptr;
#endif

  std::deque<LoadRequest> queue_;
  std::unique_ptr<BeginLoadTask> begin_load_task_;
  std::unique_ptr<QThread> worker_;
  // The fan-out entry whose start() runs now: a frame local, reached here by cancelCurrent / joinForShutdown to wake
  // a progress-silent plugin. GUI thread only; the worker never reads it.
  DataSourceRuntimeHost* active_fanout_ingest_ = nullptr;
  // Removed as each entry ends; shutdown ends any survivor.
  std::unordered_map<DatasetId, IngestToken> fanout_ingest_tokens_;
  // The plugin question open now; shutdown rejects it before joining a worker that may wait on it. Shared: the
  // WA_DeleteOnClose dialog can outlive this loader, and its hooks capture this, never FileLoader.
  std::shared_ptr<QPointer<QDialog>> active_plugin_message_dialog_;
  // Shut by joinForShutdown so a worker parked in the message-box ABI cannot deadlock the join; then replaced.
  std::shared_ptr<PluginMessageGate> message_gate_;
  // The open load-warning dialog; cleared on finished. GUI thread only.
  QPointer<QDialog> active_load_warning_;
  QStringList load_warning_lines_;
  std::unique_ptr<LoadContext> ctx_;  // the single-instance load; null otherwise
  // A prologue or worker is processing a request; guards startNext re-entrancy. GUI thread only.
  bool active_load_ = false;
  // 0 while idle. Queued worker and progress callbacks compare it to drop stale calls. GUI thread only.
  std::uint64_t load_generation_ = 0;
  // Never reset, so a value is never reused after idle or shutdown and stale callbacks cannot match.
  std::uint64_t next_load_generation_ = 0;
  quint64 next_load_ticket_ = 0;  // never 0 once minted
  quint64 current_load_ticket_ = 0;
  // For the shutdown terminal: a prologue suspended at its dialog has no ctx_.
  QString current_load_identity_;
  // The current request's loadFinished is scheduled or delivered; cleared by startNext. Keeps a cancel in the hop
  // before startNext from latching cancel_mode_ against the next load.
  bool current_ticket_terminal_ = false;
  // Meaningful only while current_ticket_terminal_: a conflicting second resolution is logged.
  LoadOutcome current_ticket_outcome_ = LoadOutcome::kLoaded;
  std::deque<PendingTerminal> pending_terminals_;
  // Whole-load stop: 0 none, 1 keep, 2 discard. Written on the GUI thread (cancelCurrent, cancelLoad, joinForShutdown),
  // read by the worker. It stops the fan-out entry in flight and every one after it; a fan-out row's stop uses a
  // per-entry slot instead.
  std::atomic<int> cancel_mode_{0};
  // Keeps finishPrologue from advancing the queue while joinForShutdown destroys a suspended prologue.
  bool shutting_down_ = false;
  // Set by the destructor: its final terminal flush runs loadFinished slots, and a load accepted then could never be
  // resolved nor its worker joined, so loadFileTicketed returns 0.
  bool destroying_ = false;
};

}  // namespace PJ
