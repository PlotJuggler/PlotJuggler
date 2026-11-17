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

  controller.setDatasetPathResolver([&catalog](DatasetId dataset_id) -> QString {
    for (const auto& [id, label] : catalog.datasets()) {
      if (id == dataset_id) {
        return label;
      }
    }
    return {};
  });

  // A dataset's row appears at its first flush, or for a short load after the last tick.
  QObject::connect(&catalog, &CatalogModel::itemsAdded, &controller, [&controller](const auto&) {
    controller.refreshDatasetPaths();
  });
  QObject::connect(&catalog, &CatalogModel::itemAdded, &controller, [&controller](const auto&) {
    controller.refreshDatasetPaths();
  });
  QObject::connect(
      &controller, &IngestProgressController::progressUpdated, &panel, &CurveListPanel::setDatasetProgress);
  QObject::connect(&controller, &IngestProgressController::rowKeyChanged, &panel, &CurveListPanel::setDatasetRowKey);

  // The token may be stale by the click; requestCancel decides, so it is not pre-screened here.
  QObject::connect(
      &panel, &CurveListPanel::cancelRequested, context, [&session, &controller](quint64 row_key, bool keep_partial) {
        if (const std::optional<IngestToken> token = controller.tokenForRowKey(row_key); token.has_value()) {
          session.requestCancel(*token, keep_partial);
        }
      });
}

}  // namespace PJ
