// SPDX-License-Identifier: MPL-2.0

#include "IngestProgressWiring.h"

#include "IngestProgressController.h"
#include "pj_runtime/CatalogModel.h"
#include "ui/CurveListPanel.h"

namespace PJ {

void wireIngestProgress(
    SessionManager& session, CatalogModel& catalog, IngestProgressController& controller, CurveListPanel& panel,
    QObject* context) {
  controller.setSessionManager(&session);

  // Maps dataset ids to their catalog display labels — the tree paths the rows
  // are filed under. An id with no row yet resolves to an empty string, which
  // legitimately leaves the entry on a ghost row until a row appears.
  controller.setDatasetPathResolver([&catalog](DatasetId dataset_id) -> QString {
    for (const auto& [id, label] : catalog.datasets()) {
      if (id == dataset_id) {
        return label;
      }
    }
    return {};
  });

  // A dataset's catalog row appears partway through its own load (at the first
  // flush) or, for a short load, only after the last progress tick. Re-resolve
  // whenever items land so a ghost row converts the moment its dataset has a
  // real row, instead of shadowing it for the rest of the load.
  QObject::connect(&catalog, &CatalogModel::itemsAdded, &controller, [&controller](const auto&) {
    controller.refreshDatasetPaths();
  });
  QObject::connect(&catalog, &CatalogModel::itemAdded, &controller, [&controller](const auto&) {
    controller.refreshDatasetPaths();
  });
  QObject::connect(
      &controller, &IngestProgressController::progressUpdated, &panel, &CurveListPanel::setDatasetProgress);
  QObject::connect(&controller, &IngestProgressController::rowKeyChanged, &panel, &CurveListPanel::setDatasetRowKey);

  // The clicked row names an ingest, never "some cancellable ingest": several
  // run in parallel, so the token comes from the controller's row-key map. It
  // may already be stale (the row can be superseded or ended between the paint
  // and the click) — requestCancel is the authority on that and every other
  // refusal, so it is asked directly instead of pre-screened here.
  QObject::connect(
      &panel, &CurveListPanel::cancelRequested, context, [&session, &controller](quint64 row_key, bool keep_partial) {
        if (const std::optional<IngestToken> token = controller.tokenForRowKey(row_key); token.has_value()) {
          session.requestCancel(*token, keep_partial);
        }
      });
}

}  // namespace PJ
