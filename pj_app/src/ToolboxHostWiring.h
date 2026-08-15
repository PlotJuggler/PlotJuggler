#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Shared wiring snippets that MUST stay identical between the two owners of
// a bound toolbox-plugin instance: MainWindow::launchToolbox (the
// interactive panel path) and HeadlessDescriptorProviderSession (the
// dialog-free layout-import path). A provider plugin must see the same host
// behavior in both, so the pieces that would otherwise be verbatim copies
// live here.

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "pj_base/diagnostic_sink.hpp"
#include "pj_base/toolbox_protocol.h"
#include "pj_plugins/host/message_parser_library.hpp"
#include "pj_runtime/SessionManager.h"

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

}  // namespace PJ
