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

/// Layout persistence of the derived recipes (filters, transforms, marker generators)
/// and their one restore across the engines. Stateless: it holds the session and the
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
  /// `scope == kFull` tags a history-exempt <transform>/<generator> with
  /// `history_exempt="1"` so it round-trips through a layout file; `kHistory` omits
  /// those entries outright (see `SnapshotScope`).
  [[nodiscard]] QDomElement save(QDomDocument& doc, SnapshotScope scope) const;

  /// Parses the <data_processors> child of `root` (a missing one wants nothing) for a
  /// restore with `intent`. Under `kHistory` it skips entries flagged
  /// `history_exempt="1"`: history has no authority to replay one. Query-only.
  [[nodiscard]] DerivedSnapshot parse(const QDomElement& root, RestoreIntent intent) const;

  /// The reason a `kHistory` restore of `snapshot` must be refused before anything is
  /// cleared: removing its non-exempt processors would strand an exempt transform or
  /// marker generator. nullopt outside `kHistory`, when nothing exempt is live, or when
  /// nothing would strand. Query-only.
  [[nodiscard]] std::optional<QString> refusal(const DerivedSnapshot& snapshot) const;

  /// Makes the live recipes what `snapshot` wants, under its intent. First the
  /// unchanged sets, upstream to downstream: filters and transforms whose recipe the
  /// snapshot does not change, then generators likewise unless they read an output
  /// this restore removes or creates (`BatchEngine::unchangedGeneratorIds`). Those stay
  /// live as they are (output TopicIds, a generator's topic, published set, run state
  /// and coverage). Then the rest is cleared downstream to upstream (generators,
  /// filters, transforms) and installed upstream to downstream: filters and transforms
  /// in rounds, then each generator with the bounded create (`CreateWait::kBounded`):
  /// a slow rule draws nothing until its run publishes, and its failure after the
  /// budget is reported by the engine (`errorOf`, the diagnostic sink), not here.
  /// History never touches a live exempt recipe. Ends with a catalog rebuild and, when
  /// the generator set changed, one `markersChanged`. A generator that cannot be
  /// restored is reported but does not fail the restore. Run `refusal` first for
  /// `kHistory`.
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
