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
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DiagnosticEmit.h"
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
  // Disconnect first, so the cancelled tickets reach no slot of this dying object; each session's destructor then
  // cancels, joins and destroys its jobs.
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
      // Identity is not compared: provider-identity and host-digest records name the same source when the
      // descriptor bytes match. No path fallback: the cache slot may hold other content now.
      if (candidate.record.provider_id == planned.ref.materialize_provider &&
          candidate.record.descriptor_json == planned.ref.materialize_descriptor_json) {
        return &candidate;
      }
      continue;
    }
    // E.g. the user opened the cache file directly.
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

void LayoutImportBatch::classifyFallback(std::size_t index, const QString& reason) {
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
  setResult(index, SourceOutcome::kFailed, tr("%1; the data source does not exist on disk").arg(reason));
  emitDiagnostic(
      DiagnosticLevel::kWarning, "layout-import-source-missing",
      tr("Layout's data source '%1' does not exist on disk and no provider could re-obtain it (%2).")
          .arg(planned.effective_path, reason));
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
      // An identity mapping, or a stale entry from an earlier registration (the live-path override).
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
  planned.descriptor_utf8 = ref.materialize_descriptor_json.toUtf8();

  const bool descriptor_valid = !ref.materialize_provider.isEmpty() && !ref.materialize_identity.isEmpty() &&
                                !planned.descriptor_utf8.trimmed().isEmpty() &&
                                !QJsonDocument::fromJson(planned.descriptor_utf8).isNull();
  if (!descriptor_valid) {
    emitDiagnostic(
        DiagnosticLevel::kWarning, "layout-import-descriptor-invalid",
        tr("Layout source '%1' carries an invalid <materialize> record; falling back to the saved path.")
            .arg(ref.resolved_path));
    classifyFallback(index, tr("invalid <materialize> record"));
    return;
  }

  // The host source cache before any provider contact. resolve() verified the artifact against its embedded
  // pj.capture manifest (quarantining it otherwise), so a hit is load-ready.
#ifndef __EMSCRIPTEN__
  if (capture_service_ != nullptr) {
    const std::string provider_utf8 = ref.materialize_provider.toStdString();
    const std::string_view descriptor_view(
        planned.descriptor_utf8.constData(), static_cast<std::size_t>(planned.descriptor_utf8.size()));
    std::string miss;
    SourceCacheStore::MissKind miss_kind = SourceCacheStore::MissKind::kAbsent;
    if (auto resolved = capture_service_->resolve(provider_utf8, descriptor_view, &miss, &miss_kind)) {
      // The record identity every host path uses (capture attach, this hit's attach).
      planned.query_identity = QString::fromStdString(sourceCacheIdentityDigest(provider_utf8, descriptor_view));
      planned.effective_path = QString::fromStdU16String(resolved->artifact.path.u16string());
      planned.host_cache_hit = true;
      planned.host_cache_pin = std::make_shared<SourceCacheStore::Pinned>(std::move(resolved->artifact));
      registerRewrite(ref, planned.effective_path, fileinfo_remap, qualifier_remap);
      const QFileInfo effective_info(planned.effective_path);
      if (const LoadedCandidate* match =
              findAlreadyLoaded(planned, pathIdentityKey(planned.effective_path, effective_info))) {
        if (match->has_record && !match->source_path.isEmpty() && match->source_path != planned.effective_path) {
          // The document must describe the session already loaded (see planViaProvider).
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
      // Another instance is publishing this request: the provider would download the same bytes again, so this
      // source fails instead (no retry).
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

  HeadlessDescriptorProviderSession* session = sessionFor(ref.materialize_provider);
  if (session == nullptr) {
    const QString reason = tr("provider '%1' is unavailable: %2")
                               .arg(ref.materialize_provider, failed_providers_.value(ref.materialize_provider));
    emitDiagnostic(
        DiagnosticLevel::kWarning, "layout-import-provider-unavailable",
        tr("Layout source '%1': %2; falling back to the saved path.").arg(ref.resolved_path, reason));
    classifyFallback(index, reason);
    return;
  }
  const auto query = session->queryDescriptor(
      std::string_view(planned.descriptor_utf8.constData(), static_cast<std::size_t>(planned.descriptor_utf8.size())));
  // The provider contract always returns an identity, and the record attach needs it: fail, never degrade.
  if (query.has_value() && query->source_identity.empty()) {
    setResult(index, SourceOutcome::kFailed, tr("provider returned no source identity"));
    emitDiagnostic(
        DiagnosticLevel::kWarning, "layout-import-query-invalid",
        tr("Provider '%1' answered the query for layout source '%2' without a source identity.")
            .arg(ref.materialize_provider, ref.resolved_path));
    return;
  }
  QString effective_path = query.has_value() ? QString::fromStdString(query->local_path_utf8) : QString{};
#ifndef __EMSCRIPTEN__
  if (effective_path.isEmpty() && query.has_value() && capture_service_ != nullptr) {
    // A provider that names no path leaves materialization to the host: the import lands in the cache slot.
    effective_path = QString::fromStdU16String(
        capture_service_->store()
            .pathFor(sourceCacheIdentity(
                ref.materialize_provider.toStdString(),
                std::string_view(
                    planned.descriptor_utf8.constData(), static_cast<std::size_t>(planned.descriptor_utf8.size()))))
            .u16string());
  }
#endif
  if (effective_path.isEmpty()) {
    const QString reason = tr("provider '%1' could not resolve it: %2")
                               .arg(
                                   ref.materialize_provider,
                                   query.has_value() ? tr("no local path") : QString::fromStdString(query.error()));
    emitDiagnostic(
        DiagnosticLevel::kWarning, "layout-import-query-failed",
        tr("Layout source '%1': %2; falling back to the saved path.").arg(ref.resolved_path, reason));
    classifyFallback(index, reason);
    return;
  }

  planned.effective_path = effective_path;
  planned.estimated_bytes = query->estimated_bytes;

  // The document must describe the session the restore will produce.
  record_rewrite(planned.effective_path);

  // Refused fails even a local cache hit.
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

  // Provenance first, then the provider's is_materialized verdict decides hit or miss: file existence alone would
  // promote a stale or partial file at the cache path to a hit.
  planned.query_identity = QString::fromStdString(query->source_identity);
  const QFileInfo effective_info(planned.effective_path);
  if (const LoadedCandidate* match =
          findAlreadyLoaded(planned, pathIdentityKey(planned.effective_path, effective_info))) {
    if (match->has_record && !match->source_path.isEmpty() && match->source_path != planned.effective_path) {
      // The cache path moved since this dataset loaded. Path-tier curve and timeline binding compare against the
      // dataset's tracked path, so the document gets the live path.
      planned.effective_path = match->source_path;
      record_rewrite(planned.effective_path);
    }
    setResult(index, SourceOutcome::kResolvedAlreadyLoaded);
    return;
  }
  if (query->is_materialized) {
    if (effective_info.exists()) {
      // No network touch, so no trust gate.
      planned.plan = Plan::kHit;
      return;
    }
    // A provider bug: importing would hide the inconsistent verdict.
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
  // An estimate of 0 is unknown: the job itself enforces max_transfer_bytes.
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
  // QDom handles are shared: doc_ is the caller's document.
  doc_ = doc;

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
      continue;
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

  // Before any child is enqueued, so "Cancel layout load" leaves the session untouched.
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

  QSet<QString> needed_providers;
  for (const std::size_t index : job_queue_) {
    needed_providers.insert(planned_[index].ref.materialize_provider);
  }
  std::erase_if(sessions_, [&needed_providers](const auto& entry) { return !needed_providers.contains(entry.first); });

  startNextJob();
  // Everything may have failed synchronously; finished() still follows, queued.
  maybeFinish();
  return StartResult::kRunning;
}

void LayoutImportBatch::startHitLoad(std::size_t index) {
  PlannedSource& planned = planned_[index];
  // A host-cache artifact is always a standard MCAP: requiring the layout's plugin and preset (possibly another
  // format) would reject, then quarantine, a valid artifact. The loader picks the MCAP source by extension.
  LoadHints hints;
  if (planned.host_cache_hit) {
    hints.dialog_policy = DialogPolicy::kNever;
    hints.prefer_reuse = true;
  } else {
    hints = layoutReplayHints(planned.ref, /*prefer_reuse=*/true);
    // Only a fallback loads the saved path; a provider-resolved cache path has no side files beside it.
    hints.record_saved_filepath = planned.provider_fallback;
  }
  const quint64 ticket = loader_.loadFileTicketed(LoadInput::fromNativePath(planned.effective_path), nullptr, hints);
  if (ticket == 0) {
    if (planned.host_cache_hit) {
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
  planned.host_cache_pin.reset();  // so the quarantine can act
  // Outside kRunning a queued job would never start (startNextJob refuses) and the batch would never finish; and
  // the failure is the cancel's, not evidence against the artifact.
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
  planViaProvider(index, nullptr, nullptr);
  // prepare() pointed the document at the quarantined cache path, which path-tier binding would never resolve.
  // A qualifier holds exactly that path, nothing else.
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
      // The batch's one trust prompt has run: fail, never prompt late.
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
      // Not expected (the session existed at query time); fail rather than crash.
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
          // At most one job runs, so this is the active job's dataset.
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
  // Before startNextJob(): the next job must not inherit this one's dataset or cancel address.
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
      // No SourceRecord: a re-saved layout carries no <materialize> for it.
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
  // Legal re-entry: the session concluded the job before this continuation ran.
  startNextJob();
  maybeFinish();
}

void LayoutImportBatch::onLoadCommitting(quint64 ticket, const QVector<DatasetId>& produced) {
  const auto it = pending_tickets_.constFind(ticket);
  if (it == pending_tickets_.constEnd()) {
    return;
  }
  const PlannedSource& planned = planned_[it.value()];
  // Layout save writes <materialize> only for record-bearing datasets. The prefer_reuse and fan-out paths never
  // emit loadCommitting, so a reused or fanned-out hit keeps a path-only identity.
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
      // The dataset reads the artifact: its own pin (released by removeDataset or merge) keeps eviction and
      // quarantine away.
      session_manager_.pinDatasetResource(dataset_id, planned.host_cache_pin);
    }
  }
}

void LayoutImportBatch::onLoadFinished(quint64 ticket, LoadOutcome outcome, const QVector<DatasetId>& produced) {
  const auto it = pending_tickets_.constFind(ticket);
  if (it == pending_tickets_.constEnd()) {
    return;
  }
  const std::size_t index = it.value();
  pending_tickets_.erase(it);
  for (const DatasetId produced_id : produced) {
    produced_datasets_.push_back(produced_id);
  }
  switch (outcome) {
    case LoadOutcome::kLoaded:
      // The datasets took their pin copies at loadCommitting.
      planned_[index].host_cache_pin.reset();
      setResult(index, SourceOutcome::kResolvedCacheHit);
      break;
    case LoadOutcome::kCancelled:
      // A keep-partial stop skips loadCommitting: kept datasets take their pin here, or one with lazy payloads
      // would lose its artifact to cleanup. A rollback removes them, releasing the pins.
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
  // After the join the terminals are queued; they still deliver, so produced_datasets_ is complete before
  // maybeFinish() sees the last child conclude.
  for (const auto& [provider_id, session] : sessions_) {
    static_cast<void>(provider_id);
    session->cancelAll();
    session->joinAll();
  }
  // Their kCancelled terminals conclude the tickets through onLoadFinished.
  const QList<quint64> tickets = pending_tickets_.keys();
  for (const quint64 ticket : tickets) {
    static_cast<void>(loader_.cancelLoad(ticket, /*keep_partial=*/false));
  }
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
    // Datasets first, then the workspace.
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
  // Set before anything re-entrant can run, so this block runs once.
  state_ = State::kFinished;
  QObject::disconnect(load_finished_conn_);
  load_finished_conn_ = {};
  QObject::disconnect(load_committing_conn_);
  load_committing_conn_ = {};
  // Queued: the owner may be inside start() or cancel().
  QMetaObject::invokeMethod(this, [this]() { emit finished(); }, Qt::QueuedConnection);
}

}  // namespace PJ
