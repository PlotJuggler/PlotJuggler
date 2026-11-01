// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/FeatureCatalogHost.h"

#include <QMap>
#include <algorithm>
#include <map>
#include <string_view>
#include <type_traits>
#include <utility>

#include "pj_base/data_source_protocol.h"
#include "pj_marketplace/extension_manager.hpp"
#include "pj_marketplace/installed_extension.hpp"
#include "pj_marketplace/platform_utils.hpp"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/ToolboxTags.h"

namespace PJ {

namespace {

constexpr int kSchema = 1;

// Bounds, in code points. A plugin or a custom registry supplies most of these
// strings, so nothing reaches the document unbounded.
constexpr size_t kMaxIdChars = 128;
constexpr size_t kMaxNameChars = 128;
constexpr size_t kMaxVersionChars = 64;
constexpr size_t kMaxCategoryChars = 64;
constexpr size_t kMaxSummaryChars = 600;
constexpr size_t kMaxEntryPointChars = 300;
// Per list: a registry is open-ended, the document is not.
constexpr size_t kMaxListEntries = 256;

constexpr std::string_view kFileImportEntry = "Sources panel → File (or File → Open Data File...)";
constexpr std::string_view kParserEntry =
    "No panel of its own: chosen as the message format inside the data source that uses it (for example a "
    "stream's message serialization selector).";

size_t codePoints(std::string_view text) {
  return static_cast<size_t>(
      std::count_if(text.begin(), text.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0U) != 0x80U; }));
}

// An id is never truncated: two long ids cut to the same prefix would land in
// the document as one. One past the bound is dropped instead.
bool idFits(std::string_view id) {
  return !id.empty() && codePoints(id) <= kMaxIdChars;
}

// Truncates to at most `max_chars` code points, ending in "…" when cut. Never
// splits a multi-byte sequence; invalid bytes are replaced at serialization.
std::string bounded(std::string_view text, size_t max_chars) {
  size_t chars = 0;
  size_t cut = text.size();
  for (size_t i = 0; i < text.size(); ++i) {
    if ((static_cast<unsigned char>(text[i]) & 0xC0U) == 0x80U) {
      continue;  // continuation byte
    }
    if (chars == max_chars - 1) {
      cut = i;  // where the ellipsis goes if the text turns out too long
    }
    if (++chars > max_chars) {
      return std::string(text.substr(0, cut)) + "…";
    }
  }
  return std::string(text);
}

std::string stringField(const nlohmann::json& object, const char* key) {
  const auto it = object.find(key);
  return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool isBlank(std::string_view text) {
  return std::all_of(text.begin(), text.end(), [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; });
}

PluginFamily familyForCategory(std::string_view category) {
  if (category == "data_loader" || category == "data_stream") {
    return PluginFamily::kDataSource;
  }
  if (category == "message_parser") {
    return PluginFamily::kMessageParser;
  }
  if (category == "toolbox") {
    return PluginFamily::kToolbox;
  }
  return PluginFamily::kUnknown;
}

std::vector<std::string> toStdStrings(const QStringList& list) {
  std::vector<std::string> out;
  out.reserve(static_cast<size_t>(list.size()));
  for (const QString& item : list) {
    out.push_back(item.toStdString());
  }
  return out;
}

FeatureCatalogHost::Plugin fromExtension(const Extension& ext) {
  FeatureCatalogHost::Plugin plugin;
  plugin.id = ext.id.toStdString();
  plugin.name = ext.name.toStdString();
  plugin.version = ext.version.toStdString();
  plugin.category = ext.category.toStdString();
  plugin.family = familyForCategory(plugin.category);
  plugin.summary = ext.description.toStdString();
  plugin.entry_point = ext.entry_point.toStdString();
  plugin.tags = toStdStrings(ext.tags);
  return plugin;
}

FeatureCatalogHost::Plugin fromManifest(
    PluginFamily family, const std::string& id, const std::string& name, const std::string& version,
    const char* manifest_json) {
  FeatureCatalogHost::Plugin plugin;
  plugin.family = family;
  plugin.id = id;
  plugin.name = name;
  plugin.version = version;
  // A null or malformed manifest is a load problem reported elsewhere; the
  // plugin is still listed, with what the catalog knows about it.
  const auto manifest = manifest_json != nullptr
                            ? nlohmann::json::parse(manifest_json, nullptr, /*allow_exceptions=*/false)
                            : nlohmann::json();
  if (manifest.is_object()) {
    plugin.category = stringField(manifest, "category");
    plugin.summary = stringField(manifest, "description");
    plugin.entry_point = stringField(manifest, "entry_point");
    plugin.tags = manifestTags(manifest);
  }
  return plugin;
}

bool hasTag(const FeatureCatalogHost::Plugin& plugin, std::string_view tag) {
  return std::find(plugin.tags.begin(), plugin.tags.end(), tag) != plugin.tags.end();
}

// The fields every plugin list shares.
nlohmann::json pluginJson(const FeatureCatalogHost::Plugin& plugin) {
  return {
      {"id", plugin.id},  // callers admit only ids that fit (idFits)
      {"name", bounded(plugin.name.empty() ? plugin.id : plugin.name, kMaxNameChars)},
      {"version", bounded(plugin.version, kMaxVersionChars)},
      {"category", bounded(plugin.category, kMaxCategoryChars)},
      {"summary", bounded(plugin.summary, kMaxSummaryChars)},
  };
}

nlohmann::json navigablePluginJson(const FeatureCatalogHost::Plugin& plugin) {
  nlohmann::json entry = pluginJson(plugin);
  entry["entry_point"] = bounded(
      isBlank(plugin.entry_point) ? FeatureCatalogHost::defaultEntryPoint(plugin) : plugin.entry_point,
      kMaxEntryPointChars);
  return entry;
}

const char* reasonName(FeatureCatalogHost::Reason reason) {
  switch (reason) {
    case FeatureCatalogHost::Reason::kDisabled:
      return "disabled";
    case FeatureCatalogHost::Reason::kNeedsRestart:
      return "needs_restart";
    case FeatureCatalogHost::Reason::kIncompatible:
      return "incompatible";
    case FeatureCatalogHost::Reason::kUnknown:
      break;
  }
  return nullptr;
}

// Copies the end-user fields of one resource list, sorted by id. Anything else
// an entry carries (evidence, verified) is for maintainers and stays behind.
// The input is the app's own compiled-in file, held to its shape by
// FeatureCatalogResourceTest.
nlohmann::json publicEntries(const nlohmann::json& resource, const char* list, bool with_navigation) {
  std::map<std::string, nlohmann::json> by_id;
  for (const auto& entry : resource.value(list, nlohmann::json::array())) {
    std::string id = stringField(entry, "id");
    nlohmann::json out = {{"id", id}, {"summary", bounded(stringField(entry, "summary"), kMaxSummaryChars)}};
    if (with_navigation) {
      out["name"] = bounded(stringField(entry, "name"), kMaxNameChars);
      out["entry_point"] = bounded(stringField(entry, "entry_point"), kMaxEntryPointChars);
    }
    by_id.emplace(std::move(id), std::move(out));
  }
  nlohmann::json array = nlohmann::json::array();
  for (auto& [id, entry] : by_id) {
    array.push_back(std::move(entry));
  }
  return array;
}

}  // namespace

FeatureCatalogHost::FeatureCatalogHost(
    Sources sources, QByteArray core_json, std::string host_version, std::string platform)
    : sources_(std::move(sources)),
      host_version_(std::move(host_version)),
      platform_(std::move(platform)),
      adapter_([this]() { return document(); }) {
  const auto resource = nlohmann::json::parse(core_json.begin(), core_json.end(), nullptr, /*allow_exceptions=*/false);
  if (resource.is_object()) {
    core_ = publicEntries(resource, "core", /*with_navigation=*/true);
    limitations_ = publicEntries(resource, "limitations", /*with_navigation=*/false);
  }
}

void FeatureCatalogHost::setStartupScanSkipped() {
  startup_scan_skipped_ = true;
}

void FeatureCatalogHost::registryFetchStarted(const QUrl& url) {
  pending_url_ = url;
}

void FeatureCatalogHost::registryFetchFinished(const QUrl& url, bool ok, QList<Extension> extensions) {
  if (pending_url_ == url) {
    pending_url_.reset();
  }
  outcomes_.insert(url, ok ? Outcome(std::move(extensions)) : std::nullopt);
}

FeatureCatalogHost::RegistryStatus FeatureCatalogHost::registryStatus() const {
  const QUrl current = sources_.registry_url();
  const auto outcome = outcomes_.constFind(current);
  const bool known = outcome != outcomes_.cend();
  RegistryStatus status;
  if (known && outcome->has_value()) {
    // A refetch in flight does not hide the list already in hand.
    status.list = &**outcome;
    status.state = "fetched";
  } else if (pending_url_ == current) {
    status.state = "pending";
  } else if (known) {
    status.state = "unreachable";  // the last fetch of this URL failed
  } else if (!outcomes_.isEmpty() || pending_url_.has_value()) {
    status.reason = "url_changed";
  } else if (startup_scan_skipped_) {
    status.reason = "updates_disabled";
  }
  return status;
}

std::string FeatureCatalogHost::document() const {
  const RegistryStatus registry = registryStatus();

  std::map<std::string, const Extension*> registry_by_id;
  if (registry.list != nullptr) {
    for (const Extension& ext : *registry.list) {
      std::string id = ext.id.toStdString();
      if (idFits(id)) {
        registry_by_id.emplace(std::move(id), &ext);
      }
    }
  }

  // Precedence loaded > installed_not_loaded > available; std::map keeps each
  // list sorted by id and the first occurrence of a duplicate id.
  std::map<std::string, Plugin> loaded;
  for (Plugin& plugin : sources_.loaded()) {
    if (idFits(plugin.id)) {
      loaded.emplace(plugin.id, std::move(plugin));
    }
  }
  std::map<std::string, Installed> installed;
  for (Installed& record : sources_.installed()) {
    if (idFits(record.plugin.id) && !loaded.contains(record.plugin.id)) {
      installed.emplace(record.plugin.id, std::move(record));
    }
  }

  nlohmann::json loaded_json = nlohmann::json::array();
  for (const auto& [id, plugin] : loaded) {
    if (loaded_json.size() == kMaxListEntries) {
      break;
    }
    nlohmann::json entry = navigablePluginJson(plugin);
    entry["in_registry"] = registry_by_id.contains(id);
    loaded_json.push_back(std::move(entry));
  }

  nlohmann::json installed_json = nlohmann::json::array();
  for (const auto& [id, record] : installed) {
    if (installed_json.size() == kMaxListEntries) {
      break;
    }
    nlohmann::json entry = pluginJson(record.plugin);
    if (const char* name = reasonName(record.reason); name != nullptr) {
      entry["reason"] = name;
    }
    installed_json.push_back(std::move(entry));
  }

  nlohmann::json available_json = nlohmann::json::array();
  for (const auto& [id, ext] : registry_by_id) {
    if (available_json.size() == kMaxListEntries) {
      break;
    }
    if (loaded.contains(id) || installed.contains(id)) {
      continue;
    }
    if (!sources_.installable(*ext)) {
      continue;
    }
    available_json.push_back(navigablePluginJson(fromExtension(*ext)));
  }

  nlohmann::json doc = {
      {"schema", kSchema},
      {"host_version", bounded(host_version_, kMaxVersionChars)},
      {"platform", bounded(platform_, kMaxCategoryChars)},
      {"core", core_},
      {"limitations", limitations_},
      {"loaded", std::move(loaded_json)},
      {"installed_not_loaded", std::move(installed_json)},
      {"available", std::move(available_json)},
      {"registry", {{"state", registry.state}, {"reason", registry.reason}}},
  };
  return doc.dump(-1, ' ', /*ensure_ascii=*/false, nlohmann::json::error_handler_t::replace);
}

std::vector<FeatureCatalogHost::Plugin> FeatureCatalogHost::loadedPlugins(const ExtensionCatalogService& catalog) {
  std::vector<Plugin> out;
  const auto append = [&out](PluginFamily family, const auto& collection) {
    for (const auto& item : collection) {
      const auto* vtable = item.library.vtable();
      Plugin plugin =
          fromManifest(family, item.id, item.name, item.version, vtable != nullptr ? vtable->manifest_json : nullptr);
      if constexpr (std::is_same_v<std::decay_t<decltype(item)>, RuntimeDataSourcePlugin>) {
        plugin.source_capabilities = item.capabilities;
      }
      out.push_back(std::move(plugin));
    }
  };
  append(PluginFamily::kDataSource, catalog.dataSources());
  append(PluginFamily::kToolbox, catalog.toolboxes());
  append(PluginFamily::kMessageParser, catalog.messageParsers());
  return out;
}

std::vector<FeatureCatalogHost::Installed> FeatureCatalogHost::installedRecords(const ExtensionManager& manager) {
  // A staged removal is no longer in installedExtensions() but is still on
  // disk until the restart.
  const QMap<QString, InstalledExtension> staged = manager.stagedUninstalls();
  QMap<QString, InstalledExtension> records = manager.installedExtensions();
  for (auto it = staged.cbegin(); it != staged.cend(); ++it) {
    if (!records.contains(it.key())) {
      records.insert(it.key(), it.value());
    }
  }

  std::vector<Installed> out;
  out.reserve(static_cast<size_t>(records.size()));
  for (const InstalledExtension& record : records) {
    Installed entry;
    entry.plugin.id = record.id.toStdString();
    entry.plugin.name = record.name.toStdString();
    entry.plugin.version = record.version.toStdString();
    entry.plugin.category = record.category.toStdString();
    entry.plugin.summary = record.description.toStdString();
    // Same gates as the load path: hostCompatibility is the runtime
    // compatibility evaluator, and the disabled list is the one
    // ExtensionCatalogService hands the plugin catalog at startup.
    if (!manager.hostCompatibility(record).ok) {
      entry.reason = Reason::kIncompatible;
    } else if (!manager.isEnabled(record.id)) {
      entry.reason = Reason::kDisabled;
    } else if (
        staged.contains(record.id) || manager.hasPendingInstall(record.id) || manager.hasPendingUninstall(record.id)) {
      entry.reason = Reason::kNeedsRestart;
    }
    out.push_back(std::move(entry));
  }
  return out;
}

bool FeatureCatalogHost::installable(const ExtensionManager& manager, const Extension& ext) {
  // hostCompatibility admits an entry with no platforms map (by design, for a
  // sideloaded row); a registry entry must name this platform to install.
  return ext.platforms.contains(PlatformUtils::currentPlatform()) && manager.hostCompatibility(ext).ok;
}

std::string FeatureCatalogHost::defaultEntryPoint(const Plugin& plugin) {
  const std::string& name = plugin.name.empty() ? plugin.id : plugin.name;
  switch (plugin.family) {
    case PluginFamily::kDataSource: {
      // Loaded sources route by what they can do at run time, not by category;
      // a source that both imports files and streams gets both.
      const uint64_t capabilities = plugin.source_capabilities.value_or(
          plugin.category == "data_loader"   ? uint64_t{PJ_DATA_SOURCE_CAPABILITY_FINITE_IMPORT}
          : plugin.category == "data_stream" ? uint64_t{PJ_DATA_SOURCE_CAPABILITY_CONTINUOUS_STREAM}
                                             : uint64_t{0});
      std::string entry;
      if ((capabilities & PJ_DATA_SOURCE_CAPABILITY_FINITE_IMPORT) != 0) {
        entry = kFileImportEntry;
      }
      if ((capabilities & PJ_DATA_SOURCE_CAPABILITY_CONTINUOUS_STREAM) != 0) {
        entry += (entry.empty() ? "" : "; ") + ("Sources panel → Stream → " + name + " → + (Start streaming)");
      }
      return entry;
    }
    case PluginFamily::kToolbox:
      // Cloud wins over hidden: LeftPanel lists a cloud toolbox whatever else
      // it is tagged with.
      if (hasTag(plugin, kToolboxTagCloud)) {
        return "Sources panel → Cloud → " + name + " → + (Open cloud browser)";
      }
      if (hasTag(plugin, kToolboxTagHidden)) {
        return {};
      }
      return "Toolbox menu → " + name;
    case PluginFamily::kMessageParser:
      return std::string(kParserEntry);
    default:
      break;
  }
  return {};
}

}  // namespace PJ
