#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QCoreApplication>
#include <QDomDocument>
#include <QDomElement>
#include <QString>
#include <map>
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
  /// The <processor> element it was parsed from (null when not parsed from a document).
  QDomElement source;
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
  /// The <generator> element it was parsed from.
  QDomElement source;
};

/// One <on_demand> entry of a snapshot, its saved input dataset resolved into `Recipe::dataset`. A non-empty
/// `parse_error` (a malformed attribute, an input dataset that does not resolve, a dependency cycle) is reported by
/// the restore, which skips the entry.
struct WantedOnDemand {
  RecipeGraph::Recipe recipe;
  QString parse_error;
  /// The <on_demand> element it was parsed from (null for a live recipe being saved).
  QDomElement source;
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
  /// The <transform> element each entry of `transforms` was parsed from, by key.
  std::map<std::string, QDomElement> transform_sources;
  /// Transform entries that failed to parse and were dropped: the restore reports
  /// them first and does not count as complete.
  std::vector<DerivedDiagnostic> diagnostics;
  /// The <transform> elements of those entries.
  std::vector<QDomElement> unparsed_transforms;
};

struct DerivedRestoreReport {
  /// False when a filter, transform or on-demand entry was not restored (it failed to parse or
  /// to install). A marker generator that failed is reported but does not count.
  bool restored_all = true;
  /// Every warning: the snapshot's parse warnings, then the apply failures in the
  /// order they happened.
  std::vector<DerivedDiagnostic> diagnostics;
  /// The saved element of every entry that failed to parse or to install (generators included), so a caller can keep
  /// exactly those. Elements of the document the snapshot was parsed from: valid while that document lives. A
  /// generator binding dropped as a duplicate of an earlier one is not listed.
  std::vector<QDomElement> skipped;
};

/// Layout persistence of the derived recipes (filters, transforms, marker generators,
/// on-demand recipes) and their one restore across the engines. Stateless: it holds the session and the
/// catalog it reads and writes, nothing else, so a caller builds one where needed.
/// GUI thread only, like the engines it drives.
///
/// The life-cycle protocol verbs map onto the engines' names: see docs/derived_objects/ARCHITECTURE.md
/// "Life cycle of recipes and results".
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
  /// Whether any recipe lives (a filter, transform, marker generator or on-demand node, previews included), without
  /// serializing them.
  [[nodiscard]] bool hasAny() const;

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

  /// Makes the live recipes what `snapshot` wants, under its intent; run `refusal` first. Unchanged sets stay live as
  /// they are (output TopicIds, a generator's topic, published set, run state, coverage): filters and transforms whose
  /// recipe the snapshot does not change, generators likewise unless they read an output this restore removes or
  /// creates (`BatchEngine::unchangedGeneratorIds`). The rest is cleared downstream to upstream (generators, filters,
  /// transforms) and installed upstream to downstream: filters and transforms in rounds, then each generator. A layout
  /// load creates with the bounded create (`CreateWait::kBounded`): a slow rule draws nothing until its run publishes,
  /// and its failure after the budget is reported by the engine (`errorOf`, the diagnostic sink), not here; an undo or
  /// redo runs it to completion. A generator naming no dataset (`WantedGenerator::bind_by_inputs`) is never kept and
  /// binds through the refreshed catalog; a later binding of one id resolved onto the same dataset is dropped, and
  /// reported when its recipe differs. History never touches a live exempt recipe. The catalog is rebuilt once the
  /// filters and transforms are installed, before any generator resolves through it, and one `markersChanged` is
  /// announced at the end when the generator set changed. A generator that cannot be restored is reported but does not
  /// fail the restore.
  ///
  /// On-demand recipes come last. A node is kept (topics, results, state) when its recipe, its upstreams and the series
  /// it reads (no processor output the restore removes or creates) all stay; the rest are cleared before the generators
  /// (their topics kept), installed after them, producers first (install requests a finding at its pin), and what no
  /// install reclaimed is retired, with one `objectTopicsChanged`. A node outside the restore's authority (a preview,
  /// an exempt one under history) reading a processor output the restore removed or created runs again, with its
  /// readers. A recipe that fails to parse or install fails the restore, as a transform does; a Python script that does
  /// not compile installs (nothing compiles Python here), its error surfacing at the node's first demanded run, never
  /// as a diagnostic.
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
      const QDomElement& element, bool skip_exempt, DerivedSnapshot& snapshot) const;
  // Parses every <processor> under `element`, resolving each input once so
  // `unchangedFilterOutputs` can match live filters; an unresolved input is not an
  // error yet.
  [[nodiscard]] std::vector<WantedFilter> parseWantedFilters(const QDomElement& element) const;
  [[nodiscard]] std::vector<WantedGenerator> parseWantedGenerators(const QDomElement& element) const;
  // Parses every <on_demand> under `element`, skipping the history-exempt entries when `skip_exempt`;
  // ordered by dependency (`DerivedSnapshot::on_demand`).
  [[nodiscard]] std::vector<WantedOnDemand> parseWantedOnDemand(const QDomElement& element, bool skip_exempt) const;

  // Re-creates every wanted filter and transform that is not live, in rounds: filters first (each input re-resolved
  // through the rebuilt catalog, so an output the previous round minted resolves), then transforms, which resolve by
  // name. A round creates only entries whose inputs exist, so a chain of N alternating links takes up to N rounds; the
  // loop stops when a round creates nothing. History has no authority over a live exempt transform
  // (`live_exempt_keys`), even when the snapshot carries its key. Each entry still not live at the end is reported
  // once, its element in `skipped`; returns whether every entry was restored.
  [[nodiscard]] bool repairDataProcessors(
      std::vector<WantedFilter>& wanted_filters, const DerivedSnapshot& snapshot,
      const std::unordered_set<std::string>& live_exempt_keys, std::vector<DerivedDiagnostic>& diagnostics,
      std::vector<QDomElement>& skipped);

  SessionManager& session_;
  CatalogModel& catalog_;
};

}  // namespace PJ
