#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QByteArray>
#include <QDomDocument>
#include <QFileInfo>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "FileLoader.h"
#include "HeadlessDescriptorProviderSession.h"
#include "LayoutXml.h"
#include "pj_base/diagnostic_sink.hpp"
#include "pj_base/sdk/descriptor_import.hpp"
#include "pj_base/types.hpp"
#include "pj_runtime/SessionManager.h"

namespace PJ {

class CatalogModel;
class ExtensionCatalogService;
class SessionManager;
class SourceCaptureService;

// One layout-restore import transaction. For every <fileInfo> with a <materialize> record: validate the descriptor,
// consult the host source cache, query the provider (bounded, main thread), rewrite the document's <fileInfo
// filename> and every *_dataset_path qualifier to the effective local path, then classify: already loaded, hit (a
// stock ticketed load) or miss (a trust-gated provider import job). Miss jobs run one at a time through one
// HeadlessDescriptorProviderSession per provider, the next started from the previous job's on_terminal (legal: the
// job is concluded before that continuation runs).
//
// Interactivity is fixed at construction: continuations run long after the caller's stack unwound. A
// non-interactive batch never opens a dialog; untrusted origins and over-limit estimates fail with a diagnostic.
// Shell effects come in through Hooks so a test can drive the class without MainWindow. Stock loads go through
// FileLoader::loadFileTicketed and are tracked by their exactly-once loadFinished.
//
// GUI thread only; every child completion arrives queued on it.
//
// Lifecycle: prepare() -> [owner's reload prompt from pendingSourceLines()] -> start() -> finished(). cancel()
// cancels every child and, once all have concluded, rolls back: removes every dataset the batch produced, then
// restores the workspace checkpointed in start(). Destroying a running batch is a hard shutdown (children
// cancelled and joined, no rollback, no finished()), for app teardown or owner replacement.
class LayoutImportBatch : public QObject {
  Q_OBJECT
 public:
  enum class SourceOutcome {
    kResolvedAlreadyLoaded,
    kResolvedCacheHit,  ///< the stock load of an existing local file succeeded
    kResolvedImported,  ///< the provider import job succeeded (promoted or eager-only)
    kFailed,            ///< a diagnostic was emitted
    kSkippedUntrusted,  ///< "Skip these sources" at the trust prompt
    kCancelled,         ///< batch cancel, or a user stop of the stock load
  };

  struct SourceResult {
    QString effective_path;  ///< the local path planned; the saved path on a stock fallback
    SourceOutcome outcome = SourceOutcome::kFailed;
    QString message;
  };

  struct BatchResult {
    QList<SourceResult> sources;
    bool cancelled = false;
  };

  enum class TrustChoice { kTrustAndImport, kSkip, kCancelLayout };

  // All optional.
  struct Hooks {
    // Checkpoints the workspace and returns the closure that restores it exactly (false = not exactly). Called at
    // most once, from start(), before the first session mutation; an empty closure (or hook) disables the rollback.
    std::function<std::function<bool()>()> begin_workspace_checkpoint;
    std::function<void(DatasetId)> remove_dataset;
    // One prompt for every source that needs it; `lines` are display-ready. Never called on a non-interactive batch;
    // unset on an interactive one means kCancelLayout.
    std::function<TrustChoice(const QStringList& lines)> confirm_import;
    DiagnosticSink diagnostics;
  };

  // `max_transfer_bytes` (0 = none) is the per-machine transfer ceiling: a miss estimated above it needs the
  // interactive confirmation or fails; every import job also enforces it on the bytes actually transferred.
  // `capture_service` (nullable, desktop only) enables the cache-first restore: the host source cache is consulted
  // before any provider contact, and a hit loads the pinned artifact with no network, no trust prompt and no
  // provider plugin; an artifact that fails to load is quarantined and re-planned through the provider.
  LayoutImportBatch(
      SessionManager& session_manager, ExtensionCatalogService& extensions, FileLoader& loader, CatalogModel& catalog,
      SourceCaptureService* capture_service, bool interactive, std::uint64_t max_transfer_bytes, Hooks hooks,
      QObject* parent = nullptr);
  ~LayoutImportBatch() override;

  LayoutImportBatch(const LayoutImportBatch&) = delete;
  LayoutImportBatch& operator=(const LayoutImportBatch&) = delete;

  // Plans and classifies every entry of `sources` that has a <materialize> record (others stay the caller's) and
  // rewrites `doc` in place to the effective paths. A source whose descriptor is invalid or whose provider is
  // unavailable or cannot resolve it falls back to its saved path, with a diagnostic. Call once, before start();
  // `sources` must be extractDataSource() of `doc`. `doc` is kept (QDom handles are shared) for the corrective
  // rewrite after a cache artifact fails to load.
  void prepare(QDomDocument& doc, const QList<layout_xml::DataSourceRef>& sources);

  // Effective paths of the sources that still need a load or a job: the owner's reload-prompt lines.
  [[nodiscard]] QStringList pendingSourceLines() const;

  [[nodiscard]] bool hasPendingWork() const;

  enum class StartResult {
    kCancelledByUser,  ///< "Cancel layout load" at the trust prompt; nothing ran, no finished()
    kNoAsyncWork,      ///< nothing to launch, no finished(); the owner may discard the batch
    kRunning,          ///< wait for finished(), which follows even if everything failed synchronously
  };

  // The trust prompt, then (only after it) the workspace checkpoint, the stock loads and the first import job.
  [[nodiscard]] StartResult start();

  // Cancels and joins every import job, cancels every ticket, and once all have concluded rolls back, then emits
  // finished(). Idempotent; a batch that never started concludes immediately (still emitting finished()).
  void cancel();

  // Non-blocking, keep-partial cancel of only the active import job: it concludes kCancelled through its normal
  // terminal and the batch continues with the next job, with no rollback. No-op when no job is active.
  void cancelActiveImportJob();

  [[nodiscard]] bool isFinished() const noexcept {
    return state_ == State::kFinished;
  }
  [[nodiscard]] bool isActive() const noexcept {
    return state_ == State::kRunning || state_ == State::kCancelling;
  }
  [[nodiscard]] bool interactive() const noexcept {
    return interactive_;
  }
  [[nodiscard]] const BatchResult& result() const noexcept {
    return result_;
  }

  // The dataset the active import job announced; empty while no job runs or before (or without) its on_dataset.
  // Cleared at that job's terminal, before the next job starts.
  [[nodiscard]] std::optional<DatasetId> activeImportDataset() const noexcept {
    return active_import_dataset_;
  }

 signals:
  // Exactly once, queued (the owner may be inside start() or cancel()), after every child concluded and any
  // rollback ran. Never after kNoAsyncWork or kCancelledByUser, nor from the destructor.
  void finished();

 private:
  enum class State { kIdle, kPrepared, kRunning, kCancelling, kFinished };
  // kDone: the verdict is in the parallel result_ slot.
  enum class Plan { kDone, kHit, kMiss };

  // planned_[i] pairs with result_.sources[i].
  struct PlannedSource {
    layout_xml::DataSourceRef ref;
    QString effective_path;
    QByteArray descriptor_utf8;
    Plan plan = Plan::kDone;
    // The provider's (or, for a host-cache hit, the host digest's) identity: the SourceRecord identity attached to
    // a hit's datasets.
    QString query_identity;
    // Interactive only: untrusted origin or over-ceiling estimate; proceeds only through the trust prompt.
    bool needs_confirmation = false;
    std::uint64_t estimated_bytes = 0;
    // Classified against the saved path with the layout's own hints; no SourceRecord is attached.
    bool provider_fallback = false;
    // effective_path is a pinned host-cache artifact. The pin (a SourceCacheStore::Pinned, type-erased so this
    // header builds where that type does not) keeps it from eviction until the produced datasets take their own
    // copy at loadCommitting (or at a keep-partial cancel).
    bool host_cache_hit = false;
    std::shared_ptr<void> host_cache_pin;
  };

  // One per live dataset, captured at prepare(); none when the catalog is empty, so a remembered but cleared source
  // reloads. Copies the SourceRecord: the already-loaded verdict needs per-dataset provenance, which the path-keyed
  // loadedSources() cannot give.
  struct LoadedCandidate {
    QString source_path;  ///< the raw tracked path, which binding compares
    QString path_key;     ///< "" when the tracked path does not resolve
    bool has_record = false;
    SourceRecord record;
  };

  void emitDiagnostic(DiagnosticLevel level, const char* id, const QString& message) const;
  void setResult(std::size_t index, SourceOutcome outcome, const QString& message = {});
  // Created once per provider; nullptr when creation failed (remembered, never retried).
  HeadlessDescriptorProviderSession* sessionFor(const QString& provider_id);
  // Records the document rewrites in the two remap accumulators, which prepare() applies after the loop.
  void planSource(
      const layout_xml::DataSourceRef& ref, std::size_t index, QHash<QString, QString>& fileinfo_remap,
      QHash<QString, QString>& qualifier_remap);
  // Query, rewrite, trust gate, classify. Null remaps record no rewrite: the post-quarantine re-plan runs after
  // prepare() and rewrites doc_ itself.
  void planViaProvider(
      std::size_t index, QHash<QString, QString>* fileinfo_remap, QHash<QString, QString>* qualifier_remap);
  // A rejected ticket of a host-cache hit goes to handleHostCacheLoadFailure; any other fails the source.
  void startHitLoad(std::size_t index);
  // Releases the pin, quarantines the artifact and re-plans this one source through the provider. A re-plan that
  // needs the trust prompt fails instead: the batch's one prompt has passed. Outside kRunning it only concludes
  // the source cancelled, leaving the cache alone (the failure is the cancel's, not the artifact's).
  void handleHostCacheLoadFailure(std::size_t index);
  // The fileInfo filename remaps by its serialized value. A *_dataset_path qualifier may hold the serialized value,
  // the resolved path or its cleanPath form (what resolveDatasetSourcePaths writes), so all three are keyed (a key
  // equal to `target` is removed). They differ on Windows, where a drive-less "/saved/x" is not absolute.
  static void registerRewrite(
      const layout_xml::DataSourceRef& ref, const QString& target, QHash<QString, QString>& fileinfo_remap,
      QHash<QString, QString>& qualifier_remap);
  // Interactive: join the trust prompt. Non-interactive: fail with a diagnostic.
  void failOrConfirm(
      std::size_t index, const char* id, const QString& result_message, const QString& diagnostic_message);
  // Classifies against the saved path. `reason` (why the provider path was abandoned) goes into a failure result.
  void classifyFallback(std::size_t index, const QString& reason);
  // isSamePath as a key: literal for a browser upload, canonical path otherwise; empty = unresolvable, never a key.
  [[nodiscard]] static QString pathIdentityKey(const QString& path, const QFileInfo& info);
  // A candidate with a SourceRecord matches only on the same provider and descriptor bytes (identity is not
  // compared: records from the provider and the host-digest schemes name the same source), never by path, since the
  // cache slot may hold other content now; a candidate without one matches by path. The caller rewrites the
  // document to a record match's live path when the cache path moved.
  [[nodiscard]] const LoadedCandidate* findAlreadyLoaded(
      const PlannedSource& planned, const QString& effective_key) const;
  // Path only: a fallback has no provenance to compare.
  [[nodiscard]] bool pathAlreadyLoaded(const QString& effective_key) const;
  // Attaches the SourceRecord to a non-fallback hit's datasets atomically with catalog publication; without it the
  // next layout save would drop their <materialize> record.
  void onLoadCommitting(quint64 ticket, const QVector<DatasetId>& produced);
  void startNextJob();
  void onJobTerminal(std::size_t planned_index, DescriptorImportOutcome outcome, const QString& message);
  void onLoadFinished(quint64 ticket, LoadOutcome outcome, const QVector<DatasetId>& produced);
  void maybeFinish();

  SessionManager& session_manager_;
  ExtensionCatalogService& extensions_;
  FileLoader& loader_;
  CatalogModel& catalog_;
  SourceCaptureService* const capture_service_;  ///< nullable
  const bool interactive_;
  const std::uint64_t max_transfer_bytes_;
  Hooks hooks_;

  State state_ = State::kIdle;
  std::vector<PlannedSource> planned_;
  BatchResult result_;
  // Shares the caller's document (see prepare()).
  QDomDocument doc_;

  std::vector<LoadedCandidate> loaded_candidates_;

  // start() releases the sessions no queued job names, which would otherwise pin their plugin for the restore.
  std::map<QString, std::unique_ptr<HeadlessDescriptorProviderSession>> sessions_;
  QHash<QString, QString> failed_providers_;  ///< provider id -> creation error

  QHash<quint64, std::size_t> pending_tickets_;  ///< FileLoader ticket -> planned_ index
  std::vector<std::size_t> job_queue_;           ///< planned_ indices awaiting an import job
  std::size_t job_cursor_ = 0;                   ///< next job_queue_ entry to start
  bool job_active_ = false;
  // The active job (at most one runs); cleared together at its terminal, before the next job starts.
  HeadlessDescriptorProviderSession* active_job_session_ = nullptr;
  HeadlessDescriptorProviderSession::JobId active_job_id_ = 0;
  std::optional<DatasetId> active_import_dataset_;
  std::function<bool()> rollback_workspace_;
  std::vector<DatasetId> produced_datasets_;
  QMetaObject::Connection load_finished_conn_;
  QMetaObject::Connection load_committing_conn_;
};

}  // namespace PJ
