#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>
#include <QString>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string_view>
#include <unordered_map>

#include "pj_base/diagnostic_sink.hpp"
#include "pj_base/expected.hpp"
#include "pj_base/sdk/descriptor_import.hpp"
#include "pj_base/types.hpp"
#include "pj_runtime/SessionManager.h"

namespace PJ {

class CatalogModel;
class DerivedRuntimeHosts;
class ExtensionCatalogService;
class FileLoader;
class ServiceRegistryBuilder;
class SourceCaptureService;
class SourcePromotionHost;
class ToolboxHandle;
class ToolboxRuntimeHost;
namespace sdk {
class SettingsBackend;
}  // namespace sdk

// One bound descriptor-import provider toolbox instance with no UI: MainWindow::launchToolbox minus the panel, owned
// by LayoutImportBatch (one per provider). create() binds the same host services as the interactive path (through
// ToolboxHostWiring.h), so the plugin sees the same host either way. No loadConfig() is issued: inputs arrive in the
// descriptor and the plugin reads its own settings through the SettingsStoreService. The stock fallback for an
// unavailable provider is the caller's; this class only reports the reason.
//
// Holding the session keeps the plugin DSO loaded (the ToolboxHandle owns its library), as the SDK requires until
// every JoinableJob from the extension view is destroyed; the view itself keeps nothing alive.
//
// GUI thread only: creation, destruction and every method (the provider's slots are main-thread and the job
// registry is unsynchronized). Both startImport callbacks are always queued onto the GUI thread: provider threads
// only post, never run host code.
//
// The destructor makes ToolboxRuntimeHost's precondition true (plugin unbound, workers quiesced before the host
// dies), in this order:
//   1. cancel every job, then SourcePromotionHost::shutdown() before any join: a provider worker may block its
//      terminal on an accepted promotion's result, which only shutdown() delivers, so joining first can deadlock;
//      shutdown() reaches plugin code, so the instance must be alive;
//   2. join and destroy every job: no provider worker can call back after this, which is also what makes ~QObject
//      purging a still-queued terminal legal (the owner already concluded those jobs);
//   3. release the plugin handle (the instance saves its settings in its destructor, so builder, hosts and settings
//      are still alive), then destroy the promotion host: a straggler call during instance destruction hits
//      shutdown()'s synchronous rejection, not freed memory;
//   4. builder, derived hosts (ending the provider's previews; its persistent rules stay), runtime host, settings:
//      the relative order of MainWindow's ~ToolboxPanelSession.
//
// Not movable: host vtables and the queued metacalls store `this`.
class HeadlessDescriptorProviderSession : public QObject {
  Q_OBJECT
 public:
  using Ptr = std::unique_ptr<HeadlessDescriptorProviderSession>;

  // Never reused within a session; valid until the job's terminal concludes it.
  using JobId = quint64;

  // All references must outlive the session. `diagnostics` (may be empty) receives the plugin's messages. Fails,
  // destroying the partial session, when the manifest id is unknown, a service cannot be published, the instance
  // cannot be created or bound, or the descriptor-import extension is missing or too small. `capture_service`
  // (nullable, desktop only) arms source capture as wireSourceCapture does for the interactive path.
  [[nodiscard]] static Expected<Ptr> create(
      SessionManager& session_manager, ExtensionCatalogService& extensions, FileLoader& loader, CatalogModel& catalog,
      const QString& provider_manifest_id, DiagnosticSink diagnostics = {},
      SourceCaptureService* capture_service = nullptr);

  ~HeadlessDescriptorProviderSession() override;

  HeadlessDescriptorProviderSession(const HeadlessDescriptorProviderSession&) = delete;
  HeadlessDescriptorProviderSession& operator=(const HeadlessDescriptorProviderSession&) = delete;

  // Bounded by the provider contract: no network, no blocking work.
  [[nodiscard]] Expected<DescriptorQueryResult> queryDescriptor(std::string_view descriptor_json) const;

  // On success `on_dataset` fires zero or one time and `on_terminal` exactly once, both queued onto the GUI thread,
  // in provider order. The job is concluded (removed and destroyed) before `on_terminal` runs, so it may call
  // startImport again. On an error return neither fires. Either may be null.
  [[nodiscard]] Expected<JobId> startImport(
      const DescriptorImportStartRequest& request, std::function<void(DatasetId)> on_dataset,
      std::function<void(DescriptorImportOutcome, std::string)> on_terminal);

  // Best-effort and non-blocking: the terminal still arrives queued (kCancelled from a well-behaved provider). An
  // unknown or concluded id is a no-op.
  void cancelJob(JobId job_id);

  // Best-effort and non-blocking.
  void cancelAll();

  // Blocks the GUI thread until each live job's terminal has returned on the provider side; delivery to the owner
  // still needs the event loop. Safe from an on_terminal continuation, whose own job is already concluded.
  void joinAll();

  // Jobs not yet concluded by a delivered terminal.
  [[nodiscard]] std::size_t activeJobCount() const noexcept {
    return jobs_.size();
  }

  [[nodiscard]] QString providerId() const {
    return provider_id_;
  }

 private:
  HeadlessDescriptorProviderSession(
      SessionManager& session, CatalogModel& catalog, QString provider_id, QString source_name,
      DiagnosticSink diagnostics);

  // Destroying the job joins it; not a self-join, as the provider's terminal returned before posting.
  void concludeJob(JobId job_id);

  SessionManager& session_;
  CatalogModel& catalog_;
  const QString provider_id_;
  // The plugin's manifest name, else its id.
  const QString source_name_;
  DiagnosticSink diagnostics_;

  // The destructor resets these explicitly in the order of the class comment, so a member reorder cannot break it.
  std::unique_ptr<sdk::SettingsBackend> settings_;
  std::unique_ptr<ServiceRegistryBuilder> builder_;
  std::unique_ptr<ToolboxRuntimeHost> host_;
  std::unique_ptr<DerivedRuntimeHosts> derived_hosts_;
  std::unique_ptr<SourcePromotionHost> promotion_host_;
  std::unique_ptr<ToolboxHandle> handle_;

  // Borrowed (extension pointer + plugin ctx); valid while handle_ lives.
  DescriptorImportProviderView view_;

  JobId next_job_id_ = 0;
  std::map<JobId, JoinableJob> jobs_;
  std::unordered_map<DatasetId, IngestToken> ingest_tokens_;
};

}  // namespace PJ
