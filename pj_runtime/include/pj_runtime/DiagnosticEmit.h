#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <string>
#include <utility>

#include "pj_base/diagnostic_sink.hpp"

namespace PJ {

/// Fills one Diagnostic and forwards it to `sink`; no-op when the sink is unset (the zero-cost
/// no-listener path).
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

}  // namespace PJ
