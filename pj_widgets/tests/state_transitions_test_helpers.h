#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Shared StateRow and StateSegment fixture builders for the state-transitions test binaries (scene and view);
// test-only.

#include <QString>
#include <utility>
#include <vector>

#include "pj_widgets/StateTransitionsView.h"

inline constexpr qint64 kSecondNs = 1'000'000'000LL;

inline PJ::StateSegment seg(qint64 start_s, qint64 end_s, const char* value) {
  return {.t_start_ns = start_s * kSecondNs, .t_end_ns = end_s * kSecondNs, .value = QString::fromUtf8(value)};
}

inline PJ::StateRow makeRow(quint64 id, const QString& name, std::vector<PJ::StateSegment> segments) {
  PJ::StateRow row;
  row.id = id;
  row.name = name;
  row.segments = std::move(segments);
  return row;
}

/// Name derived from the id ("series_<id>") — the scene tests' shorthand.
inline PJ::StateRow makeRow(quint64 id, std::vector<PJ::StateSegment> segments) {
  return makeRow(id, QStringLiteral("series_%1").arg(id), std::move(segments));
}
