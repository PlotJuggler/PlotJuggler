// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_datastore/data_processor.hpp"
#include "pj_scripting/filter_class.h"
#include "pj_scripting/script_engine.h"

namespace PJ::scripting {

class LuaMimoTransform;

/// One catalogued filter class plus its provenance.
struct CatalogueEntry {
  FilterClass cls;
  std::string origin;  ///< e.g. "bundled"
  bool bundled = true;
};

/// Qt-free registry of self-describing filter classes. In v1 the only source is
/// the single bundled multi-class resource (its source string is handed in at
/// init); `addBundledSource` evaluates the module and registers EVERY class it
/// declares. No filesystem scan in v1 — a user-override drop dir + marketplace
/// install dir are the DEFERRED follow-on (see `rescanUserDir`).
class FilterCatalogue {
 public:
  explicit FilterCatalogue(std::shared_ptr<ScriptEngine> engine);

  /// Compile + inspect `source` and register every class it declares. A duplicate
  /// id (already registered) is skipped (first wins). Returns the number of
  /// classes added, or an error string on a compile/shape failure.
  Expected<std::size_t> addBundledSource(std::string source, std::string origin);

  [[nodiscard]] const std::vector<CatalogueEntry>& entries() const {
    return entries_;
  }
  [[nodiscard]] const CatalogueEntry* find(std::string_view id) const;

  /// Build a configured filter for `id` as a `LuaSisoTransform` (no session arg —
  /// it captures the session epoch from the first sample). Error if `id` is unknown.
  [[nodiscard]] Expected<std::unique_ptr<proc::DataProcessor>> makeProcessor(
      std::string_view id, const std::string& params_json) const;

  /// Build a configured filter from an EMBEDDED module `source` (a layout's
  /// `<source_fallback>`), locating the class by `id` within it. Compiled +
  /// inspected through the same sandboxed engine as the bundled path, so M2
  /// budgets apply. Error if the source fails to compile or declares no class
  /// with `id`. Used by the restore path when `id` is absent from the catalogue.
  [[nodiscard]] Expected<std::unique_ptr<proc::DataProcessor>> makeProcessorFromSource(
      const std::string& source, std::string_view id, const std::string& params_json) const;

  /// Build a MIMO filter from an embedded module `source`, locating the class by
  /// `id`, as a `LuaMimoTransform` declaring `num_outputs` output topics. The N→M
  /// sibling of `makeProcessorFromSource` — returns the `IMIMOTransform`
  /// implementation (not a `proc::DataProcessor`, which is SISO-only). Error if the
  /// source fails to compile or declares no class with `id`.
  [[nodiscard]] Expected<std::unique_ptr<LuaMimoTransform>> makeMimoFromSource(
      const std::string& source, std::string_view id, const std::string& params_json, std::size_t num_outputs) const;

  /// A Luau module returning just class `id`, whose `create(params)` fills every parameter the
  /// caller leaves out with its declared default: how a catalogued filter runs as a plugin-style
  /// transform (DataProcessorService::upsertTransform), which passes params through unfilled.
  /// nullopt for an unknown id.
  [[nodiscard]] std::optional<std::string> standaloneClassScript(std::string_view id) const;

  // DEFERRED (post-v1): rescanUserDir(path) would merge a user-override file +
  // marketplace dir over the bundled set (user ids shadow bundled, except the
  // reserved builtin ids). Not built in v1.

 private:
  std::shared_ptr<ScriptEngine> engine_;
  std::vector<CatalogueEntry> entries_;
};

}  // namespace PJ::scripting
