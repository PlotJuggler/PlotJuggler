// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/DerivedRuntimeHosts.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DatasetQualifiedName.h"
#include "pj_runtime/SessionManager.h"

namespace PJ {

namespace {

// Dataset → raw source name: the qualifier vocabulary of a markers input key.
std::function<std::optional<std::string>(DatasetId)> sourceNameOf(CatalogModel& catalog) {
  return [&catalog](DatasetId id) -> std::optional<std::string> {
    const std::optional<QString> src = catalog.datasetSourceName(id);
    return src.has_value() ? std::optional<std::string>(src->toStdString()) : std::nullopt;
  };
}

}  // namespace

DerivedRuntimeHosts::DerivedRuntimeHosts(SessionManager& session, CatalogModel& catalog, const std::string& plugin_id)
    : session_(session),
      source_name_of_(sourceNameOf(catalog)),
      markers_(
          session.batchEngine(), plugin_id,
          [&catalog, this](std::vector<std::string>& inputs, std::vector<std::string>& outputs) -> Expected<DatasetId> {
            // Bind the generator to the dataset its keys name (qualified keys pick it
            // explicitly and are normalized to bare form); an unqualified input that
            // exists in several datasets is refused with the qualified candidates —
            // landing on the first-loaded dataset silently stranded generators on the
            // wrong data. Policy lives in DatasetQualifiedName so it is testable and
            // stays in step with the series resolver AppSession installs.
            std::vector<DatasetId> datasets;
            for (const auto& [id, label] : catalog.datasets()) {
              datasets.push_back(id);
            }
            return resolveMarkerDataset(inputs, outputs, catalog.items(), datasets, source_name_of_);
          },
          source_name_of_),
      transforms_(session.dataProcessorService(), plugin_id),
      on_demand_(
          session.onDemandService(), session.objectStore(), plugin_id, [&catalog] { catalog.rebuildIfChanged(); }),
      router_(markers_, transforms_, on_demand_) {}

DerivedRuntimeHosts::~DerivedRuntimeHosts() {
  session_.removeEphemerals(transforms_.previews(), markers_.previews(), on_demand_.previews());
}

Status DerivedRuntimeHosts::registerServices(ServiceRegistryBuilder& builder) {
  // ONE pj.data_processors.v1 registration, routed by kind: the registry rejects
  // duplicate names, so registering each bridge directly would drop all but the first.
  return router_.registerServices(builder);
}

}  // namespace PJ
