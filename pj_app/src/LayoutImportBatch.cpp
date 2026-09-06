// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "LayoutImportBatch.h"

#include <QDir>
#include <QJsonDocument>
#include <QLocale>
#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

#include "HeadlessDescriptorProviderSession.h"
#include "LayoutReplayHints.h"
#include "LoadInput.h"
#include "ToolboxHostWiring.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"
#ifndef __EMSCRIPTEN__
#include "pj_runtime/SourceCaptureService.h"
#endif
using namespace Qt::StringLiterals;

namespace PJ {

LayoutImportBatch::LayoutImportBatch(
    SessionManager& session_manager, ExtensionCatalogService& extensions, FileLoader& loader, CatalogModel& catalog,
    SourceCaptureService* capture_service, bool interactive, std::uint64_t max_transfer_bytes, Hooks hooks,
    QObject* parent)
    : QObject(parent),
      session_manager_(session_manager),
      extensions_(extensions),
      loader_(loader),
      catalog_(catalog),
      capture_service_(capture_service),
      interactive_(interactive),
      max_transfer_bytes_(max_transfer_bytes),
      hooks_(std::move(hooks)) {}

LayoutImportBatch::~LayoutImportBatch() {
  // HARD shutdown for a still-running batch (app teardown / owner
  // replacement): stop listening, discard outstanding tickets, and let each
  // session's destructor cancel+join+destroy its jobs (T5b's quiescence
  // guarantee). No rollback and no finished() — the graceful path is cancel().
  QObject::disconnect(load_finished_conn_);
  QObject::disconnect(load_committing_conn_);
  for (auto it = pending_tickets_.cbegin(); it != pending_tickets_.cend(); ++it) {
    static_cast<void>(loader_.cancelLoad(it.key(), /*keep_partial=*/false));
  }
  sessions_.clear();
}

void LayoutImportBatch::emitDiagnostic(DiagnosticLevel level, const char* id, const QString& message) const {
  emitDiagnosticTo(hooks_.diagnostics, level, "Layout", id, message.toStdString());
}

void LayoutImportBatch::setResult(std::size_t index, SourceOutcome outcome, const QString& message) {
  SourceResult& slot = result_.sources[static_cast<qsizetype>(index)];
  slot.effective_path = planned_[index].effective_path;
  slot.outcome = outcome;
  slot.message = message;
}

HeadlessDescriptorProviderSession* LayoutImportBatch::sessionFor(const QString& provider_id) {
  if (const auto it = sessions_.find(provider_id); it != sessions_.end()) {
    return it->second.get();
  }
  if (failed_providers_.contains(provider_id)) {
    return nullptr;
  }
  auto created = HeadlessDescriptorProviderSession::create(
      session_manager_, extensions_, loader_, catalog_, provider_id, hooks_.diagnostics, capture_service_);
  if (!created.has_value()) {
    failed_providers_.insert(provider_id, QString::fromStdString(created.error()));
    return nullptr;
  }
  HeadlessDescriptorProviderSession* session = created->get();
  sessions_.emplace(provider_id, std::move(*created));
  return session;
}

QString LayoutImportBatch::pathIdentityKey(const QString& path, const QFileInfo& info) {
  // Mirrors layout_xml::isSamePath: literal identity for opaque browser
  // uploads, canonical identity (must resolve) for filesystem paths.
  if (path.isEmpty()) {
    return {};
  }
  if (isBrowserUploadIdentity(path)) {
    return path;
  }
  return info.canonicalFilePath();
}

const LayoutImportBatch::LoadedCandidate* LayoutImportBatch::findAlreadyLoaded(
    const PlannedSource& planned, const QString& effective_key) const {
  for (const LoadedCandidate& candidate : loaded_candidates_) {
    if (candidate.has_record) {
      // Exact provenance is the verdict for record-carrying datasets:
      // provider + the FULL descriptor bytes. Identity is only a fast-path
      // key and is deliberately NOT required to agree — records minted under
      // the provider-identity scheme and the host digest scheme name the same
      // source when their descriptor bytes match byte-for-byte. Never fall
      // back to path identity for these — the cache slot may have been
      // reused for different content.
      if (candidate.record.provider_id == planned.ref.materialize_provider &&
          candidate.record.descriptor_json == planned.ref.materialize_descriptor_json) {
        return &candidate;
      }
      continue;
    }
    // Record-less candidate (e.g. the user opened the cache file directly):
    // path identity is all it has.
    if (!effective_key.isEmpty() && candidate.path_key == effective_key) {
      return &candidate;
    }
  }
  return nullptr;
}

bool LayoutImportBatch::pathAlreadyLoaded(const QString& effective_key) const {
  return !effective_key.isEmpty() &&
         std::any_of(loaded_candidates_.begin(), loaded_candidates_.end(), [&effective_key](const LoadedCandidate& c) {
           return c.path_key == effective_key;
         });
}

void LayoutImportBatch::classifyFallback(std::size_t index) {
  // §6.4 stock degrade: no provider answer, so the SAVED path is the only
  // hint. If it exists the stock load proceeds (the strongest degraded
  // mode); a missing file is today's data-source-missing outcome.
  PlannedSource& planned = planned_[index];
  planned.provider_fallback = true;
  planned.effective_path = planned.ref.resolved_path;
  const QFileInfo info(planned.effective_path);
  if (pathAlreadyLoaded(pathIdentityKey(planned.effective_path, info))) {
    setResult(index, SourceOutcome::kResolvedAlreadyLoaded);
    return;
  }
  if (info.exists()) {
    planned.plan = Plan::kHit;
    return;
  }
  setResult(index, SourceOutcome::kFailed, tr("data source does not exist on disk"));
  emitDiagnostic(
      DiagnosticLevel::kWarning, "layout-import-source-missing",
      tr("Layout's data source '%1' does not exist on disk and no provider could re-obtain it.")
          .arg(planned.effective_path));
}

void LayoutImportBatch::failOrConfirm(
    std::size_t index, const char* id, const QString& result_message, const QString& diagnostic_message) {
  PlannedSource& planned = planned_[index];
  if (interactive_) {
    planned.needs_confirmation = true;
    return;
  }
  planned.plan = Plan::kDone;
  setResult(index, SourceOutcome::kFailed, result_message);
  emitDiagnostic(DiagnosticLevel::kWarning, id, diagnostic_message);
}

void LayoutImportBatch::registerRewrite(
    const layout_xml::DataSourceRef& ref, const QString& target, QHash<QString, QString>& fileinfo_remap,
    QHash<QString, QString>& qualifier_remap) {
  fileinfo_remap.insert(ref.serialized_path, target);
  const QString keys[] = {ref.serialized_path, ref.resolved_path, QDir::cleanPath(ref.resolved_path)};
  for (const QString& key : keys) {
    if (key.isEmpty()) {
      continue;
    }
    if (key == target) {
      // Identity mapping (or a stale entry from an earlier registration for
      // this source, e.g. the record-matched live-path override): drop it.
      qualifier_remap.remove(key);
    } else {
      qualifier_remap.insert(key, target);
    }
  }
}

void LayoutImportBatch::planSource(
    const layout_xml::DataSourceRef& ref, std::size_t index, QHash<QString, QString>& fileinfo_remap,
    QHash<QString, QString>& qualifier_remap) {
  PlannedSource& planned = planned_[index];
  planned.ref = ref;
  planned.descriptor_utf8 = ref.materialize_descriptor_json.toUtf8();  // the ONE encode

  // Step 1: validate the descriptor (parseable, non-empty, identity present).
  const bool descriptor_valid = !ref.materialize_provider.isEmpty() && !ref.materialize_identity.isEmpty() &&
                                !planned.descriptor_utf8.trimmed().isEmpty() &&
                                !QJsonDocument::fromJson(planned.descriptor_utf8).isNull();
  if (!descriptor_valid) {
    emitDiagnostic(
        DiagnosticLevel::kWarning, "layout-import-descriptor-invalid",
        tr("Layout source '%1' carries an invalid <materialize> record; falling back to the saved path.")
            .arg(ref.resolved_path));
    classifyFallback(index);
    return;
  }

  // Step 2a (M3): the HOST source cache, BEFORE any provider contact. A hit
  // is a stock load of the pinned artifact — no network, no trust prompt, and
  // the provider plugin need not be installed. resolve() already verified the
  // artifact strictly against its embedded pj.capture manifest (and
  // quarantined it otherwise), so a hit here is load-ready.
#ifndef __EMSCRIPTEN__
  if (capture_service_ != nullptr) {
    const std::string provider_utf8 = ref.materialize_provider.toStdString();
    const std::string_view descriptor_view(
        planned.descriptor_utf8.constData(), static_cast<std::size_t>(planned.descriptor_utf8.size()));
    std::string miss;
    SourceCacheStore::MissKind miss_kind = SourceCacheStore::MissKind::kAbsent;
    if (auto resolved = capture_service_->resolve(provider_utf8, descriptor_view, &miss, &miss_kind)) {
      // The digest is the record identity every host path shares (attach on
      // capture, this hit's loadCommitting attach, already-loaded matching).
      planned.query_identity = QString::fromStdString(sourceCacheIdentityDigest(provider_utf8, descriptor_view));
      planned.effective_path = QString::fromStdU16String(resolved->artifact.path.u16string());
      planned.host_cache_hit = true;
      planned.host_cache_pin = std::make_shared<SourceCacheStore::Pinned>(std::move(resolved->artifact));
      registerRewrite(ref, planned.effective_path, fileinfo_remap, qualifier_remap);
      const QFileInfo effective_info(planned.effective_path);
      if (const LoadedCandidate* match =
              findAlreadyLoaded(planned, pathIdentityKey(planned.effective_path, effective_info))) {
        if (match->has_record && !match->source_path.isEmpty() && match->source_path != planned.effective_path) {
          // Same live-path override as the provider flow: the document must
          // describe the session that is already loaded.
          planned.effective_path = match->source_path;
          registerRewrite(ref, planned.effective_path, fileinfo_remap, qualifier_remap);
        }
        planned.host_cache_hit = false;
        planned.host_cache_pin.reset();  // the loaded dataset holds its own pin (or none)
        setResult(index, SourceOutcome::kResolvedAlreadyLoaded);
        return;
      }
      planned.plan = Plan::kHit;
      return;
    }
    if (miss_kind == SourceCacheStore::MissKind::kContended) {
      // Another instance is mid-publish for this exact request. Falling
      // through to the provider would start a duplicate download of bytes
      // that are about to exist locally, so fail this ONE source instead
      // (deliberate design decision: no retry loop, no duplicate download).
      setResult(
          index, SourceOutcome::kFailed,
          tr("another PlotJuggler instance is currently downloading this request; wait for it to finish and load "
             "the layout again"));
      emitDiagnostic(
          DiagnosticLevel::kWarning, "layout-import-cache-contended",
          tr("Layout source '%1': another PlotJuggler instance is currently downloading this request; wait for it "
             "to finish and load the layout again.")
              .arg(ref.resolved_path));
      return;
    }
  }
#endif

  planViaProvider(index, &fileinfo_remap, &qualifier_remap);
}

void LayoutImportBatch::planViaProvider(
    std::size_t index, QHash<QString, QString>* fileinfo_remap, QHash<QString, QString>* qualifier_remap) {
  PlannedSource& planned = planned_[index];
  const layout_xml::DataSourceRef& ref = planned.ref;
  const auto record_rewrite = [&](const QString& target) {
    if (fileinfo_remap != nullptr && qualifier_remap != nullptr) {
      registerRewrite(ref, target, *fileinfo_remap, *qualifier_remap);
    }
  };

  // Step 2: the provider query (strictly bounded, main-thread).
  HeadlessDescriptorProviderSession* session = sessionFor(ref.materialize_provider);
  if (session == nullptr) {
    emitDiagnostic(
        DiagnosticLevel::kWarning, "layout-import-provider-unavailable",
        tr("Provider '%1' for layout source '%2' is unavailable (%3); falling back to the saved path.")
            .arg(ref.materialize_provider, ref.resolved_path, failed_providers_.value(ref.materialize_provider)));
    classifyFallback(index);
    return;
  }
  const auto query = session->queryDescriptor(
      std::string_view(planned.descriptor_utf8.constData(), static_cast<std::size_t>(planned.descriptor_utf8.size())));
  // A successful answer without a source identity is malformed (§6.3:
  // identity and local path are ALWAYS returned) — provenance keys the
  // rewrite and the record matching, so the source cannot be classified
  // safely. Fail closed, never degrade.
  if (query.has_value() && query->source_identity.empty()) {
    setResult(index, SourceOutcome::kFailed, tr("provider returned no source identity"));
    emitDiagnostic(
        DiagnosticLevel::kWarning, "layout-import-query-invalid",
        tr("Provider '%1' answered the query for layout source '%2' without a source identity.")
            .arg(ref.materialize_provider, ref.resolved_path));
    return;
  }
  if (!query.has_value() || query->local_path_utf8.empty()) {
    emitDiagnostic(
        DiagnosticLevel::kWarning, "layout-import-query-failed",
        tr("Provider '%1' could not resolve layout source '%2' (%3); falling back to the saved path.")
            .arg(
                ref.materialize_provider, ref.resolved_path,
                query.has_value() ? tr("no local path") : QString::fromStdString(query.error())));
    classifyFallback(index);
    return;
  }

  // Step 3: the effective local path (hit: cached; miss: the path
  // materialize will produce).
  planned.effective_path = QString::fromStdString(query->local_path_utf8);
  planned.estimated_bytes = query->estimated_bytes;

  // Steps 4+5: record the document rewrites — the document must describe the
  // session the restore will actually produce. Applied by the two remap
  // passes at the end of prepare(); fallback sources are absent, so their
  // elements stay untouched.
  record_rewrite(planned.effective_path);

  // Trust gate input. kRefused fails the source outright — even a local
  // cache hit: the provider actively rejected this descriptor (fail-closed).
  if (query->trust == DescriptorTrust::kRefused) {
    setResult(index, SourceOutcome::kFailed, tr("descriptor refused by provider"));
    emitDiagnostic(
        DiagnosticLevel::kWarning, "layout-import-refused",
        tr("Provider '%1' refused the import descriptor for '%2'%3")
            .arg(
                ref.materialize_provider, planned.effective_path,
                query->message.empty() ? u"."_s : u": %1"_s.arg(QString::fromStdString(query->message))));
    return;
  }

  // Step 7: classify. Provenance first (record-aware already-loaded, §10),
  // then the provider's is_materialized verdict as the AUTHORITATIVE
  // hit/miss call — raw existence classification lives only in the §6.4
  // fallback, so a stale/partial file at the cache path can never be
  // promoted to a hit against the provider's word.
  planned.query_identity = QString::fromStdString(query->source_identity);
  const QFileInfo effective_info(planned.effective_path);
  if (const LoadedCandidate* match =
          findAlreadyLoaded(planned, pathIdentityKey(planned.effective_path, effective_info))) {
    if (match->has_record && !match->source_path.isEmpty() && match->source_path != planned.effective_path) {
      // Codex r2-1: the provider's cache path MOVED since this dataset was
      // loaded. The document must describe the LIVE session — path-tier
      // curve/timeline binding compares against the matched dataset's
      // tracked path, and the freshly queried path names a file nothing in
      // the session was loaded from. Override the steps-4/5 rewrite with
      // the live path (record-matched candidates only; a record-less match
      // was found BY the effective path, so the two are already equal).
      planned.effective_path = match->source_path;
      record_rewrite(planned.effective_path);
    }
    setResult(index, SourceOutcome::kResolvedAlreadyLoaded);
    return;
  }
  if (query->is_materialized) {
    if (effective_info.exists()) {
      // A cache hit downloads nothing, so the trust gate (which guards the
      // NETWORK touch) does not apply — §10: cache file present -> no network.
      planned.plan = Plan::kHit;
      return;
    }
    // Materialized-but-missing is a provider bug: importing silently would
    // paper over an inconsistent cache verdict — fail loudly instead.
    setResult(index, SourceOutcome::kFailed, tr("provider cache verdict is inconsistent"));
    emitDiagnostic(
        DiagnosticLevel::kWarning, "layout-import-materialized-missing",
        tr("Provider '%1' reports layout source '%2' as materialized, but no file exists there.")
            .arg(ref.materialize_provider, planned.effective_path));
    return;
  }
  planned.plan = Plan::kMiss;
  if (query->trust == DescriptorTrust::kNeedsConfirmation) {
    failOrConfirm(
        index, "layout-import-untrusted", tr("untrusted import origin"),
        tr("Layout source '%1' requires confirmation of an untrusted origin; refused in non-interactive mode. "
           "Connect to this server once in the provider toolbox to trust it.")
            .arg(planned.effective_path));
  }
  // §7 guard 3: the estimate against the per-machine ceiling. 0 = unknown
  // estimate — enforcement then rides max_transfer_bytes on the job itself
  // (actual transferred bytes).
  if (planned.plan == Plan::kMiss && max_transfer_bytes_ > 0 && planned.estimated_bytes > max_transfer_bytes_) {
    failOrConfirm(
        index, "layout-import-size-limit", tr("estimated size exceeds the transfer limit"),
        tr("Layout source '%1' estimates %2 bytes, above the configured limit of %3 bytes; refused in "
           "non-interactive mode.")
            .arg(planned.effective_path)
            .arg(planned.estimated_bytes)
            .arg(max_transfer_bytes_));
  }
}

void LayoutImportBatch::prepare(QDomDocument& doc, const QList<layout_xml::DataSourceRef>& sources) {
  Q_ASSERT(state_ == State::kIdle);
  state_ = State::kPrepared;
  // QDom handles are explicitly shared: this copy addresses the caller's
  // document, letting the post-quarantine fallback re-run the same remap
  // passes prepare() applies below (handleHostCacheLoadFailure).
  doc_ = doc;

  // One candidate per LIVE dataset, up front: its path identity key (one
  // canonicalization per dataset, not two per comparison) plus a copy of its
  // SourceRecord — record-aware already-loaded classification needs
  // dataset-level provenance, which the path-keyed loadedSources() registry
  // cannot supply. Empty catalog -> no candidates: a remembered-but-cleared
  // source must reload.
  if (!catalog_.isEmpty()) {
    for (const auto& [dataset_id, name] : catalog_.datasets()) {
      static_cast<void>(name);
      LoadedCandidate candidate;
      candidate.source_path = loader_.sourcePathForDataset(dataset_id);
      candidate.path_key = pathIdentityKey(candidate.source_path, QFileInfo(candidate.source_path));
      if (const SourceRecord* record = session_manager_.sourceRecord(dataset_id)) {
        candidate.has_record = true;
        candidate.record = *record;
      }
      loaded_candidates_.push_back(std::move(candidate));
    }
  }

  QHash<QString, QString> fileinfo_remap;   // serialized fileInfo filename -> effective
  QHash<QString, QString> qualifier_remap;  // every saved-path form -> effective (see registerRewrite)
  for (const layout_xml::DataSourceRef& ref : sources) {
    if (!layout_xml::hasMaterializeRecord(ref)) {
      continue;  // never touch a plain source — it stays the caller's
    }
    result_.sources.push_back({});
    planned_.push_back({});
    planSource(ref, planned_.size() - 1, fileinfo_remap, qualifier_remap);
  }

  if (!fileinfo_remap.isEmpty()) {
    layout_xml::remapFileInfoFilenames(
        doc, [&fileinfo_remap](const QString& filename) { return fileinfo_remap.value(filename, filename); });
  }
  if (!qualifier_remap.isEmpty()) {
    layout_xml::remapDatasetSourcePaths(
        doc, [&qualifier_remap](const QString& saved_path) { return qualifier_remap.value(saved_path, saved_path); });
  }
}

QStringList LayoutImportBatch::pendingSourceLines() const {
  QStringList lines;
  for (const PlannedSource& planned : planned_) {
    if (planned.plan != Plan::kDone) {
      lines.push_back(planned.effective_path);
    }
  }
  return lines;
}

bool LayoutImportBatch::hasPendingWork() const {
  return std::any_of(
      planned_.begin(), planned_.end(), [](const PlannedSource& planned) { return planned.plan != Plan::kDone; });
}

LayoutImportBatch::StartResult LayoutImportBatch::start() {
  Q_ASSERT(state_ == State::kPrepared);

  // Trust/size gate — strictly BEFORE any child is enqueued, so a
  // "Cancel layout load" answer leaves the session untouched.
  QStringList confirm_lines;
  for (const PlannedSource& planned : planned_) {
    if (planned.plan == Plan::kMiss && planned.needs_confirmation) {
      confirm_lines.push_back(
          planned.estimated_bytes > 0
              ? tr("%1  (about %2)")
                    .arg(
                        planned.effective_path,
                        QLocale().formattedDataSize(static_cast<qint64>(planned.estimated_bytes)))
              : planned.effective_path);
    }
  }
  if (!confirm_lines.isEmpty()) {
    // needs_confirmation is only ever set on an interactive batch; the
    // non-interactive path already failed those sources during prepare().
    const TrustChoice choice =
        hooks_.confirm_import ? hooks_.confirm_import(confirm_lines) : TrustChoice::kCancelLayout;
    switch (choice) {
      case TrustChoice::kCancelLayout:
        state_ = State::kFinished;
        return StartResult::kCancelledByUser;
      case TrustChoice::kSkip:
        for (std::size_t index = 0; index < planned_.size(); ++index) {
          PlannedSource& planned = planned_[index];
          if (planned.plan == Plan::kMiss && planned.needs_confirmation) {
            planned.plan = Plan::kDone;
            setResult(index, SourceOutcome::kSkippedUntrusted);
          }
        }
        break;
      case TrustChoice::kTrustAndImport:
        break;
    }
  }

  if (!hasPendingWork()) {
    state_ = State::kFinished;
    return StartResult::kNoAsyncWork;
  }

  if (hooks_.begin_workspace_checkpoint) {
    rollback_workspace_ = hooks_.begin_workspace_checkpoint();
  }
  state_ = State::kRunning;

  load_finished_conn_ = connect(
      &loader_, &FileLoader::loadFinished, this,
      [this](
          quint64 ticket, LoadOutcome outcome, const QString& /*effective_path*/, DatasetId /*dataset_id*/,
          const QVector<DatasetId>& produced) { onLoadFinished(ticket, outcome, produced); });
  load_committing_conn_ = connect(
      &loader_, &FileLoader::loadCommitting, this,
      [this](
          quint64 ticket, const QVector<DatasetId>& produced, const QString& /*plugin_id*/,
          const QString& /*captured_config_json*/) { onLoadCommitting(ticket, produced); });

  for (std::size_t index = 0; index < planned_.size(); ++index) {
    PlannedSource& planned = planned_[index];
    if (planned.plan == Plan::kHit) {
      startHitLoad(index);
    } else if (planned.plan == Plan::kMiss) {
      job_queue_.push_back(index);
    }
  }

  // Release the provider sessions no queued job names: they were bound for
  // prepare()'s queries, and keeping them would pin their DSO + plugin
  // instance (and their FileLoader observers) for the whole restore with
  // nothing left to run.
  QSet<QString> needed_providers;
  for (const std::size_t index : job_queue_) {
    needed_providers.insert(planned_[index].ref.materialize_provider);
  }
  std::erase_if(sessions_, [&needed_providers](const auto& entry) { return !needed_providers.contains(entry.first); });

  startNextJob();
  // Everything may already have failed synchronously (every ticket rejected,
  // every job refused to start). maybeFinish() then finalizes via the queued
  // emission — the batch still reports kRunning, and finished() follows.
  maybeFinish();
  return StartResult::kRunning;
}

void LayoutImportBatch::startHitLoad(std::size_t index) {
  PlannedSource& planned = planned_[index];
  // §6.4 fallback hits mirror today's replay hints exactly (the saved
  // preset already names the saved path); provider-resolved hits ask the
  // loader to rewrite the preset's filepath to the effective path.
  // Host-cache hits get their OWN hints: a capture artifact is always a
  // standard MCAP, so the layout's original plugin identity/preset (which
  // may name a non-MCAP provider format) must not be required — a hint
  // mismatch would reject and then quarantine a perfectly valid artifact.
  // The loader picks the MCAP source by extension, dialog-free.
  LoadHints hints;
  if (planned.host_cache_hit) {
    hints.dialog_policy = DialogPolicy::kNever;
    hints.prefer_reuse = true;
  } else {
    const bool rewrite = planned.provider_fallback ? planned.ref.rewrite_plugin_filepath : true;
    hints = layoutReplayHints(planned.ref, /*prefer_reuse=*/true, rewrite);
  }
  const quint64 ticket = loader_.loadFileTicketed(LoadInput::fromNativePath(planned.effective_path), nullptr, hints);
  if (ticket == 0) {
    if (planned.host_cache_hit) {
      // The cached artifact is unloadable in this session: heal + provider.
      handleHostCacheLoadFailure(index);
      return;
    }
    planned.plan = Plan::kDone;
    setResult(index, SourceOutcome::kFailed, tr("the loader rejected the file"));
    emitDiagnostic(
        DiagnosticLevel::kWarning, "layout-import-load-rejected",
        tr("The loader rejected layout source '%1'.").arg(planned.effective_path));
    return;
  }
  pending_tickets_.insert(ticket, index);
}

void LayoutImportBatch::handleHostCacheLoadFailure(std::size_t index) {
  PlannedSource& planned = planned_[index];
  planned.plan = Plan::kDone;
  planned.host_cache_hit = false;
  planned.host_cache_pin.reset();  // release our pin so the quarantine can act
  // A batch that is no longer running must not re-plan: a job queued here
  // would never start (startNextJob refuses outside kRunning) and the batch
  // would hang short of maybeFinish's drained condition. It must not touch
  // the cache either — the load "failure" is a casualty of the cancel, not
  // evidence against the artifact.
  if (state_ != State::kRunning) {
    setResult(index, SourceOutcome::kCancelled);
    return;
  }
  const QString stale_path = planned.effective_path;
#ifndef __EMSCRIPTEN__
  const std::string provider_utf8 = planned.ref.materialize_provider.toStdString();
  const std::string_view descriptor_view(
      planned.descriptor_utf8.constData(), static_cast<std::size_t>(planned.descriptor_utf8.size()));
  std::string refusal;
  const bool healed = capture_service_ != nullptr && capture_service_->store().quarantine(
                                                         sourceCacheIdentity(provider_utf8, descriptor_view), &refusal);
  emitDiagnostic(
      DiagnosticLevel::kWarning, "layout-import-cache-load-failed",
      tr("Cached copy of layout source '%1' failed to load%2; falling back to the provider.")
          .arg(planned.effective_path, healed ? tr(" and was removed") : QString{}));
#endif
  // Fall back to the live provider for this ONE source.
  planViaProvider(index, nullptr, nullptr);
  // Corrective document rewrite: prepare() pointed the fileInfo filename and
  // every *_dataset_path qualifier at the (now quarantined) cache path, and
  // the path binding tier would otherwise never resolve against the fallback
  // load. QDom documents are explicitly shared, so doc_ IS the caller's
  // document; the remap uses the same two passes prepare() applied — a
  // qualifier holds exactly the stale cache path, nothing else.
  if (!doc_.isNull() && planned.effective_path != stale_path) {
    const auto remap = [&stale_path, &planned](const QString& value) {
      return value == stale_path ? planned.effective_path : value;
    };
    layout_xml::remapFileInfoFilenames(doc_, remap);
    layout_xml::remapDatasetSourcePaths(doc_, remap);
  }
  if (planned.plan == Plan::kHit) {
    startHitLoad(index);
  } else if (planned.plan == Plan::kMiss) {
    if (planned.needs_confirmation) {
      // The consolidated trust prompt has already run for this batch; a new
      // network touch cannot be confirmed now (D5) — fail, never prompt late.
      planned.plan = Plan::kDone;
      planned.needs_confirmation = false;
      setResult(index, SourceOutcome::kFailed, tr("untrusted import origin"));
      emitDiagnostic(
          DiagnosticLevel::kWarning, "layout-import-untrusted",
          tr("Layout source '%1' requires confirmation of an untrusted origin, which cannot be asked after the "
             "batch started. Connect to this server once in the provider toolbox to trust it.")
              .arg(planned.effective_path));
    } else {
      job_queue_.push_back(index);
      startNextJob();
    }
  }
}

void LayoutImportBatch::startNextJob() {
  if (state_ != State::kRunning || job_active_) {
    return;
  }
  while (job_cursor_ < job_queue_.size()) {
    const std::size_t index = job_queue_[job_cursor_++];
    PlannedSource& planned = planned_[index];
    HeadlessDescriptorProviderSession* session = sessionFor(planned.ref.materialize_provider);
    if (session == nullptr) {
      // The session existed at query time; losing it between prepare and
      // start is not expected, but fail the source rather than crash.
      setResult(index, SourceOutcome::kFailed, tr("provider session unavailable"));
      continue;
    }
    DescriptorImportStartRequest request;
    request.descriptor_json.assign(
        planned.descriptor_utf8.constData(), static_cast<std::size_t>(planned.descriptor_utf8.size()));
    request.flags = PJ_DESCRIPTOR_IMPORT_START_FLAG_NONE;
    request.max_transfer_bytes = max_transfer_bytes_;
    const auto started = session->startImport(
        request,
        [this](DatasetId dataset_id) {
          // The T7 correlation surface, alongside the rollback-ledger append:
          // sequential v1 runs at most one job, so the announcement is
          // unambiguously the active job's.
          active_import_dataset_ = dataset_id;
          produced_datasets_.push_back(dataset_id);
        },
        [this, index](DescriptorImportOutcome outcome, std::string message) {
          onJobTerminal(index, outcome, QString::fromStdString(message));
        });
    if (!started) {
      setResult(index, SourceOutcome::kFailed, QString::fromStdString(started.error()));
      emitDiagnostic(
          DiagnosticLevel::kWarning, "layout-import-job-start-failed",
          tr("Import of layout source '%1' could not start: %2")
              .arg(planned.effective_path, QString::fromStdString(started.error())));
      continue;
    }
    job_active_ = true;
    active_job_session_ = session;
    active_job_id_ = *started;
    return;
  }
}

void LayoutImportBatch::onJobTerminal(
    std::size_t planned_index, DescriptorImportOutcome outcome, const QString& message) {
  job_active_ = false;
  // Clear the T7 correlation BEFORE the startNextJob() chain below: the next
  // job must never inherit its predecessor's announced dataset or cancel
  // address.
  active_job_session_ = nullptr;
  active_job_id_ = 0;
  active_import_dataset_.reset();
  const QString& effective_path = planned_[planned_index].effective_path;
  switch (outcome) {
    case DescriptorImportOutcome::kSucceededPromoted:
      setResult(planned_index, SourceOutcome::kResolvedImported, message);
      break;
    case DescriptorImportOutcome::kSucceededEagerOnly:
      setResult(planned_index, SourceOutcome::kResolvedImported, message);
      // §10: the eager dataset is usable, but no import record is attached —
      // a re-saved layout will not carry a <materialize> for it.
      emitDiagnostic(
          DiagnosticLevel::kInfo, "layout-import-eager-only",
          tr("Layout source '%1' imported as data only (no re-importable source record was attached).")
              .arg(effective_path));
      break;
    case DescriptorImportOutcome::kCancelled:
      setResult(planned_index, SourceOutcome::kCancelled, message);
      if (state_ != State::kCancelling) {
        emitDiagnostic(
            DiagnosticLevel::kWarning, "layout-import-cancelled",
            tr("Import of layout source '%1' was cancelled.").arg(effective_path));
      }
      break;
    case DescriptorImportOutcome::kFailed:
      setResult(planned_index, SourceOutcome::kFailed, message);
      emitDiagnostic(
          DiagnosticLevel::kWarning, "layout-import-job-failed",
          tr("Import of layout source '%1' failed: %2").arg(effective_path, message));
      break;
  }
  // Sequential v1 chaining: the job was concluded before this continuation
  // ran (the session contract), so starting the next import from here is the
  // intended re-entrant loop.
  startNextJob();
  maybeFinish();
}

void LayoutImportBatch::onLoadCommitting(quint64 ticket, const QVector<DatasetId>& produced) {
  const auto it = pending_tickets_.constFind(ticket);
  if (it == pending_tickets_.constEnd()) {
    return;  // not one of ours
  }
  const PlannedSource& planned = planned_[it.value()];
  // Codex r2-2: a provider-resolved cache hit must carry the same provenance
  // a promoted miss gets — layout save emits <materialize> only for
  // record-bearing datasets, so without this the restored hit degrades to an
  // ordinary path source on the next save. Attached on the SYNCHRONOUS
  // pre-catalog seam, atomic with catalog publication (the promotion
  // pattern). §6.4 fallback hits have no provider answer — nothing to
  // attach. Seam limits (same as promotion): the prefer_reuse and fan-out
  // epilogues never run loadCommitting, so a reused/fan-out hit keeps
  // path-only identity.
  if (planned.provider_fallback) {
    return;
  }
  for (const DatasetId dataset_id : produced) {
    session_manager_.attachSourceRecord(
        dataset_id, SourceRecord{
                        .provider_id = planned.ref.materialize_provider,
                        .source_identity = planned.query_identity,
                        .descriptor_json = planned.ref.materialize_descriptor_json,
                    });
    if (planned.host_cache_pin != nullptr) {
      // The dataset now reads the cache artifact: it owns a read pin for its
      // lifetime (released by removeDataset/merge), so eviction and
      // quarantine cannot pull the file out from under it.
      session_manager_.pinDatasetResource(dataset_id, planned.host_cache_pin);
    }
  }
}

void LayoutImportBatch::onLoadFinished(quint64 ticket, LoadOutcome outcome, const QVector<DatasetId>& produced) {
  const auto it = pending_tickets_.constFind(ticket);
  if (it == pending_tickets_.constEnd()) {
    return;  // not one of ours
  }
  const std::size_t index = it.value();
  pending_tickets_.erase(it);
  for (const DatasetId produced_id : produced) {
    produced_datasets_.push_back(produced_id);
  }
  switch (outcome) {
    case LoadOutcome::kLoaded:
      // Host-cache hits: the produced datasets took their pin copies at
      // loadCommitting; the batch's own copy may now go.
      planned_[index].host_cache_pin.reset();
      setResult(index, SourceOutcome::kResolvedCacheHit);
      break;
    case LoadOutcome::kCancelled:
      // A keep-partial stop skipped loadCommitting, so any KEPT datasets never
      // took their pin there — hand it over here, or a surviving dataset with
      // lazy payloads would lose its backing artifact to cleanup/quarantine.
      // (A rolled-back batch removes these datasets, which releases the pins.)
      if (planned_[index].host_cache_pin != nullptr) {
        for (const DatasetId produced_id : produced) {
          session_manager_.pinDatasetResource(produced_id, planned_[index].host_cache_pin);
        }
      }
      planned_[index].host_cache_pin.reset();
      setResult(index, SourceOutcome::kCancelled);
      break;
    case LoadOutcome::kFailed:
      if (planned_[index].host_cache_hit) {
        // Strict rejection: a cached artifact whose stock load fails is never
        // a hit — quarantine it and fall back to the live provider.
        handleHostCacheLoadFailure(index);
        break;
      }
      setResult(index, SourceOutcome::kFailed, tr("the stock load failed"));
      emitDiagnostic(
          DiagnosticLevel::kWarning, "layout-import-load-failed",
          tr("Loading layout source '%1' failed.").arg(planned_[index].effective_path));
      break;
  }
  maybeFinish();
}

void LayoutImportBatch::cancelActiveImportJob() {
  // D3 keep-partial: forward to the session's per-job cancel and nothing
  // else. The job's kCancelled terminal arrives through the normal queued
  // path (onJobTerminal records the outcome and chains the next job) —
  // deliberately none of cancel()'s rollback machinery runs.
  if (!job_active_ || active_job_session_ == nullptr) {
    return;
  }
  active_job_session_->cancelJob(active_job_id_);
}

void LayoutImportBatch::cancel() {
  if (state_ == State::kFinished || state_ == State::kCancelling) {
    return;
  }
  result_.cancelled = true;
  if (state_ != State::kRunning) {
    // Never started: nothing to unwind. Conclude every source and finish.
    for (std::size_t index = 0; index < planned_.size(); ++index) {
      if (planned_[index].plan != Plan::kDone) {
        planned_[index].plan = Plan::kDone;
        setResult(index, SourceOutcome::kCancelled);
      }
    }
    state_ = State::kCancelling;
    maybeFinish();
    return;
  }
  state_ = State::kCancelling;
  // Cancel + join the provider side (terminals are now queued); the queued
  // continuations still deliver, so the produced-dataset ledger is complete
  // before maybeFinish() sees the last child conclude.
  for (const auto& [provider_id, session] : sessions_) {
    static_cast<void>(provider_id);
    session->cancelAll();
    session->joinAll();
  }
  // Discard outstanding stock loads; their kCancelled terminals conclude
  // the tickets through the normal queued path.
  const QList<quint64> tickets = pending_tickets_.keys();
  for (const quint64 ticket : tickets) {
    static_cast<void>(loader_.cancelLoad(ticket, /*keep_partial=*/false));
  }
  // Unstarted queue entries conclude immediately.
  for (std::size_t cursor = job_cursor_; cursor < job_queue_.size(); ++cursor) {
    setResult(job_queue_[cursor], SourceOutcome::kCancelled);
  }
  job_cursor_ = job_queue_.size();
  maybeFinish();
}

void LayoutImportBatch::maybeFinish() {
  if (state_ != State::kRunning && state_ != State::kCancelling) {
    return;
  }
  if (!pending_tickets_.isEmpty() || job_active_ || job_cursor_ < job_queue_.size()) {
    return;
  }
  if (state_ == State::kCancelling && rollback_workspace_) {
    // ROLLBACK (the rollbackBrowserReplayImports template): remove every
    // dataset the batch produced, then exact-restore the pre-batch
    // workspace.
    for (const DatasetId dataset_id : produced_datasets_) {
      if (hooks_.remove_dataset) {
        hooks_.remove_dataset(dataset_id);
      }
    }
    produced_datasets_.clear();
    if (!rollback_workspace_()) {
      emitDiagnostic(
          DiagnosticLevel::kWarning, "layout-import-rollback-failed",
          tr("The cancelled layout import was removed, but the previous workspace could not be restored exactly."));
    }
  }
  // The state check above makes this block run exactly once: kFinished is
  // set before anything re-entrant can observe it.
  state_ = State::kFinished;
  QObject::disconnect(load_finished_conn_);
  load_finished_conn_ = {};
  QObject::disconnect(load_committing_conn_);
  load_committing_conn_ = {};
  // Queued, never inline: the owner may be inside start()/cancel() when the
  // last child concludes synchronously.
  QMetaObject::invokeMethod(this, [this]() { emit finished(); }, Qt::QueuedConnection);
}

}  // namespace PJ
