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
class DataProcessorsRuntimeHost;
class ExtensionCatalogService;
class FileLoader;
class QSettingsBackend;
class ServiceRegistryBuilder;
class SourceCaptureService;
class SourcePromotionHost;
class ToolboxHandle;
class ToolboxRuntimeHost;

// Headless per-batch owner of ONE descriptor-import provider toolbox
// instance — MainWindow::launchToolbox minus the UI half (no dialog, no
// PanelEngine, no QWidget). LayoutImportBatch (T6) owns exactly one of
// these per import batch and is this class's sole intended consumer.
//
// create() finds the provider toolbox by its STABLE MANIFEST ID in the
// plugin catalog, assembles the SAME host-service set the interactive path
// binds (ToolboxRuntimeHost + DataProcessorsRuntimeHost + the per-instance
// SourcePromotionHost, all registered strictly before bind()), creates and
// binds one plugin instance, and resolves its "pj.descriptor_import.v1"
// extension — so a provider plugin sees an IDENTICAL host environment
// headless vs interactive. No loadConfig() is issued: an import session's
// inputs arrive via the descriptor, and the plugin reads its own persisted
// settings (server history, credentials) through the SettingsStoreService
// that ToolboxRuntimeHost::registerServices already provides. Creation FAILS
// cleanly (an error Expected with a precise reason, no leaked handle) when
// the manifest id is unknown, bind() fails, the extension is absent, or the
// extension's struct_size does not cover the v1 slots. The §6.4
// provider-absent stock fallback is the CALLER's job — this class only
// reports the reason.
//
// Holding the session pins the plugin DSO: the ToolboxHandle's internal
// library owner keeps the shared library loaded even across catalog
// rescans, satisfying the SDK rule that the plugin instance and its DSO
// stay alive until every JoinableJob obtained through the extension view
// has been destroyed (the view itself holds no keep-alive).
//
// Threading: the session is a QObject that must be created and destroyed on
// the GUI thread; create()/queryDescriptor()/startImport()/cancelJob()/
// cancelAll()/joinAll() are GUI-thread-only ([main-thread] slots on the
// provider side, and the job registry is deliberately unsynchronized). Both
// startImport callbacks are marshalled QUEUED-ONLY onto the GUI thread —
// provider job-callback threads only post, never run host code (the
// project-wide marshal rule shared with the promotion/runtime-host
// callbacks).
//
// QUIESCENCE GUARANTEE (the absorbed PR-A follow-up): the concurrent
// dtor-vs-worker race on ToolboxRuntimeHost is out of contract — the plugin
// must be unbound and its workers quiesced before host destruction
// (ToolboxRuntimeHost.h). This session is the layer that MAKES that
// precondition true. Its destructor, in order:
//   (1) cancels, joins, and destroys EVERY import job (so no provider
//       worker can ever call back into the hosts again);
//   (2) destroys the SourcePromotionHost (its dtor fails pending promotion
//       callbacks, which reaches plugin code — the instance/DSO must still
//       be alive);
//   (3) releases the plugin handle (instance destroy, DSO unpin);
//   (4) tears down builder, dp-host, runtime host, settings — the same
//       relative order as MainWindow's ~PanelSession.
// Teardown consequence of the queued-only marshal: a queued-but-undelivered
// terminal metacall is purged by ~QObject. That is legal ONLY because step
// (1) already cancelled+joined those jobs, so the owner has concluded them
// as cancelled before the notification could have been observed.
//
// Not movable: host vtables and the queued metacalls store `this`.
class HeadlessDescriptorProviderSession : public QObject {
  Q_OBJECT
 public:
  using Ptr = std::unique_ptr<HeadlessDescriptorProviderSession>;

  // Session-unique identity of one started import job — the cancelJob()
  // address, valid until that job's terminal concludes it. Never reused
  // within a session.
  using JobId = quint64;

  // Builds a fully-bound session for `provider_manifest_id` from the pieces
  // MainWindow::launchToolbox uses. All references must outlive the session.
  // `diagnostics` receives the plugin's report_message stream (level +
  // source + text — the same information MainWindow routes to its
  // diagnostic history); it may be empty. Returns an error with a distinct,
  // precise reason on any of the four creation failures (see class
  // doc-comment) — the partially-built session is destroyed on that path,
  // so no plugin handle leaks.
  // `capture_service` (nullable, desktop only) arms this binding's delegated
  // ingests for M3 source capture — identical wiring to the interactive path
  // (ToolboxHostWiring.h wireSourceCapture).
  [[nodiscard]] static Expected<Ptr> create(
      SessionManager& session_manager, ExtensionCatalogService& extensions, FileLoader& loader, CatalogModel& catalog,
      const QString& provider_manifest_id, DiagnosticSink diagnostics = {},
      SourceCaptureService* capture_service = nullptr);

  ~HeadlessDescriptorProviderSession() override;

  HeadlessDescriptorProviderSession(const HeadlessDescriptorProviderSession&) = delete;
  HeadlessDescriptorProviderSession& operator=(const HeadlessDescriptorProviderSession&) = delete;

  // [main-thread, strictly bounded] Passthrough to the provider's
  // query_descriptor (no network, no blocking work — the provider contract).
  [[nodiscard]] Expected<DescriptorQueryResult> queryDescriptor(std::string_view descriptor_json) const;

  // [main-thread] Starts one import through the provider's start_import and
  // keeps the returned job in the session-owned registry, returning the
  // job's JobId (the cancelJob() address). On success, `on_dataset` fires
  // zero-or-one time and `on_terminal` exactly once, both QUEUED onto the
  // GUI thread (never inline, never on a provider thread), in provider
  // order. The job is concluded (removed from the registry and destroyed)
  // before `on_terminal` runs, so the callback may re-enter startImport
  // freely. On an error return, neither callback will ever be invoked.
  // Either callback may be null.
  [[nodiscard]] Expected<JobId> startImport(
      const DescriptorImportStartRequest& request, std::function<void(DatasetId)> on_dataset,
      std::function<void(DescriptorImportOutcome, std::string)> on_terminal);

  // [main-thread] Best-effort, non-blocking cancel of ONE live job (its
  // JoinableJob::cancel passthrough — no join): the job's terminal still
  // arrives through the normal queued marshal, as kCancelled for a
  // well-behaved provider. An unknown or already-concluded id is a safe
  // no-op.
  void cancelJob(JobId job_id);

  // [main-thread] Best-effort, non-blocking cancel of every live job. Safe
  // to call at any time, including with no jobs live.
  void cancelAll();

  // [main-thread, blocking] Joins every live job — returns only after each
  // job's terminal callback has RETURNED on the provider side (delivery to
  // the owner still needs the event loop; see the queued-only marshal).
  // GUI-thread blocking by design: the batch owner calls it from its own
  // teardown/cancel path. Never call it from inside an on_terminal
  // continuation of one of THIS session's jobs that has not yet been
  // concluded (join-from-callback is a provider-side self-join) — in
  // practice continuations run after their job is concluded, so joining the
  // remaining jobs from there is safe.
  void joinAll();

  // Jobs started and not yet concluded by a delivered terminal (or teardown).
  [[nodiscard]] std::size_t activeJobCount() const noexcept {
    return jobs_.size();
  }

  // The provider's stable manifest id this session was bound for.
  [[nodiscard]] QString providerId() const {
    return provider_id_;
  }

 private:
  HeadlessDescriptorProviderSession(
      SessionManager& session, CatalogModel& catalog, QString provider_id, QString source_name,
      DiagnosticSink diagnostics);

  // GUI thread: remove `job_id` from the registry and destroy it (destroy =
  // cancel+join; never a self-join here — the provider's terminal already
  // returned when the queued continuation that calls this runs).
  void concludeJob(JobId job_id);

  SessionManager& session_;
  CatalogModel& catalog_;
  const QString provider_id_;
  // Human-readable diagnostic source: the plugin's manifest name, falling
  // back to the id (mirrors MainWindow::launchToolbox).
  const QString source_name_;
  DiagnosticSink diagnostics_;

  // Owner block mirroring MainWindow's PanelSession. The destructor makes
  // the load-bearing teardown order EXPLICIT (jobs -> promotion_host_ ->
  // handle_ -> builder_ -> dp_host_ -> host_ -> settings_); the implicit
  // reverse-declaration destruction that follows only resets already-null
  // pointers, so a future member reorder cannot silently break it.
  std::unique_ptr<QSettingsBackend> settings_;
  std::unique_ptr<ServiceRegistryBuilder> builder_;
  std::unique_ptr<ToolboxRuntimeHost> host_;
  std::unique_ptr<DataProcessorsRuntimeHost> dp_host_;
  std::unique_ptr<SourcePromotionHost> promotion_host_;
  std::unique_ptr<ToolboxHandle> handle_;

  // Borrowed (extension pointer + plugin ctx); valid while handle_ lives.
  DescriptorImportProviderView view_;

  // GUI-thread only (startImport / cancelJob / queued continuations /
  // teardown).
  JobId next_job_id_ = 0;
  std::map<JobId, JoinableJob> jobs_;
  std::unordered_map<DatasetId, IngestToken> ingest_tokens_;
};

}  // namespace PJ
