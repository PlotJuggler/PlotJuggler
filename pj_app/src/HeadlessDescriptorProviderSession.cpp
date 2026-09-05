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
#include "pj_runtime/DataProcessorsRuntimeHost.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/QSettingsBackend.h"
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
  // 1. Find the provider toolbox by STABLE MANIFEST ID (RuntimeToolboxPlugin::id).
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

  // 2. Assemble the host services — the SAME set the interactive path binds
  //    (MainWindow::launchToolbox), so the plugin sees an identical host
  //    environment headless vs interactive. Registration strictly BEFORE the
  //    handle's bind() (the ServiceRegistryBuilder contract).
  session->settings_ = std::make_unique<QSettingsBackend>();
  session->builder_ = std::make_unique<ServiceRegistryBuilder>();

  ToolboxRuntimeHost::Callbacks callbacks;
  // The DATA half of MainWindow's on_data_changed: publish the ingested
  // topics into the catalog tree and refresh each ingested dataset's time
  // reference (FileLoader parity — without it the t0-offset origin memo can
  // stay pinned at the 0 it acquired before any data existed). The
  // PRESENTATION half is deliberately absent here, because it lives on
  // MainWindow-owned surfaces this session has no business reaching:
  // 3D-scene TF bridging (transform_service_), playback focus/seeding
  // (AppSession::focusPlaybackOnDatasets / seedPlaybackFromSession), and the
  // Custom Series panel rows. MainWindow (the batch owner) supplies that
  // presentation from outside: its batch-scoped SessionManager observers
  // (installLayoutBatchObservers) drive the title-bar ingest strip
  // mid-import, and the progressive-restore drain settles the rest.
  callbacks.on_data_changed = [self](std::vector<DatasetId> ingested_datasets) {
    self->catalog_.rebuildFromDatastore();
    for (const DatasetId id : ingested_datasets) {
      self->session_.refreshDatasetTimeReference(id);
    }
  };
  // Same information MainWindow forwards to its diagnostic history: mapped
  // level, human-readable source, message text.
  callbacks.on_message = [self](PJ_toolbox_message_level_t level, std::string message) {
    emitDiagnosticTo(
        self->diagnostics_, toolboxDiagnosticLevel(level), self->source_name_.toStdString(),
        self->provider_id_.toStdString(), std::move(message));
  };
  // Ingest lifecycle: the SessionManager bookkeeping only (the #470 hoist).
  // MainWindow's versions add strip-widget presentation and FileLoader
  // arbitration on top; a headless session has no strip.
  // A headless session has no UI to stop anything from, so the plugin's own
  // cancellable declaration is deliberately ignored here.
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
      // A headless session's producer reports no outcome, so kUnknown is the
      // truthful terminal — claiming kCompleted would assert success nobody saw.
      self->session_.endIngest(token_it->second, IngestOutcome::kUnknown);
      self->ingest_tokens_.erase(token_it);
    }
  };

  // Parser-ingest deps: identical to the interactive path, including parser
  // registration before the first pushed message (see ToolboxHostWiring.h).
  ToolboxRuntimeHost::ParserIngestDeps ingest_deps;
  ingest_deps.catalog = &extensions;
  ingest_deps.ingest_taps = session_manager.ingestTapsShared();
  ingest_deps.register_object_parser = makeObjectParserRegistrar(session_manager);
  // M3: capture this binding's delegated downloads into the source cache
  // (identical to the interactive path; no-op when capture_service is null).
  wireSourceCapture(ingest_deps, capture_service, wanted, session_manager);

  session->host_ = std::make_unique<ToolboxRuntimeHost>(
      session_manager.dataEngine(), session_manager.objectStore(), *session->settings_, std::move(callbacks),
      std::move(ingest_deps));
  if (auto status = session->host_->registerServices(*session->builder_); !status) {
    return unexpected("failed to publish toolbox services for '" + wanted + "': " + status.error());
  }

  session->dp_host_ = std::make_unique<DataProcessorsRuntimeHost>(session_manager.dataProcessorService(), wanted);
  if (auto status = session->dp_host_->registerServices(*session->builder_); !status) {
    return unexpected("failed to publish data-processor services for '" + wanted + "': " + status.error());
  }

  // Source promotion, bound per plugin instance: provider identity is THIS
  // binding's manifest id (host-derived, unspoofable) and the ownership
  // predicate is this instance's own ingest bookkeeping. `host_` outlives
  // `promotion_host_` (see the destructor), so the captured raw pointer
  // stays valid for the predicate's whole life.
  session->promotion_host_ = std::make_unique<SourcePromotionHost>(
      loader, session_manager, provider_manifest_id,
      [host = session->host_.get()](DatasetId dataset_id) { return host->hasIngestForDataset(dataset_id); });
  if (auto status = session->promotion_host_->registerServices(*session->builder_); !status) {
    return unexpected("failed to publish the source-promotion service for '" + wanted + "': " + status.error());
  }

  // 3. Create + bind the plugin instance. Every failure from here returns
  //    through `session`'s destructor: no jobs exist yet, so the quiescence
  //    step is trivially satisfied and the handle (if created) is released —
  //    nothing leaks.
  session->handle_ = std::make_unique<ToolboxHandle>(it->library.createHandle());
  if (!session->handle_->valid()) {
    return unexpected("descriptor-import provider toolbox '" + wanted + "' failed to create a plugin instance");
  }
  if (auto status = session->handle_->bind(session->builder_->view()); !status) {
    return unexpected("failed to bind descriptor-import provider toolbox '" + wanted + "': " + status.error());
  }

  // 4. Resolve the descriptor-import extension (tail-slot gated) and gate on
  //    the view's struct_size validation.
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
  return session;  // implicit move (returning a local by name)
}

HeadlessDescriptorProviderSession::~HeadlessDescriptorProviderSession() {
  // (1) Cancel every import job, then FAIL THE PROMOTION INTAKE BEFORE
  // JOINING. A provider worker may legally block its terminal on an ACCEPTED
  // promotion's exactly-once result callback — the ABI has no way to cancel
  // a promotion — so joining first can deadlock: joinAll waits on a terminal
  // that waits on a callback only shutdown() can deliver. shutdown()
  // (idempotent; phase 1 of the two-phase teardown, uniform with
  // ~PanelSession — Codex r1 F5) closes the intake and fails every
  // accepted-but-unfinished promotion (ok=false), which reaches plugin
  // code — the instance/DSO is still alive here, exactly as its contract
  // demands.
  cancelAll();
  if (promotion_host_ != nullptr) {
    promotion_host_->shutdown();
  }
  // (2) QUIESCENCE: join + destroy every import job while the plugin
  // instance, its DSO, and every host it can call into are still alive.
  // After this, no provider worker exists and (per the SDK's JoinableJob
  // contract) no job callback can ever run again — which is the
  // precondition ToolboxRuntimeHost's destructor demands and what makes the
  // ~QObject purge of any still-queued terminal metacalls legal.
  joinAll();
  jobs_.clear();
  // (3) The plugin instance persists its state through the settings backend
  // in its destructor, so the handle dies while builder/hosts/settings live.
  // The promotion-host OBJECT survives until after the handle: a straggler
  // call into the raw service pointer during instance destroy hits
  // shutdown()'s safe synchronous rejection, never a use-after-free.
  handle_.reset();
  promotion_host_.reset();
  // (4) Service views into the hosts, then the hosts, then the backend —
  // the same relative order as MainWindow's ~PanelSession.
  builder_.reset();
  dp_host_.reset();
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

  // Queued-only marshal (the locked project rule): the provider's
  // job-callback thread only POSTS; host-side code runs exclusively on the
  // GUI thread. Qt::QueuedConnection unconditionally — never Auto — so
  // delivery is uniform and never inline.
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
          // Conclude BEFORE the owner callback: the registry is already
          // consistent (and re-entrant startImport calls are fine) when the
          // callback observes the terminal. Destroying the job here is not a
          // self-destroy — the provider's terminal returned when it posted
          // this metacall, and destroy() joins from the GUI thread.
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
    return;  // unknown or already concluded — per-job cancel is best-effort
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
    return;  // defensive: no live path reaches here — teardown purges pending continuations
  }
  JoinableJob job = std::move(it->second);
  jobs_.erase(it);
  // `job` destructs here: destroy = cancel+join, idempotent after the
  // terminal already returned on the provider side.
}

}  // namespace PJ
