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

/// The merge warning (rich text): it is destructive, plus which datasets overlap in displayed time, which of those
/// share a topic name, and which carry object topics. Built from the runtime alone, not from any widget's tracks.
[[nodiscard]] QString composeDatasetMergeWarning(const AppSession& session, const std::vector<DatasetId>& datasets);

/// The one merge entry point (Source Timeline and curve tree). Keeps the data-bearing datasets of `ids`; with two or
/// more, refuses a live streaming dataset or conflicting object topics, else asks, then AppSession::mergeDatasets.
/// `on_merged` gets the surviving anchor, and runs only after a merge that did something. The question is
/// non-blocking: `on_merged` may run after this returns, and `session` must outlive the dialog.
void confirmAndMergeDatasets(
    QWidget* parent, AppSession& session, const std::vector<DatasetId>& ids,
    std::function<void(DatasetId anchor)> on_merged);

}  // namespace PJ
