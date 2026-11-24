// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "HeadlessDescriptorProviderSession.h"

#include <QMetaObject>
#include <Qt>
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "FileLoader.h"
#include "SourcePromotionHost.h"
#include "ToolboxHostWiring.h"
#include "pj_base/descriptor_import_protocol.h"
#include "pj_base/toolbox_protocol.h"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_plugins/host/toolbox_handle.hpp"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DerivedRuntimeHosts.h"
#include "pj_runtime/DiagnosticEmit.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/ToolboxRuntimeHost.h"

namespace PJ {

HeadlessDescriptorProviderSession::HeadlessDescriptorProviderSession(
    SessionManager& session, CatalogModel& catalog, QString provider_id, QString source_name,
    DiagnosticSink diagnostics)
    : session_(session),
      catalog_(catalog),
      provider_id_(std::move(provider_id)),
      source_name_(std::move(source_name)),
      diagnostics_(std::move(diagnostics)) {}

Expected<HeadlessDescriptorProviderSession::Ptr> HeadlessDescriptorProviderSession::create(
    SessionManager& session_manager, ExtensionCatalogService& extensions, FileLoader& loader, CatalogModel& catalog,
    const QString& provider_manifest_id, DiagnosticSink diagnostics, SourceCaptureService* capture_service) {
  // By manifest id (RuntimeToolboxPlugin::id), never by display name.
  const std::string wanted = provider_manifest_id.toStdString();
  const auto& toolboxes = extensions.toolboxes();
  const auto it = std::find_if(
      toolboxes.begin(), toolboxes.end(), [&wanted](const RuntimeToolboxPlugin& tb) { return tb.id == wanted; });
  if (it == toolboxes.end()) {
    return unexpected("descriptor-import provider toolbox '" + wanted + "' not found in the plugin catalog");
  }
  const QString source_name = it->name.empty() ? provider_manifest_id : QString::fromStdString(it->name);

  Ptr session(new HeadlessDescriptorProviderSession(
      session_manager, catalog, provider_manifest_id, source_name, std::move(diagnostics)));
  auto* self = session.get();

  // The services MainWindow::launchToolbox binds, all registered before bind() (the ServiceRegistryBuilder
  // contract).
  session->settings_ = makeToolboxSettings(it->id);
  session->builder_ = std::make_unique<ServiceRegistryBuilder>();

  ToolboxRuntimeHost::Callbacks callbacks;
  // The data half of MainWindow's on_data_changed. Refreshing the time reference matters: without it the t0
  // origin can stay at the 0 it took before any data existed. The presentation half (TF bridging, playback focus,
  // Custom Topics rows) is MainWindow's, through its batch-scoped SessionManager observers.
  callbacks.on_data_changed = [self](std::vector<DatasetId> ingested_datasets) {
    self->catalog_.rebuildFromDatastore();
    for (const DatasetId id : ingested_datasets) {
      self->session_.refreshDatasetTimeReference(id);
    }
  };
  callbacks.on_message = [self](PJ_toolbox_message_level_t level, std::string message) {
    emitDiagnosticTo(
        self->diagnostics_, toolboxDiagnosticLevel(level), self->source_name_.toStdString(),
        self->provider_id_.toStdString(), std::move(message));
  };
  // SessionManager bookkeeping only; MainWindow adds the strip widget and FileLoader arbitration. With no UI to
  // stop from, the plugin's cancellable flag is ignored.
  callbacks.on_ingest_started = [self](DatasetId dataset, std::string label, uint64_t total, bool /*cancellable*/) {
    const IngestToken token =
        self->session_.beginIngest(dataset, QString::fromStdString(label), total, /*cancellable=*/false, {});
    self->ingest_tokens_[dataset] = token;
  };
  callbacks.on_ingest_progress = [self](DatasetId dataset, uint64_t current, uint64_t total) {
    const auto token_it = self->ingest_tokens_.find(dataset);
    if (token_it != self->ingest_tokens_.end()) {
      self->session_.updateIngest(token_it->second, current, total);
    }
  };
  callbacks.on_ingest_finished = [self](DatasetId dataset) {
    const auto token_it = self->ingest_tokens_.find(dataset);
    if (token_it != self->ingest_tokens_.end()) {
      // The producer reports no outcome; kCompleted would claim a success nobody saw.
      self->session_.endIngest(token_it->second, IngestOutcome::kUnknown);
      self->ingest_tokens_.erase(token_it);
    }
  };

  ToolboxRuntimeHost::ParserIngestDeps ingest_deps;
  ingest_deps.catalog = &extensions;
  ingest_deps.ingest_taps = session_manager.ingestTapsShared();
  ingest_deps.register_object_parser = makeObjectParserRegistrar(session_manager);
  wireSourceCapture(ingest_deps, capture_service, wanted, session_manager);

  session->host_ = std::make_unique<ToolboxRuntimeHost>(
      session_manager.dataEngine(), session_manager.objectStore(), *session->settings_, std::move(callbacks),
      std::move(ingest_deps));
  if (auto status = session->host_->registerServices(*session->builder_); !status) {
    return unexpected("failed to publish toolbox services for '" + wanted + "': " + status.error());
  }

  session->derived_hosts_ = std::make_unique<DerivedRuntimeHosts>(session_manager, catalog, wanted);
  if (auto status = session->derived_hosts_->registerServices(*session->builder_); !status) {
    return unexpected("failed to publish data-processor services for '" + wanted + "': " + status.error());
  }

  // The provider identity is this binding's manifest id, never plugin-supplied. `host_` outlives `promotion_host_`
  // (see the destructor), so the captured pointer stays valid.
  session->promotion_host_ = std::make_unique<SourcePromotionHost>(
      loader, session_manager, provider_manifest_id,
      [host = session->host_.get()](DatasetId dataset_id) { return host->hasIngestForDataset(dataset_id); });
  if (auto status = session->promotion_host_->registerServices(*session->builder_); !status) {
    return unexpected("failed to publish the source-promotion service for '" + wanted + "': " + status.error());
  }

  // A failure from here releases the handle through `session`'s destructor (no jobs exist yet).
  session->handle_ = std::make_unique<ToolboxHandle>(it->library.createHandle());
  if (!session->handle_->valid()) {
    return unexpected("descriptor-import provider toolbox '" + wanted + "' failed to create a plugin instance");
  }
  if (auto status = session->handle_->bind(session->builder_->view()); !status) {
    return unexpected("failed to bind descriptor-import provider toolbox '" + wanted + "': " + status.error());
  }

  const void* extension = session->handle_->getPluginExtension(PJ_DESCRIPTOR_IMPORT_EXTENSION_V1);
  if (extension == nullptr) {
    return unexpected("toolbox '" + wanted + "' does not expose the " PJ_DESCRIPTOR_IMPORT_EXTENSION_V1 " extension");
  }
  session->view_ = DescriptorImportProviderView(extension, session->handle_->context());
  if (!session->view_.valid()) {
    return unexpected(
        "toolbox '" + wanted +
        "' exposes a " PJ_DESCRIPTOR_IMPORT_EXTENSION_V1
        " extension whose struct_size does not cover the v1 query_descriptor/start_import slots");
  }
  return session;
}

HeadlessDescriptorProviderSession::~HeadlessDescriptorProviderSession() {
  // Order and reasons: the class comment. Shutdown before the join: the ABI cannot cancel an accepted promotion,
  // and a worker may block its terminal on that promotion's result.
  cancelAll();
  if (promotion_host_ != nullptr) {
    promotion_host_->shutdown();
  }
  joinAll();
  jobs_.clear();
  handle_.reset();
  promotion_host_.reset();
  builder_.reset();
  derived_hosts_.reset();
  host_.reset();
  settings_.reset();
}

Expected<DescriptorQueryResult> HeadlessDescriptorProviderSession::queryDescriptor(
    std::string_view descriptor_json) const {
  return view_.queryDescriptor(descriptor_json);
}

Expected<HeadlessDescriptorProviderSession::JobId> HeadlessDescriptorProviderSession::startImport(
    const DescriptorImportStartRequest& request, std::function<void(DatasetId)> on_dataset,
    std::function<void(DescriptorImportOutcome, std::string)> on_terminal) {
  const JobId job_id = ++next_job_id_;

  // Always queued, never Auto: delivery is never inline, even from the GUI thread.
  auto queued_dataset = [this, cb = std::move(on_dataset)](DatasetId dataset) {
    if (!cb) {
      return;
    }
    QMetaObject::invokeMethod(this, [cb, dataset]() { cb(dataset); }, Qt::QueuedConnection);
  };
  auto queued_terminal = [this, job_id, cb = std::move(on_terminal)](
                             DescriptorImportOutcome outcome, std::string message) {
    QMetaObject::invokeMethod(
        this,
        [this, job_id, cb, outcome, message = std::move(message)]() {
          // Conclude before the owner callback, which may call startImport again.
          concludeJob(job_id);
          if (cb) {
            cb(outcome, message);
          }
        },
        Qt::QueuedConnection);
  };

  auto job = view_.startImport(request, std::move(queued_dataset), std::move(queued_terminal));
  if (!job) {
    return unexpected(std::move(job).error());
  }
  jobs_.emplace(job_id, std::move(*job));
  return job_id;
}

void HeadlessDescriptorProviderSession::cancelJob(JobId job_id) {
  const auto it = jobs_.find(job_id);
  if (it == jobs_.end()) {
    return;
  }
  it->second.cancel();
}

void HeadlessDescriptorProviderSession::cancelAll() {
  for (auto& entry : jobs_) {
    entry.second.cancel();
  }
}

void HeadlessDescriptorProviderSession::joinAll() {
  for (auto& entry : jobs_) {
    entry.second.join();
  }
}

void HeadlessDescriptorProviderSession::concludeJob(JobId job_id) {
  const auto it = jobs_.find(job_id);
  if (it == jobs_.end()) {
    return;
  }
  JoinableJob job = std::move(it->second);
  jobs_.erase(it);
}

}  // namespace PJ
