#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Shared wiring snippets that MUST stay identical between the two owners of
// a bound toolbox-plugin instance: MainWindow::launchToolbox (the
// interactive panel path) and HeadlessDescriptorProviderSession (the
// dialog-free layout-import path). A provider plugin must see the same host
// behavior in both, so the pieces that would otherwise be verbatim copies
// live here.

#include <QMetaObject>
#include <QString>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "pj_base/diagnostic_sink.hpp"
#include "pj_base/toolbox_protocol.h"
#include "pj_plugins/host/message_parser_library.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/ToolboxRuntimeHost.h"
#ifndef __EMSCRIPTEN__
#include "pj_runtime/SourceCaptureService.h"
#endif

namespace PJ {

// Map a toolbox diagnostic level onto the app-side DiagnosticLevel (unknown
// values fall back to kInfo).
[[nodiscard]] inline DiagnosticLevel toolboxDiagnosticLevel(PJ_toolbox_message_level_t level) {
  if (level == PJ_TOOLBOX_MESSAGE_ERROR) {
    return DiagnosticLevel::kError;
  }
  if (level == PJ_TOOLBOX_MESSAGE_WARNING) {
    return DiagnosticLevel::kWarning;
  }
  return DiagnosticLevel::kInfo;
}

// One-call Diagnostic fill+forward for the host-side sinks (the layout-import
// batch and the headless provider session each hand-rolled this same block).
// No-op when the sink is unset — the zero-cost no-listener path.
inline void emitDiagnosticTo(
    const DiagnosticSink& sink, DiagnosticLevel level, std::string source, std::string id, std::string message) {
  if (!sink) {
    return;
  }
  Diagnostic diagnostic;
  diagnostic.level = level;
  diagnostic.source = std::move(source);
  diagnostic.id = std::move(id);
  diagnostic.message = std::move(message);
  sink(diagnostic);
}

// ToolboxRuntimeHost::ParserIngestDeps::register_object_parser wiring. The
// SessionManager registry is worker-safe, and registration must complete before
// cbEnsureParserBinding returns so a synchronous ingest tap can resolve it on
// the first push.
[[nodiscard]] inline std::function<void(ObjectTopicId, std::unique_ptr<MessageParserHandle>)> makeObjectParserRegistrar(
    SessionManager& session) {
  return [&session](ObjectTopicId id, std::unique_ptr<MessageParserHandle> parser) {
    session.registerObjectTopicParser(id, std::move(parser));
  };
}

// M3 source-capture wiring shared by MainWindow::launchToolbox and
// HeadlessDescriptorProviderSession: arm every parser-ingest context of this
// toolbox binding for capture under the plugin's stable manifest id, and on a
// record-worthy release attach the dataset's SourceRecord (what layout save
// serializes as <materialize>) on the GUI thread. `session` must outlive the
// toolbox host (both callers bind app-lifetime services). A null service —
// wasm, or a shell running without a cache — leaves the deps untouched.
inline void wireSourceCapture(
    ToolboxRuntimeHost::ParserIngestDeps& deps, SourceCaptureService* capture_service, std::string provider_manifest_id,
    SessionManager& session) {
#ifdef __EMSCRIPTEN__
  static_cast<void>(deps);
  static_cast<void>(capture_service);
  static_cast<void>(provider_manifest_id);
  static_cast<void>(session);
#else
  if (capture_service == nullptr) {
    return;
  }
  deps.capture_service = capture_service;
  deps.capture_provider_id = std::move(provider_manifest_id);
  deps.on_capture_finalized = [&session](
                                  DatasetId dataset, const std::string& provider_id, const std::string& descriptor_json,
                                  const std::string& source_identity, bool published,
                                  const std::string& /*refusal_reason*/, const std::filesystem::path& artifact_path) {
    // Release runs on a plugin worker thread; the record and source
    // registries are GUI-thread-only. Queued onto the SessionManager's (GUI)
    // thread.
    const QString artifact = published ? QString::fromStdU16String(artifact_path.u16string()) : QString{};
    QMetaObject::invokeMethod(
        &session,
        [&session, dataset, artifact, provider = QString::fromStdString(provider_id),
         identity = QString::fromStdString(source_identity), descriptor = QString::fromStdString(descriptor_json)]() {
          session.attachSourceRecord(
              dataset, SourceRecord{
                           .provider_id = provider,
                           .source_identity = identity,
                           .descriptor_json = descriptor,
                       });
          // A published capture is the dataset's saveable backing file:
          // register it like a loaded file so the layout-save walk (which
          // serializes loadedSources() entries backing live datasets) emits a
          // <fileInfo> + <materialize> for this download. No plugin identity —
          // the artifact is a standard MCAP, and the cache-first restore
          // builds its own loader hints. An unpublished capture stays
          // record-only (nothing on disk to reference).
          if (!artifact.isEmpty()) {
            session.setDatasetSourcePath(dataset, artifact);
            session.recordLoadedSource(artifact, /*prefix=*/{});
          }
        },
        Qt::QueuedConnection);
  };
#endif
}

}  // namespace PJ
