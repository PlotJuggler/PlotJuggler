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

// Owner of ONE layout-restore import transaction (spec §6.2): for every
// <fileInfo> that carries a <materialize> record, it runs the
// rewrite-then-classify pipeline — validate descriptor -> provider query
// (strictly bounded, main-thread) -> resolve the effective local path ->
// rewrite the working document's <fileInfo filename> -> remap every
// *_dataset_path qualifier -> classify (already-loaded / cache hit -> stock
// ticketed load / miss -> trust-gated provider import job). Miss jobs run
// SEQUENTIALLY (v1) through one HeadlessDescriptorProviderSession per
// provider, chained via on_terminal re-entrancy (legal per that class's
// contract: a job is concluded before its terminal continuation runs).
//
// The batch owns the interactive-vs-non-interactive policy for its whole
// lifetime (D5): interactivity is CONSTRUCTION-TIME and immutable, because
// late continuations run long after the caller's stack (and any
// startup-flag) has unwound. Non-interactive batches never open a dialog:
// kNeedsConfirmation descriptors and over-limit estimates fail with a
// diagnostic instead of prompting.
//
// Deliberately MainWindow-free: the shell-owned effects are injected as a
// small Hooks struct of std::function seams (workspace checkpoint/rollback,
// dataset removal, the consolidated trust prompt, diagnostics), so the
// class is drivable headlessly by a standalone test. (Interactivity is a
// plain bool for the same reason — MainWindow's LayoutLoadInteractivity
// enum lives on the shell side.) FileLoader is taken directly — stock
// cache-hit loads go through loadFileTicketed and are tracked by their
// exactly-once loadFinished terminals, never through the untracked
// loadFile()/queueDrained surface.
//
// Threading: GUI-thread only. All child completions (provider job
// callbacks, FileLoader terminals) arrive queued on the GUI thread.
//
// Lifecycle: prepare(doc, sources) -> [owner shows its reload prompt using
// pendingSourceLines()] -> start() -> ... -> finished() (emitted exactly
// once, queued, after every import job concluded AND every FileLoader
// ticket terminalized). cancel() cancels all children and, once they have
// all concluded, ROLLS BACK: every dataset the batch produced is removed
// and the pre-batch workspace (checkpointed at start(), before the first
// mutation) is restored exactly. Destroying a still-running batch is a
// HARD shutdown (children cancelled and joined, no rollback, no signal) —
// reserved for app teardown / owner replacement.
class LayoutImportBatch : public QObject {
  Q_OBJECT
 public:
  enum class SourceOutcome {
    kResolvedAlreadyLoaded,  ///< effective path already loaded (catalog non-empty)
    kResolvedCacheHit,       ///< effective path existed; stock ticketed load succeeded
    kResolvedImported,       ///< provider import job succeeded (promoted or eager-only)
    kFailed,                 ///< validation / trust / limit / load / job failure (diagnostic emitted)
    kSkippedUntrusted,       ///< user chose "Skip these sources" at the trust prompt
    kCancelled,              ///< batch cancel (or a user stop of the stock load)
  };

  struct SourceResult {
    QString effective_path;  ///< provider-planned local path (== saved on stock fallback)
    SourceOutcome outcome = SourceOutcome::kFailed;
    QString message;
  };

  struct BatchResult {
    QList<SourceResult> sources;
    bool cancelled = false;
  };

  // The consolidated trust/size confirmation (template: the 3-way layout
  // reload prompt): Trust and import / Skip these sources / Cancel layout load.
  enum class TrustChoice { kTrustAndImport, kSkip, kCancelLayout };

  // Injected shell effects. All optional (empty = no-op / non-interactive
  // fail); `confirm_import` is only ever invoked on an INTERACTIVE batch.
  struct Hooks {
    // Checkpoint the pre-batch workspace and return the closure that
    // exact-restores it (false = the restore could not be applied exactly).
    // Called at most once, from start(), before the batch's first session
    // mutation; an empty returned closure disables the rollback.
    std::function<std::function<bool()>()> begin_workspace_checkpoint;
    // Remove one batch-produced dataset (the un-import primitive).
    std::function<void(DatasetId)> remove_dataset;
    // ONE consolidated confirmation for every source needing it. `lines` is
    // display-ready (path + size estimate). Never called when !interactive().
    std::function<TrustChoice(const QStringList& lines)> confirm_import;
    DiagnosticSink diagnostics;
  };

  // `max_transfer_bytes` is the §7 guard-3 per-machine ceiling (0 = no
  // ceiling): a miss whose estimated_bytes exceeds it needs interactive
  // confirmation, or fails outright when non-interactive; the value is also
  // passed to every import job as its enforcement channel.
  // `capture_service` (nullable; desktop only) enables the M3 cache-first
  // restore: prepare() consults the host source cache BEFORE any provider
  // contact, a hit loads the pinned artifact through the stock loader with no
  // network, no trust prompt and no provider plugin required, and an artifact
  // whose load fails is quarantined and re-planned through the provider.
  LayoutImportBatch(
      SessionManager& session_manager, ExtensionCatalogService& extensions, FileLoader& loader, CatalogModel& catalog,
      SourceCaptureService* capture_service, bool interactive, std::uint64_t max_transfer_bytes, Hooks hooks,
      QObject* parent = nullptr);
  ~LayoutImportBatch() override;

  LayoutImportBatch(const LayoutImportBatch&) = delete;
  LayoutImportBatch& operator=(const LayoutImportBatch&) = delete;

  // Phase 1 (synchronous): the §6.2 steps 1-6 for every materialize-bearing
  // entry of `sources` (non-materialize entries are NEVER touched — they
  // remain the caller's), rewriting `doc` in place (fileInfo filename +
  // dataset-path qualifiers to each provider's effective path). Classifies
  // each processed source; sources that fail validation / provider lookup /
  // query degrade to the §6.4 stock fallback against the SAVED path (with a
  // distinct diagnostic each). Call exactly once, before start().
  // `sources` must be extractDataSource(doc, ...) of the same document, in
  // document order.
  void prepare(QDomDocument& doc, const QList<layout_xml::DataSourceRef>& sources);

  // Effective paths of the sources that still need work (stock loads +
  // import jobs) — the owner's reload-prompt lines. Valid after prepare().
  [[nodiscard]] QStringList pendingSourceLines() const;

  // True when start() would launch at least one stock load or import job.
  [[nodiscard]] bool hasPendingWork() const;

  enum class StartResult {
    kCancelledByUser,  ///< the user cancelled the whole layout load at the trust prompt
    kNoAsyncWork,      ///< nothing to launch; the owner may discard the batch
    kRunning,          ///< children launched; wait for finished()
  };

  // Phase 2: the trust gate (ONE consolidated prompt when interactive;
  // fail-with-diagnostic when not), then — strictly after the gate — the
  // workspace checkpoint, the stock ticketed loads, and the first import
  // job. Nothing is enqueued when the user cancels the layout load.
  [[nodiscard]] StartResult start();

  // Cancels every child (best-effort session cancelAll + joinAll, ticket
  // cancels) and finalizes once they have ALL concluded: rollback (produced
  // datasets removed, prior workspace restored), then finished(). Safe to
  // call repeatedly; a batch that never started concludes immediately.
  void cancel();

  // Nonblocking, keep-partial cancel of ONLY the active import job (D3): the
  // job concludes kCancelled through its normal terminal and the batch
  // CONTINUES with the next child — no rollback, every dataset produced so
  // far stays. No-op when no import job is active. cancel() above remains
  // the whole-batch rollback path.
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

  // The dataset the ACTIVE import job announced (the T7 correlation
  // surface): empty while no job runs, before the job's zero-or-one
  // on_dataset arrives, and for jobs that never announce one. Cleared at
  // that job's terminal, strictly before the next job starts.
  [[nodiscard]] std::optional<DatasetId> activeImportDataset() const noexcept {
    return active_import_dataset_;
  }

 signals:
  // Exactly once, queued, after all children concluded (and, for a
  // cancelled batch, after the rollback ran). Not emitted by a batch that
  // reported kNoAsyncWork, nor by the hard-shutdown destructor.
  void finished();

 private:
  enum class State { kIdle, kPrepared, kRunning, kCancelling, kFinished };
  // Only pending work needs a live plan; every terminal classification is
  // kDone with the verdict recorded in the parallel result_ slot.
  enum class Plan { kDone, kHit, kMiss };

  // planned_[i] pairs with result_.sources[i] — prepare() pushes both in
  // lockstep, so the vector index IS the result index.
  struct PlannedSource {
    layout_xml::DataSourceRef ref;
    QString effective_path;
    // The descriptor encoded ONCE (feeds validation, the provider query,
    // and the import start request).
    QByteArray descriptor_utf8;
    Plan plan = Plan::kDone;
    // The provider's durable identity from THIS query — the record's key
    // half for both already-loaded matching and the hit-ticket attach.
    QString query_identity;
    // Interactive-only: the source may proceed only through the consolidated
    // confirmation (untrusted origin and/or over-ceiling estimate).
    bool needs_confirmation = false;
    std::uint64_t estimated_bytes = 0;
    // §6.4 stock degrade (provider absent / invalid descriptor / failed
    // query): classify against the saved path with the layout's own hints.
    bool provider_fallback = false;
    // M3 host-cache hit: effective_path is the pinned cache artifact, loaded
    // with no provider contact. The pin (a SourceCacheStore::Pinned behind an
    // opaque pointer, so this header builds where that type does not) keeps
    // the artifact un-evictable until the produced datasets take their own
    // copy at loadCommitting; a failed load quarantines and re-plans through
    // the provider (handleHostCacheLoadFailure).
    bool host_cache_hit = false;
    std::shared_ptr<void> host_cache_pin;
  };

  // One entry per LIVE dataset, captured at prepare(): the identity key of
  // its tracked source path (one canonicalization per dataset instead of two
  // per comparison) plus a COPY of its SourceRecord when one is attached —
  // record-aware already-loaded classification needs dataset-level
  // provenance, which the path-keyed loadedSources() registry cannot carry.
  // Empty when the catalog has no data, so a remembered-but-cleared source
  // still reloads.
  struct LoadedCandidate {
    QString source_path;  ///< the dataset's RAW tracked path (what binding compares)
    QString path_key;     ///< "" when the tracked path does not resolve
    bool has_record = false;
    SourceRecord record;
  };

  void emitDiagnostic(DiagnosticLevel level, const char* id, const QString& message) const;
  void setResult(std::size_t index, SourceOutcome outcome, const QString& message = {});
  // Lazily create (once per provider id) the headless provider session.
  // Returns nullptr — remembering the failure — when creation fails.
  HeadlessDescriptorProviderSession* sessionFor(const QString& provider_id);
  // §6.2 steps 1-3 (+ trust/limit gating) for planned_[index], recording the
  // document rewrites in the two remap accumulators for the post-loop passes.
  // Consults the host source cache (capture_service_) BEFORE the provider.
  void planSource(
      const layout_xml::DataSourceRef& ref, std::size_t index, QHash<QString, QString>& fileinfo_remap,
      QHash<QString, QString>& qualifier_remap);
  // The provider half of planSource (§6.2 steps 2-7: query, rewrite, trust,
  // classify). Null remap maps skip the document rewrite — the post-quarantine
  // re-plan runs after prepare() consumed the document, so its rewrites can no
  // longer land (binding then degrades to the record/topic tiers).
  void planViaProvider(
      std::size_t index, QHash<QString, QString>* fileinfo_remap, QHash<QString, QString>* qualifier_remap);
  // Launches the stock ticketed load of a Plan::kHit source (start(), and the
  // post-quarantine re-plan). On a rejected ticket: host-cache hits route to
  // handleHostCacheLoadFailure, everything else fails the source.
  void startHitLoad(std::size_t index);
  // A host-cache artifact failed to load: release the pin, quarantine the
  // artifact (the service's resolve rules) and fall back to the live provider
  // by re-planning this ONE source through planViaProvider. A re-plan that
  // needs the interactive trust confirmation fails instead — the batch's one
  // consolidated prompt has already passed.
  void handleHostCacheLoadFailure(std::size_t index);
  // Records the steps-4/5 document rewrite for `ref` -> `target`. The
  // fileInfo filename remaps by its SERIALIZED value; the *_dataset_path
  // qualifiers may hold ANY of three forms of the saved path — the
  // serialized value exactly as written, the resolved absolute, or the
  // cleanPath'd resolved form MainWindow's resolveDatasetSourcePaths
  // pre-pass rewrites them to before prepare() runs — so ALL are keyed
  // (insert, or remove when the key already equals the target). Keying the
  // resolved form alone broke on Windows: a drive-less saved path like
  // "/saved/x" is NOT absolute there (QFileInfo::isAbsolute needs a
  // drive/UNC), so its resolved form never matches the document's
  // serialized value; on Linux the two coincide, which hid the miss.
  static void registerRewrite(
      const layout_xml::DataSourceRef& ref, const QString& target, QHash<QString, QString>& fileinfo_remap,
      QHash<QString, QString>& qualifier_remap);
  // The two gate refusals share this: interactive -> join the consolidated
  // confirmation; non-interactive -> fail with a diagnostic (D5).
  void failOrConfirm(
      std::size_t index, const char* id, const QString& result_message, const QString& diagnostic_message);
  // §6.4: classify planned_[index] against its SAVED path for the stock loader.
  void classifyFallback(std::size_t index);
  // The comparable identity of one source path: literal for opaque browser
  // uploads, canonical filesystem identity otherwise (empty = unresolvable,
  // never a valid key) — isSamePath semantics folded into one
  // canonicalization per side. `info` shares the caller's cached stat.
  [[nodiscard]] static QString pathIdentityKey(const QString& path, const QFileInfo& info);
  // Record-aware already-loaded verdict over loaded_candidates_ (§10:
  // "dataset already loaded with a matching source record"): a candidate
  // WITH a SourceRecord matches only on the exact {provider, query
  // source_identity, descriptor bytes} triple — a record naming different
  // provenance never matches by path (the cache slot may have been reused);
  // a record-LESS candidate matches by path identity alone.
  // Returns the matched candidate (nullptr = not loaded). The caller needs
  // the candidate itself for a RECORD match: its live tracked path is what
  // the document must be rewritten to when the cache path moved.
  [[nodiscard]] const LoadedCandidate* findAlreadyLoaded(
      const PlannedSource& planned, const QString& effective_key) const;
  // Raw §6.4 path-identity membership (no provenance available on the
  // fallback path) — classifyFallback only.
  [[nodiscard]] bool pathAlreadyLoaded(const QString& effective_key) const;
  // loadCommitting seam for the batch's HIT tickets: attach the provider
  // provenance atomically with catalog publication, the same pattern miss
  // promotion uses — or the next layout save loses the <materialize> record.
  void onLoadCommitting(quint64 ticket, const QVector<DatasetId>& produced);
  void startNextJob();
  void onJobTerminal(std::size_t planned_index, DescriptorImportOutcome outcome, const QString& message);
  void onLoadFinished(quint64 ticket, LoadOutcome outcome, const QVector<DatasetId>& produced);
  void maybeFinish();

  SessionManager& session_manager_;
  ExtensionCatalogService& extensions_;
  FileLoader& loader_;
  CatalogModel& catalog_;
  SourceCaptureService* const capture_service_;  ///< nullable: no host cache
  const bool interactive_;
  const std::uint64_t max_transfer_bytes_;
  Hooks hooks_;

  State state_ = State::kIdle;
  std::vector<PlannedSource> planned_;
  BatchResult result_;
  // Shared handle onto the caller's layout document (set by prepare) so the
  // post-quarantine provider fallback can apply its corrective path remap
  // through the same mechanism as prepare()'s two passes.
  QDomDocument doc_;

  std::vector<LoadedCandidate> loaded_candidates_;

  // One bound provider session per provider manifest id (created lazily at
  // the first source naming it); failed creations are remembered so each
  // later source degrades without re-attempting the bind. start() releases
  // the sessions no queued import job names — keeping them would pin their
  // DSO + plugin instance for the whole restore with nothing to run.
  std::map<QString, std::unique_ptr<HeadlessDescriptorProviderSession>> sessions_;
  QHash<QString, QString> failed_providers_;  ///< provider id -> creation error

  QHash<quint64, std::size_t> pending_tickets_;  ///< FileLoader ticket -> planned_ index
  std::vector<std::size_t> job_queue_;           ///< planned_ indices awaiting an import job
  std::size_t job_cursor_ = 0;                   ///< next job_queue_ entry to start
  bool job_active_ = false;
  // The active import job's cancel address on its owning session, and the
  // dataset its on_dataset announced (if any) — the T7 correlation state.
  // Set as the job starts/announces; all cleared together at its terminal,
  // before the startNextJob() chain (sequential v1: at most one job runs).
  HeadlessDescriptorProviderSession* active_job_session_ = nullptr;
  HeadlessDescriptorProviderSession::JobId active_job_id_ = 0;
  std::optional<DatasetId> active_import_dataset_;
  // The exact-restore closure from begin_workspace_checkpoint (empty until
  // start() reaches the checkpoint, or when the hook is unset).
  std::function<bool()> rollback_workspace_;
  std::vector<DatasetId> produced_datasets_;
  QMetaObject::Connection load_finished_conn_;
  QMetaObject::Connection load_committing_conn_;
};

}  // namespace PJ
