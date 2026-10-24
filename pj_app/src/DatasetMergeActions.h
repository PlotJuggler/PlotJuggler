#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <functional>
#include <vector>

#include "pj_base/types.hpp"  // PJ::DatasetId

class QWidget;

namespace PJ {

class AppSession;

/// Human-readable destructive-merge warning for `datasets`: a base "this is
/// destructive" line plus, when applicable, which datasets overlap in displayed
/// time, which of those also collide on a shared topic name, and which carry
/// object topics that the merge will fold into the result. Reconstructed from the
/// runtime (catalog names, raw bounds, per-source display offset, engine topics),
/// so it is independent of any widget's local track list.
[[nodiscard]] QString composeDatasetMergeWarning(const AppSession& session, const std::vector<DatasetId>& datasets);

/// Confirm-then-merge entry point shared by the Source Timeline footer and the
/// curve tree's context menu. Filters `ids` to the data-bearing datasets; if ≥2
/// remain, shows the destructive-merge warning (composeDatasetMergeWarning) as a
/// question parented to `parent`. On confirm, runs AppSession::mergeDatasets and
/// passes the surviving anchor id to `on_merged`, which never runs on fewer than
/// two data-bearing datasets, on cancel/Esc, or a no-op merge. The browser's
/// question is non-blocking, so `on_merged` may run after this returns and
/// `session` must outlive the dialog.
void confirmAndMergeDatasets(
    QWidget* parent, AppSession& session, const std::vector<DatasetId>& ids,
    std::function<void(DatasetId anchor)> on_merged);

}  // namespace PJ
