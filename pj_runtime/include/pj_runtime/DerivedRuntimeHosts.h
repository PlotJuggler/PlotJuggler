#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <functional>
#include <optional>
#include <string>

#include "pj_base/expected.hpp"
#include "pj_runtime/DataProcessorsKindRouter.h"
#include "pj_runtime/DataProcessorsRuntimeHost.h"
#include "pj_runtime/MarkersRuntimeHost.h"
#include "pj_runtime/OnDemandRuntimeHost.h"

namespace PJ {

class CatalogModel;
class ServiceRegistryBuilder;
class SessionManager;

/// The derived-data runtime hosts of ONE plugin binding: the markers bridge
/// (`kind=markers`, `BatchEngine`), the transform bridge (`DataProcessorService`), the
/// on-demand bridge (`kind=on_demand`, `OnDemandService`) and the kind router that
/// registers the single `pj.data_processors.v1` service over the three. The interactive toolbox session and the
/// headless descriptor-import session both build theirs here, so a plugin sees the same derived surface in either.
///
/// The bridges own nothing the plugin created: its persistent recipes live on in the
/// engines after this is destroyed, but its previews (ephemeral transforms and marker
/// rules) end with it: a preview belongs to the plugin instance that made it, not to
/// its plugin id, so another instance of the same plugin (a headless binding beside an
/// open panel) keeps its own. Its evaluations end with it too: the router, destroyed
/// first, releases the handles it holds, and the on-demand bridge cancels the rest. Destroy it after the plugin handle,
/// so no plugin call can create a preview afterwards, and after the service views registered into a builder. `session`
/// and `catalog` must outlive it. Not movable (the bridges' vtables store `this`).
class DerivedRuntimeHosts {
 public:
  /// A markers create binds to the dataset its input keys name (a qualified key picks
  /// it, an unqualified one found in several datasets is refused with the qualified
  /// candidates: `resolveMarkerDataset` over `catalog`); `plugin_id` namespaces every id.
  DerivedRuntimeHosts(SessionManager& session, CatalogModel& catalog, const std::string& plugin_id);

  /// Ends the previews this instance's bridges created and still hold, in the three engines
  /// (`SessionManager::removeEphemerals`); one replaced by a persistent recipe or gone
  /// already is skipped.
  ~DerivedRuntimeHosts();

  DerivedRuntimeHosts(const DerivedRuntimeHosts&) = delete;
  DerivedRuntimeHosts& operator=(const DerivedRuntimeHosts&) = delete;

  /// Registers the routed `pj.data_processors.v1` service into the plugin's registry.
  [[nodiscard]] Status registerServices(ServiceRegistryBuilder& builder);

 private:
  SessionManager& session_;
  /// The markers bridge's dataset-name vocabulary (its resolver and its config read-back).
  std::function<std::optional<std::string>(DatasetId)> source_name_of_;
  MarkersRuntimeHost markers_;
  DataProcessorsRuntimeHost transforms_;
  OnDemandRuntimeHost on_demand_;
  /// Last member, destroyed first: it holds fat pointers into the bridges and releases its evaluation handles on
  /// them, so every bridge outlives it.
  DataProcessorsKindRouter router_;
};

}  // namespace PJ
