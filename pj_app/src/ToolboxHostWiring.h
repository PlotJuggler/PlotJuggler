#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Wiring shared by the two owners of a bound toolbox plugin instance, MainWindow::launchToolbox and
// HeadlessDescriptorProviderSession: a provider plugin must see the same host in both.

#include <QMetaObject>
#include <QString>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "pj_base/diagnostic_sink.hpp"
#include "pj_base/toolbox_protocol.h"
#include "pj_plugins/host/message_parser_library.hpp"
#include "pj_runtime/DiagnosticEmit.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/ToolboxRuntimeHost.h"
#ifndef __EMSCRIPTEN__
#include "pj_runtime/SourceCaptureService.h"
#endif

namespace PJ {

// Unknown levels map to kInfo.
[[nodiscard]] inline DiagnosticLevel toolboxDiagnosticLevel(PJ_toolbox_message_level_t level) {
  if (level == PJ_TOOLBOX_MESSAGE_ERROR) {
    return DiagnosticLevel::kError;
  }
  if (level == PJ_TOOLBOX_MESSAGE_WARNING) {
    return DiagnosticLevel::kWarning;
  }
  return DiagnosticLevel::kInfo;
}

// For ParserIngestDeps::register_object_parser; called on worker threads. Registration completes before
// cbEnsureParserBinding returns, so a synchronous ingest tap resolves the parser on the first push.
[[nodiscard]] inline std::function<void(ObjectTopicId, std::unique_ptr<MessageParserHandle>)> makeObjectParserRegistrar(
    SessionManager& session) {
  return [&session](ObjectTopicId id, std::unique_ptr<MessageParserHandle> parser) {
    session.registerObjectTopicParser(id, std::move(parser));
  };
}

// Arms every parser-ingest context of this binding for source capture under the plugin's manifest id; a finalized
// capture attaches the dataset's SourceRecord (saved as <materialize>) on the GUI thread. `session` must outlive the
// toolbox host. A null service (browser, or no cache) leaves `deps` untouched.
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
    // Runs on a plugin worker thread; the record and source registries are GUI-thread only.
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
          // A published capture becomes the dataset's backing file, registered like a loaded file so a layout
          // save writes <fileInfo> + <materialize> for it. No plugin identity: the artifact is a standard MCAP. An
          // unpublished capture stays record-only.
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
