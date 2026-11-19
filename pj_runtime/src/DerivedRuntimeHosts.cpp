// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/DerivedRuntimeHosts.h"

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pj_datastore/engine.hpp"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DatasetQualifiedName.h"
#include "pj_runtime/SessionManager.h"

namespace PJ {

DerivedRuntimeHosts::DerivedRuntimeHosts(
    SessionManager& session, CatalogModel& catalog, const std::string& plugin_id,
    std::function<void()> on_history_change)
    : session_(session),
      markers_(
          session.batchEngine(), plugin_id,
          [&catalog, &session](
              std::vector<std::string>& inputs, std::vector<std::string>& outputs) -> Expected<DatasetId> {
            // Bind the generator to the dataset its keys name (qualified keys pick it explicitly and are normalized to
            // bare form); an unqualified input that exists in several datasets is refused with the qualified
            // candidates, since landing on the first-loaded dataset would strand the generator on the wrong data. The
            // policy lives in DatasetQualifiedName so it is testable and stays in step with the series resolver
            // AppSession installs.
            return resolveMarkerDataset(inputs, outputs, catalog.items(), session.dataEngine());
          },
          [&session](DatasetId id) -> std::optional<std::string> {
            std::string label = session.dataEngine().datasetLabel(id);
            return label.empty() ? std::nullopt : std::optional{std::move(label)};
          }),
      transforms_(session.dataProcessorService(), plugin_id),
      on_demand_(
          session.onDemandService(), session.objectStore(), plugin_id, [&catalog] { catalog.rebuildIfChanged(); }),
      router_(markers_, transforms_, on_demand_, std::move(on_history_change)) {}

DerivedRuntimeHosts::~DerivedRuntimeHosts() {
  session_.removeEphemerals(transforms_.previews(), markers_.previews(), on_demand_.previews());
}

Status DerivedRuntimeHosts::registerServices(ServiceRegistryBuilder& builder) {
  // ONE pj.data_processors.v1 registration, routed by kind: the registry rejects
  // duplicate names, so registering each bridge directly would drop all but the first.
  return router_.registerServices(builder);
}

}  // namespace PJ
