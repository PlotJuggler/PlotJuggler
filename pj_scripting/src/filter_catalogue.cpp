// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scripting/filter_catalogue.h"

#include <algorithm>
#include <cctype>
#include <utility>

#include "pj_scripting/lua_mimo_transform.h"
#include "pj_scripting/lua_siso_transform.h"

namespace PJ::scripting {

FilterCatalogue::FilterCatalogue(std::shared_ptr<ScriptEngine> engine) : engine_(std::move(engine)) {}

Expected<std::size_t> FilterCatalogue::addBundledSource(std::string source, std::string origin) {
  auto classes = engine_->inspectModule(source, origin);
  if (!classes.has_value()) {
    return PJ::unexpected(classes.error());
  }
  std::size_t added = 0;
  for (FilterClass& cls : classes.value()) {
    if (find(cls.id) != nullptr) {
      continue;  // duplicate id — first registration wins
    }
    entries_.push_back(CatalogueEntry{std::move(cls), origin, /*bundled=*/true});
    ++added;
  }
  return added;
}

const CatalogueEntry* FilterCatalogue::find(std::string_view id) const {
  for (const CatalogueEntry& e : entries_) {
    if (e.cls.id == id) {
      return &e;
    }
  }
  return nullptr;
}

Expected<std::unique_ptr<proc::DataProcessor>> FilterCatalogue::makeProcessor(
    std::string_view id, const std::string& params_json) const {
  const CatalogueEntry* entry = find(id);
  if (entry == nullptr) {
    return PJ::unexpected(std::string("unknown filter id '") + std::string(id) + "'");
  }
  return std::unique_ptr<proc::DataProcessor>(std::make_unique<LuaSisoTransform>(engine_, entry->cls, params_json));
}

Expected<std::unique_ptr<proc::DataProcessor>> FilterCatalogue::makeProcessorFromSource(
    const std::string& source, std::string_view id, const std::string& params_json) const {
  auto classes = engine_->inspectModule(source, "embedded");
  if (!classes.has_value()) {
    return PJ::unexpected(classes.error());
  }
  for (FilterClass& cls : classes.value()) {
    if (cls.id == id) {
      return std::unique_ptr<proc::DataProcessor>(
          std::make_unique<LuaSisoTransform>(engine_, std::move(cls), params_json));
    }
  }
  return PJ::unexpected(std::string("embedded source has no filter id '") + std::string(id) + "'");
}

Expected<std::unique_ptr<LuaMimoTransform>> FilterCatalogue::makeMimoFromSource(
    const std::string& source, std::string_view id, const std::string& params_json, std::size_t num_outputs) const {
  auto classes = engine_->inspectModule(source, "embedded");
  if (!classes.has_value()) {
    return PJ::unexpected(classes.error());
  }
  for (FilterClass& cls : classes.value()) {
    if (cls.id == id) {
      return std::make_unique<LuaMimoTransform>(engine_, std::move(cls), params_json, num_outputs);
    }
  }
  return PJ::unexpected(std::string("embedded source has no filter id '") + std::string(id) + "'");
}

std::optional<std::string> FilterCatalogue::standaloneClassScript(std::string_view id) const {
  const CatalogueEntry* entry = find(id);
  // The id is spliced into Luau source: only identifier characters may reach it.
  if (entry == nullptr ||
      !std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isalnum(c) != 0 || c == '_'; })) {
    return std::nullopt;
  }
  const std::string quoted = "\"" + std::string(id) + "\"";
  return "local module = (function()\n" + entry->cls.source +
         "\nend)()\n"
         "local classes = module.id ~= nil and { module } or module\n"
         "for _, c in ipairs(classes) do\n"
         "  if c.id == " +
         quoted +
         " then\n"
         "    local create = c.create\n"
         "    c.create = function(given)\n"
         "      local p = {}\n"
         "      for _, d in ipairs(c.parameters or {}) do p[d.name] = d.default end\n"
         "      for k, v in pairs(given or {}) do p[k] = v end\n"
         "      return create(p)\n"
         "    end\n"
         "    return c\n"
         "  end\n"
         "end\n"
         "error(\"no filter class \" .. " +
         quoted + ")\n";
}

}  // namespace PJ::scripting
