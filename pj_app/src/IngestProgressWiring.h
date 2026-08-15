#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <QObject>

#include "pj_runtime/SessionManager.h"

namespace PJ {

class CatalogModel;
class CurveListPanel;
class IngestProgressController;

/// Connects the per-dataset load-progress chain: SessionManager's ingest
/// lifecycle -> IngestProgressController -> the curve list's tree decoration,
/// plus the cancel affordance route back.
///
/// This is the single definition of that wiring, so any consumer exercises the
/// connections the app actually ships rather than a re-implementation of them:
/// a dropped connection or an unforwarded flag fails in both or neither.
///
/// A clicked affordance goes straight to SessionManager::requestCancel with the
/// choice the user expressed by WHICH glyph they hit (✕ keeps what arrived, bin
/// discards it). There is no host policy to inject: the row IS the decision, and
/// requestCancel is the one place allowed to refuse it (stale, ended, not
/// cancellable, or a discard a producer cannot honour).
///
/// `catalog` must be the catalog backing `panel`; the dataset-path resolver
/// holds a reference to it. `context` scopes the cancel connection to the
/// caller's lifetime. Call once, after `panel.setCatalog().`
void wireIngestProgress(
    SessionManager& session, CatalogModel& catalog, IngestProgressController& controller, CurveListPanel& panel,
    QObject* context);

}  // namespace PJ
