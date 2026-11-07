#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QUrl>
#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "pj_base/plugin_data_api.h"
#include "pj_base/sdk/feature_catalog_host.hpp"
#include "pj_marketplace/extension.hpp"
#include "pj_plugins/host/plugin_catalog.hpp"

namespace PJ {

class ExtensionCatalogService;
class ExtensionManager;

/// Host side of the optional "pj.feature_catalog.v1" service: composes the
/// schema-1 JSON document that tells a plugin (the assistant agent) what this
/// PlotJuggler offers — the built-in features and known limitations from the
/// embedded `:/resources/feature_catalog.json`, the plugins loaded in this
/// process, the installed extensions that are not loaded, and the installable
/// entries of a registry the app ALREADY fetched. It never fetches anything.
///
/// Each extension id lands in exactly one list, with precedence
/// loaded > installed_not_loaded > available. Every string is bounded, lists
/// are sorted by id and capped, so the document is bounded and deterministic.
/// Every input is a provider called per document, so a test composes it from
/// fakes and the app from its live services (the static adapters below).
///
/// Desktop only (no Marketplace in the browser build). GUI thread only. The
/// object must outlive every plugin session bound to view(): the service's ctx
/// is this object.
class FeatureCatalogHost {
 public:
  /// One extension as a plugin list shows it.
  struct Plugin {
    /// Selects the default entry point; kUnknown and kDialog have none.
    PluginFamily family = PluginFamily::kUnknown;
    std::string id;
    std::string name;
    std::string version;
    std::string category;  ///< manifest/registry category string, verbatim
    std::string summary;   ///< manifest/registry description
    /// Manifest/registry `entry_point` override; empty = use the host default.
    std::string entry_point;
    std::vector<std::string> tags;
    /// Runtime DataSource capability bits (PJ_DATA_SOURCE_CAPABILITY_*), known
    /// only for a loaded source; otherwise the default follows `category`.
    std::optional<uint64_t> source_capabilities;
  };

  /// Why an installed extension is not loaded. kUnknown omits the field
  /// (an enabled, compatible record that is not loaded: e.g. installed during
  /// this session before the catalog reloaded, or one that failed to load).
  enum class Reason : uint8_t { kUnknown, kDisabled, kNeedsRestart, kIncompatible };

  /// Listed without an entry point: the installed record does not know where
  /// its extension would appear, and a missing field means unknown.
  struct Installed {
    Plugin plugin;
    Reason reason = Reason::kUnknown;
  };

  /// All four are required.
  struct Sources {
    std::function<std::vector<Plugin>()> loaded;
    std::function<std::vector<Installed>()> installed;
    /// The real install gate for a registry entry (the app passes
    /// FeatureCatalogHost::installable).
    std::function<bool(const Extension&)> installable;
    /// The registry URL the app would fetch now; a fetch of any other URL is
    /// reported as not_fetched/url_changed.
    std::function<QUrl()> registry_url;
  };

  /// `core_json` is the resource document ({"schema":1,"core":[…],
  /// "limitations":[…]}), the app's own compiled-in file; invalid JSON yields
  /// empty core/limitations.
  FeatureCatalogHost(Sources sources, QByteArray core_json, std::string host_version, std::string platform);

  // The SDK adapter captures `this`; the object stays where it was built.
  FeatureCatalogHost(const FeatureCatalogHost&) = delete;
  FeatureCatalogHost& operator=(const FeatureCatalogHost&) = delete;

  // ---- Registry state: the host reports fetches the app made itself. ----

  /// The startup scan was skipped; reported as not_fetched/updates_disabled
  /// until some fetch happens.
  void setStartupScanSkipped();
  /// A fetch of `url` started (state pending for that URL until a fetch of it
  /// finishes, unless a list is already in hand).
  /// A fetch destroyed mid-flight (the Marketplace closed) reports no finish, so
  /// the state stays pending until the next fetch of that URL finishes.
  void registryFetchStarted(const QUrl& url);
  /// A fetch of `url` finished; the last one to finish decides that URL's state,
  /// so a failure replaces an earlier list. `extensions` is ignored when !ok.
  void registryFetchFinished(const QUrl& url, bool ok, QList<Extension> extensions);

  /// The schema-1 document as compact UTF-8 JSON.
  [[nodiscard]] std::string document() const;

  /// The C ABI service to register as sdk::FeatureCatalogService.
  [[nodiscard]] PJ_feature_catalog_t view() noexcept {
    return adapter_.view();
  }

  /// The plugins loaded in this process, one entry per id.
  [[nodiscard]] static std::vector<Plugin> loadedPlugins(const ExtensionCatalogService& catalog);
  /// The extension manager's installed records (plus staged removals) with
  /// their reason: incompatible > disabled > needs_restart > unknown.
  [[nodiscard]] static std::vector<Installed> installedRecords(const ExtensionManager& manager);
  /// Whether `ext` installs here: it declares this platform (as the
  /// Marketplace list requires) AND passes ExtensionManager::hostCompatibility.
  [[nodiscard]] static bool installable(const ExtensionManager& manager, const Extension& ext);
  /// The host's navigation text for a plugin without an override; "" when it
  /// has no place of its own (a "hidden" toolbox, an unknown category).
  [[nodiscard]] static std::string defaultEntryPoint(const Plugin& plugin);

 private:
  /// The latest outcome of fetching one URL: its list, or nullopt when that
  /// fetch failed.
  using Outcome = std::optional<QList<Extension>>;

  struct RegistryStatus {
    std::string state = "not_fetched";
    std::string reason;
    /// The list of the current URL; null unless state is "fetched". Points into
    /// outcomes_, so it dies with the next finished fetch.
    const QList<Extension>* list = nullptr;
  };

  /// The registry state for the URL the app would use now. A list fetched from
  /// another URL describes some other registry and is not offered.
  [[nodiscard]] RegistryStatus registryStatus() const;

  Sources sources_;
  std::string host_version_;
  std::string platform_;
  // From the resource, maintainer-only fields (evidence, verified) stripped.
  nlohmann::json core_ = nlohmann::json::array();
  nlohmann::json limitations_ = nlohmann::json::array();

  bool startup_scan_skipped_ = false;
  // The URL of the fetch started last, until a fetch of that URL finishes.
  std::optional<QUrl> pending_url_;
  // One entry per URL fetched, so a late outcome for an old URL never replaces
  // the current URL's.
  QHash<QUrl, Outcome> outcomes_;

  sdk::FeatureCatalogHost adapter_;
};

}  // namespace PJ
