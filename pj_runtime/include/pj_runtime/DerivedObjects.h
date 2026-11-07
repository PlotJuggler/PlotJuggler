#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QCoreApplication>
#include <QDomDocument>
#include <QDomElement>
#include <QString>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "pj_runtime/BatchEngine.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/DataProcessorService.h"
#include "pj_runtime/HistoryScope.h"
#include "pj_runtime/RecipeGraph.h"

namespace PJ {

class SessionManager;

/// One warning a parse or a restore wants shown. `id` is a static diagnostic id
/// ("transform-restore-failed", ...); the caller reports it under source "Layout".
struct DerivedDiagnostic {
  const char* id = "";
  QString message;
};

/// One <processor> entry of a snapshot: the input path as persisted plus the
/// service-side description with that input resolved against the current
/// catalog (input fields zero while it does not resolve). The path is kept
/// because the restore re-resolves it each round: the input may be an output
/// that only a later step of the same restore creates.
struct WantedFilter {
  SavedSeriesPath input;
  DataProcessorService::PersistedFilter persisted;
};

/// One <generator> entry of a snapshot, its dataset identity resolved against the
/// current session. A non-empty `parse_error` is reported by the restore, which skips
/// the entry, unless history has no authority over it anyway.
struct WantedGenerator {
  BatchEngine::GeneratorRecipe recipe;
  QString parse_error;
  /// The entry names no dataset (a generic layout; never `all_datasets`), so
  /// `recipe.dataset_id` is not set yet: the restore binds it to the one loaded dataset
  /// carrying every input, after its transforms exist, since an input may be one's output.
  bool bind_by_inputs = false;
};

/// One <on_demand> entry of a snapshot, its saved input dataset resolved into `Recipe::dataset`. A non-empty
/// `parse_error` (a malformed attribute, an input dataset that does not resolve, a dependency cycle) is reported by
/// the restore, which skips the entry.
struct WantedOnDemand {
  RecipeGraph::Recipe recipe;
  QString parse_error;
};

/// What a <data_processors> block wants live. Parsing only queries the session, so a
/// snapshot can drive a refusal before anything is cleared. The default (empty,
/// `kReplace`) snapshot wants nothing: restoring it clears every recipe.
struct DerivedSnapshot {
  /// The authority it was parsed for, which its refusal and restore apply: a
  /// `kHistory` parse skips exempt entries, so it must never drive a `kReplace` restore.
  RestoreIntent intent = RestoreIntent::kReplace;
  std::vector<WantedFilter> filters;
  /// Producers before dependents (`orderTransformsByDependency`).
  std::vector<DataProcessorService::TransformRecipe> transforms;
  std::vector<WantedGenerator> generators;
  /// Producers before readers (a reader names its upstream `<key>/<output>`, maybe qualified); the entries that failed
  /// to parse at the end.
  std::vector<WantedOnDemand> on_demand;
  /// Transform entries that failed to parse and were dropped: the restore reports
  /// them first and does not count as complete.
  std::vector<DerivedDiagnostic> diagnostics;
};

struct DerivedRestoreReport {
  /// False when a filter or transform entry was not restored (it failed to parse or
  /// to install). A marker generator that failed is reported but does not count.
  bool restored_all = true;
  /// Every warning: the snapshot's parse warnings, then the apply failures in the
  /// order they happened.
  std::vector<DerivedDiagnostic> diagnostics;
};

/// Layout persistence of the derived recipes (filters, transforms, marker generators,
/// on-demand recipes) and their one restore across the engines. Stateless: it holds the session and the
/// catalog it reads and writes, nothing else, so a caller builds one where needed.
/// GUI thread only, like the engines it drives.
///
/// The ARCHITECTURE §13 protocol verbs map onto the engines' existing names; see
/// docs/derived_objects/ARCHITECTURE.md §13.
class DerivedObjects {
  Q_DECLARE_TR_FUNCTIONS(DerivedObjects)

 public:
  /// Both must outlive this object.
  DerivedObjects(SessionManager& session, CatalogModel& catalog);

  /// Serializes the live recipes into a <data_processors> block: per-curve filters as
  /// <processor> entries (INPUT stored as a stable (topic, field) path so it rebinds on
  /// reload) and plugin-created transforms and marker generators as <transform> /
  /// <generator> entries (script + bindings carried by value, inputs/outputs by name).
  /// On-demand recipes are <on_demand> entries (script, inputs with their dataset, what a
  /// qualified input reads as `topic` and, for another node's output, its node and type;
  /// typed outputs, pin, query clock), producers first; a preview is never written.
  /// `scope == kFull` tags a history-exempt <transform>/<generator>/<on_demand> with
  /// `history_exempt="1"` so it round-trips through a layout file; `kHistory` omits
  /// those entries outright (see `SnapshotScope`).
  [[nodiscard]] QDomElement save(QDomDocument& doc, SnapshotScope scope) const;

  /// Parses the <data_processors> child of `root` (a missing one wants nothing) for a
  /// restore with `intent`. Under `kHistory` it skips entries flagged
  /// `history_exempt="1"`: history has no authority to replay one. Query-only.
  [[nodiscard]] DerivedSnapshot parse(const QDomElement& root, RestoreIntent intent) const;

  /// The reason a restore of `snapshot` must be refused before anything is cleared. Under
  /// either intent: a preview holds the key of an on-demand recipe it wants (no restore
  /// clears a preview, so the recipe would be skipped). Under `kHistory`: removing its
  /// non-exempt processors would strand an exempt transform or marker generator, or it
  /// takes away an output an exempt on-demand recipe reads (a processor's, or an on-demand
  /// node's object output: its node dropped, moved to another dataset or no longer
  /// declaring it). nullopt otherwise. Query-only.
  [[nodiscard]] std::optional<QString> refusal(const DerivedSnapshot& snapshot) const;

  /// Makes the live recipes what `snapshot` wants, under its intent. First the
  /// unchanged sets, upstream to downstream: filters and transforms whose recipe the
  /// snapshot does not change, then generators likewise unless they read an output
  /// this restore removes or creates (`BatchEngine::unchangedGeneratorIds`). Those stay
  /// live as they are (output TopicIds, a generator's topic, published set, run state
  /// and coverage). Then the rest is cleared downstream to upstream (generators,
  /// filters, transforms) and installed upstream to downstream: filters and transforms
  /// in rounds, then each generator. A layout load creates it with the bounded create
  /// (`CreateWait::kBounded`): a slow rule draws nothing until its run publishes, and its
  /// failure after the budget is reported by the engine (`errorOf`, the diagnostic
  /// sink), not here. An undo or redo runs it to completion. A generator naming no
  /// dataset (`WantedGenerator::bind_by_inputs`) is never kept and binds then, through
  /// the refreshed catalog; a later binding of one id resolved onto the same dataset is
  /// dropped, and reported when its recipe differs. History never touches a live exempt
  /// recipe. Rebuilds the catalog once the filters and transforms are installed, before
  /// any generator resolves through it, and announces one `markersChanged` at the end when
  /// the generator set changed. A generator that cannot be restored is reported but does
  /// not fail the restore. On-demand recipes come last: a node is kept when its recipe,
  /// its upstreams and the series it reads (no processor output the restore removes or
  /// creates) all stay, with its topics, results and state; the rest are cleared before
  /// the generators (their topics kept), installed after them, producers first (the
  /// install requests a finding at its pin), and what no install reclaimed is retired, with
  /// one `objectTopicsChanged`; a node outside the restore's authority (a preview, an exempt
  /// one under history) reading a processor output the restore removed or created runs
  /// again, with its readers. One that fails to parse or install fails the restore, as a
  /// transform does; a Python script that does not compile installs (nothing compiles Python
  /// here), its error surfacing at the node's first demanded run, never as a diagnostic. Run
  /// `refusal` first.
  [[nodiscard]] DerivedRestoreReport restore(DerivedSnapshot snapshot);

 private:
  // What a restore of a snapshot leaves live: the filters (by output TopicId) and the
  // transforms (by key) it describes unchanged.
  struct KeepSets {
    std::unordered_set<TopicId> filter_outputs;
    std::unordered_set<std::string> transform_keys;
  };
  [[nodiscard]] KeepSets keepSets(const DerivedSnapshot& snapshot) const;

  // Parses every <transform> under `element`, resolving persisted dataset identities
  // and skipping the history-exempt entries when `skip_exempt`. An entry that fails to
  // parse is reported into `diagnostics` and dropped.
  [[nodiscard]] std::vector<DataProcessorService::TransformRecipe> parseWantedTransforms(
      const QDomElement& element, bool skip_exempt, std::vector<DerivedDiagnostic>& diagnostics) const;
  // Parses every <processor> under `element`, resolving each input once so
  // `unchangedFilterOutputs` can match live filters; an unresolved input is not an
  // error yet.
  [[nodiscard]] std::vector<WantedFilter> parseWantedFilters(const QDomElement& element) const;
  [[nodiscard]] std::vector<WantedGenerator> parseWantedGenerators(const QDomElement& element) const;
  // Parses every <on_demand> under `element`, skipping the history-exempt entries when `skip_exempt`;
  // ordered by dependency (`DerivedSnapshot::on_demand`).
  [[nodiscard]] std::vector<WantedOnDemand> parseWantedOnDemand(const QDomElement& element, bool skip_exempt) const;

  // Re-creates every wanted filter and transform that is not live, in rounds:
  // filters first, each input re-resolved through the catalog (rebuilt so an
  // output the previous round minted resolves), then transforms, which resolve
  // by name. A round can only create the entries whose inputs exist, so a chain
  // of N alternating links takes up to N rounds; the loop stops as soon as a
  // round creates nothing. History has no authority over a live exempt transform
  // (`live_exempt_keys`), even when the snapshot carries its key. Each entry
  // that is still not live at the end is reported once; returns whether every
  // entry was restored.
  [[nodiscard]] bool repairDataProcessors(
      std::vector<WantedFilter>& wanted_filters,
      const std::vector<DataProcessorService::TransformRecipe>& wanted_transforms,
      const std::unordered_set<std::string>& live_exempt_keys, std::vector<DerivedDiagnostic>& diagnostics);

  SessionManager& session_;
  CatalogModel& catalog_;
};

}  // namespace PJ
