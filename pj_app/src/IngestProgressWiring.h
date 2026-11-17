#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <QObject>

#include "pj_runtime/SessionManager.h"

namespace PJ {

class CatalogModel;
class CurveListPanel;
class IngestProgressController;

/// SessionManager ingests -> IngestProgressController -> the curve list's progress rows, and the rows' cancel back.
/// The one definition of this wiring, so tests exercise what the app ships. A click goes straight to
/// SessionManager::requestCancel (X keeps what arrived, the bin discards it), the only place allowed to refuse it.
/// `catalog` backs `panel` and must outlive `controller` (the resolver references it); `context` scopes the cancel
/// connection. Call once, after `panel.setCatalog()`.
void wireIngestProgress(
    SessionManager& session, CatalogModel& catalog, IngestProgressController& controller, CurveListPanel& panel,
    QObject* context);

}  // namespace PJ
