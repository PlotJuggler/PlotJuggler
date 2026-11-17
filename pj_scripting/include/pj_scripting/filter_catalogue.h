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

/// Qt-free registry of self-describing filter classes. The only source is the bundled multi-class resource,
/// whose text the host passes to `addBundledSource`; no directory is scanned.
class FilterCatalogue {
 public:
  explicit FilterCatalogue(std::shared_ptr<ScriptEngine> engine);

  /// Compiles `source` and registers every class it declares; a duplicate id is skipped (first wins). Returns the
  /// number added, or the compile or shape error.
  Expected<std::size_t> addBundledSource(std::string source, std::string origin);

  [[nodiscard]] const std::vector<CatalogueEntry>& entries() const {
    return entries_;
  }
  [[nodiscard]] const CatalogueEntry* find(std::string_view id) const;

  /// A configured `LuaSisoTransform` for `id`; error if `id` is unknown.
  [[nodiscard]] Expected<std::unique_ptr<proc::DataProcessor>> makeProcessor(
      std::string_view id, const std::string& params_json) const;

  /// A configured filter from an embedded module `source` (a layout's `<source_fallback>`), class found by `id`: the
  /// restore path when `id` is not in the catalogue. Runs in the same sandbox and budgets as a bundled class. Error if
  /// the source does not compile or declares no class `id`.
  [[nodiscard]] Expected<std::unique_ptr<proc::DataProcessor>> makeProcessorFromSource(
      const std::string& source, std::string_view id, const std::string& params_json) const;

  /// The N→M sibling of `makeProcessorFromSource`: a `LuaMimoTransform` (a `PJ::IMIMOTransform`, not a
  /// `proc::DataProcessor`) declaring `num_outputs` output topics.
  [[nodiscard]] Expected<std::unique_ptr<LuaMimoTransform>> makeMimoFromSource(
      const std::string& source, std::string_view id, const std::string& params_json, std::size_t num_outputs) const;

  /// A Luau module returning just class `id`, whose `create(params)` fills every parameter the caller leaves out with
  /// its declared default: how a catalogued filter runs as a plugin-style transform (`DataProcessorService::
  /// upsertTransform`), which passes params through unfilled. nullopt for an unknown id.
  [[nodiscard]] std::optional<std::string> standaloneClassScript(std::string_view id) const;

 private:
  std::shared_ptr<ScriptEngine> engine_;
  std::vector<CatalogueEntry> entries_;
};

}  // namespace PJ::scripting
