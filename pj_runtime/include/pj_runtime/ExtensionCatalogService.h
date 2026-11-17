#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "pj_base/diagnostic_sink.hpp"
#include "pj_plugins/host/message_parser_library.hpp"
#include "pj_runtime/ParserRoutingService.h"
#include "pj_runtime/PluginRuntimeCatalog.h"

namespace PJ {

class ExtensionManager;

using LoadedDataSource = RuntimeDataSourcePlugin;
using LoadedMessageParser = RuntimeMessageParserPlugin;
using LoadedToolbox = RuntimeToolboxPlugin;
using LoadedWidget = RuntimeWidgetPlugin;

// The marketplace (ExtensionManager) and the per-family plugin catalog behind one facade: `pj_app` asks this service,
// never pj_marketplace or pj_plugins. Construction, in order: the ExtensionManager applies pending staged actions,
// seedBundledPlugins() syncs the bundled plugins into the marketplace dir, the scan hierarchy loads. The bundled dir is
// a seed source only, never a load path. reload() after an install/uninstall hot-loads new plugins.
class ExtensionCatalogService : public QObject {
  Q_OBJECT
 public:
  using Ptr = std::shared_ptr<ExtensionCatalogService>;

  // Directory roots, resolved at construction. Tests inject temp dirs so seeding and scanning never touch the real
  // profile or executable path.
  struct Paths {
    // Managed by the ExtensionManager and the top scan tier: the --plugin-dir override. Empty: default mode.
    QString install_dir;
    // Seed destination and lowest (managed) scan tier. Empty: PlatformUtils::extensionsDir().
    QString marketplace_dir;
    // Seed source. Empty: <prefix>/lib/plotjuggler/plugins next to the executable (Contents/Resources/plugins in a
    // macOS bundle).
    QString bundled_dir;
  };

  explicit ExtensionCatalogService(QString extensions_dir = {}, QObject* parent = nullptr);

  ExtensionCatalogService(QString extensions_dir, DiagnosticSink sink, QObject* parent = nullptr);

  ExtensionCatalogService(Paths paths, DiagnosticSink sink, QObject* parent = nullptr);

  // Registers the static plugins before the first scan: the startup seam of static/WASM binaries.
  ExtensionCatalogService(
      QString extensions_dir, DiagnosticSink sink, StaticPluginSet static_plugins, QObject* parent = nullptr);

  // The set's vtable/dialog pointers must have static storage duration: the catalog keeps them.
  ExtensionCatalogService(Paths paths, DiagnosticSink sink, StaticPluginSet static_plugins, QObject* parent = nullptr);

  ~ExtensionCatalogService() override;

  ExtensionCatalogService(const ExtensionCatalogService&) = delete;

  ExtensionCatalogService& operator=(const ExtensionCatalogService&) = delete;

  // Browser builds have no marketplace: their plugins are registered statically.
#ifndef PJ_TARGET_WASM
  ExtensionManager& extensionManager() const {
    return *extension_manager_;
  }
#endif

  // Tests register in-process plugins here. Not after construction: a raw registration takes no catalog lock, does
  // not rebuild the parser claims (route selection would not see it), and poll threads walk the parser set
  // concurrently. Application static plugins go in the constructor set.
  PluginRuntimeCatalog& pluginCatalog() {
    return *plugin_catalog_;
  }

  QString extensionsDir() const {
    return extensions_dir_;
  }

  void reload();

  const std::vector<LoadedDataSource>& dataSources() const;

  const std::vector<LoadedMessageParser>& messageParsers() const;

  const std::vector<LoadedToolbox>& toolboxes() const;

  // Includes Qt-mismatched plugins (qt_compatible == false: the dock explains). A Widget library is never unloaded: an
  // entry leaves only when reload() withdraws a plugin the user disabled, and running panels keep working.
  const std::vector<LoadedWidget>& widgets() const;

  std::vector<const LoadedDataSource*> fileImportSources() const;

  std::vector<const LoadedDataSource*> streamSources() const;

  std::vector<const LoadedDataSource*> findSourcesForExtension(QStringView ext) const;

  // A pointer into the catalog vector: valid until the next reload(), GUI thread only (reload reallocates it). Off-GUI
  // callers (a poll thread) use resolveParserRoutes()/parserEncodings(), which resolve under the catalog lock.
  const LoadedMessageParser* findParserByEncoding(QStringView encoding) const;

  // Null when not loaded. Lifetime and threading as findParserByEncoding.
  const LoadedToolbox* findToolbox(QStringView id) const;

  // Null when not loaded; rules as findToolbox. A caller keeping the library (a dock with a live widget) copies the
  // WidgetLibrary's shared ownership through createHandle(), never the pointer.
  const LoadedWidget* findWidget(QStringView plugin_id) const;

  // Short label naming the Toolbox `id` on a tab watermark or a Custom Topics row badge: its manifest `badge`, else
  // the toolbox name, else the id; an ad-hoc owner's name; empty when neither is known. Same lifetime and threading
  // rules as findToolbox.
  [[nodiscard]] QString toolboxBadge(QStringView id) const;
  // The toolbox's display name, else an ad-hoc owner's name, else `id` itself. Same rules as findToolbox.
  [[nodiscard]] QString toolboxDisplayName(QStringView id) const;
  // Names `id`, an owner of tabs and derived series that is no plugin (an AI agent acting through the toolbox
  // services), so toolboxBadge and toolboxDisplayName label it instead of reporting a missing plugin. A loaded
  // toolbox with the same id still wins. Kept for the process lifetime (its tabs and series outlive the agent).
  // Main thread.
  void registerAdHocOwner(const QString& id, const QString& name);

  // The id of the loaded toolbox whose manifest lists `custom_topics_editor` in its `capabilities`: the editor of
  // the host's Custom Topics (the "+" opens it, and its recipes are the only ones the panel lets the user edit or
  // delete). When several declare it, one wins (see pickCustomTopicsEditor) and one warning names the rest. Empty
  // when none does. Resolved by the constructor and by every reload() that changed the catalog, so this is a plain
  // read. Same lifetime and threading rules as findToolbox.
  [[nodiscard]] const QString& customTopicsEditorId() const noexcept {
    return custom_topics_editor_id_;
  }
  // Test seam: names the editor of a catalog that loaded no plugin declaring it.
  void setCustomTopicsEditorIdForTest(const QString& plugin_id) {
    custom_topics_editor_id_ = plugin_id;
  }
  // The one ownership predicate: `owner` is the Custom Topics editor. False for everyone when none is declared.
  // The static form serves a holder of the id that has no catalog at hand (the Custom Topics panel).
  [[nodiscard]] static bool isEditorOwner(const QString& editor_id, const QString& owner) noexcept {
    return !editor_id.isEmpty() && owner == editor_id;
  }
  [[nodiscard]] bool isCustomTopicsEditor(const QString& owner) const noexcept {
    return isEditorOwner(custom_topics_editor_id_, owner);
  }
  // The editor among `declarers` (the toolboxes declaring `custom_topics_editor`, in scan order): `current` while it
  // is still one of them, else the first. A reload appends an updated plugin after the others, so first-scanned
  // alone would hand the editor to another plugin on every update. Empty when there is no declarer.
  [[nodiscard]] static QString pickCustomTopicsEditor(const QString& current, const QStringList& declarers);

  // [thread-safe] A sorted, de-duplicated copy, so no caller holds a reference across a reload().
  [[nodiscard]] std::vector<std::string> parserEncodings() const;

  // [thread-safe] Per-route parser selection for one topic, on the poll/stream thread. For encodings in the SDK claim
  // registry the scalar and object routes resolve independently: every parser holds the wildcard scalar claim for its
  // encodings; exact per-type claims are discovered lazily per (provider, encoding, type, schema, config) through
  // pj.parser_route_claims.v1, or classify_schema (object route only). Candidates are probed exact > wildcard, then
  // provenance tier, then priority. Other encodings take the first provider registered for both routes
  // (`route_dispatch == false`). Each winner is an instance no binding shares, prepared (bindSchema + loadConfig).
  // Probe decisions are cached across topics until the catalog rebuilds.
  [[nodiscard]] ParserRouteSelection resolveParserRoutes(
      std::string_view encoding, std::string_view type_name, Span<const uint8_t> schema,
      std::string_view parser_config_json) const;

  // [thread-safe] The object route's claimed type at advertise time; nullopt when unclaimed. Instantiates no winner for
  // registry encodings.
  [[nodiscard]] std::optional<sdk::BuiltinObjectType> classifyParserObjectRoute(
      std::string_view encoding, std::string_view type_name, Span<const uint8_t> schema) const;

  QString buildFileFilter() const;

  // User plugin folders (Preferences::plugin_folders): an authoritative scan tier below --plugin-dir, above the
  // marketplace dir. Changes apply on next launch (no hot reload): the setter only writes the key.
  [[nodiscard]] QStringList customPluginFolders() const;
  void setCustomPluginFolders(const QStringList& folders);

  // The scanned built-in folders, in priority order: the install dir and, when distinct, the marketplace dir.
  // Read-only, shown to the user.
  [[nodiscard]] QStringList builtinPluginFolders() const;

  // Empty when the host would load this plugin, else a readable reason. The seed overwrites an installed copy above
  // the bundled version with the bundled build when it is non-empty. Same primitive as marketplace admission and
  // PluginRuntimeCatalog:
  //   - ABI: `abi_major` must equal `PJ_ABI_VERSION`; 0 (undeclared) passes here and the load path's ABI symbol check
  //     catches a real mismatch.
  //   - `min_sdk_required`: the host SDK must be at least that SemVer floor; empty passes.
  // `min_plotjuggler_version` never gates. Wording matches ExtensionManager::hostCompatibility, so the seed and the
  // marketplace UI say the same thing.
  [[nodiscard]] static QString descriptorIncompatReason(const PluginDescriptor& descriptor);

  [[nodiscard]] static bool descriptorIsCompatibleWithHost(const PluginDescriptor& descriptor) {
    return descriptorIncompatReason(descriptor).isEmpty();
  }

 signals:
  void catalogChanged();

 private:
  void reportDiagnostic(DiagnosticLevel level, const QString& message, const QString& id = {}) const;

  // Highest priority first: --plugin-dir (when explicit), the custom folders (both authoritative: version-blind
  // override in duplicate resolution), then the managed marketplace dir. Missing folders are skipped, so an absent
  // optional folder does not raise a kError every launch.
  [[nodiscard]] std::vector<PluginDirEntry> buildScanHierarchy(bool extensions_dir_is_explicit) const;

  // One wildcard scalar claim per parser per registry encoding (a parser with none stays on first-registered
  // selection); advances the provider generation and clears every cached probe. Runs after the first scan and inside
  // every changing reload()'s exclusive section, so no resolution sees claims that do not describe the catalog. Caller
  // holds routing_mutex_ and catalog_mutex_ (exclusive).
  void rebuildParserClaims();

  // Picks the `custom_topics_editor` toolbox into custom_topics_editor_id_ (see customTopicsEditorId). Run
  // wherever the toolbox list changes.
  void resolveCustomTopicsEditor();
  QString custom_topics_editor_id_;

  // Statically linked or seeded-bundled: kBundled; an authoritative folder: kFolderDrop; else kMarketplace.
  [[nodiscard]] ParserClaimProvenance provenanceForParser(const LoadedMessageParser& parser) const;

  // Invalid handle when the id is not loaded. The routing service's create callback: runs inside a resolution that
  // already holds catalog_mutex_ shared.
  [[nodiscard]] MessageParserHandle createParserHandleForProviderId(std::string_view provider_id) const;

  // The only way bundled plugins become loadable. Runs in every mode (the --plugin-dir dir is never a seed source or
  // destination), after staged installs are applied, so a staged upgrade is compared. Per bundled id: absent or
  // unreadable -> copy; installed older -> staged copy and rename swap (a failed refresh keeps the old copy); same or
  // newer -> untouched (ship a change by bumping the version). A size+mtime match against the bundled DSO skips the
  // manifest read. Failures are logged and retried next launch. The id -> version map reaches the ExtensionManager
  // (uninstall lock, downgrade to bundled) only in default mode: under --plugin-dir it governs user-owned copies.
  void seedBundledPlugins();

  QString extensions_dir_;
  // Seed destination and lowest scan tier; equals extensions_dir_ in default mode.
  QString marketplace_dir_;
  QString bundled_dir_;
  // No --plugin-dir override. Captured at construction, never re-derived by comparing paths (a Paths caller may make
  // the two dirs textually equal).
  bool default_mode_ = false;
  DiagnosticSink sink_;

#ifndef PJ_TARGET_WASM
  std::unique_ptr<ExtensionManager> extension_manager_;
#endif
  std::unique_ptr<PluginRuntimeCatalog> plugin_catalog_;

  // A poll thread resolving a parser while the GUI thread reloads the catalog: reload() takes it exclusive, the
  // thread-safe accessors shared. GUI-only readers do not lock (reload() is GUI-only too).
  mutable std::shared_mutex catalog_mutex_;

  // ----- Route-aware parser selection -----
  struct BundledPluginInfo {
    std::string version;
    std::filesystem::path dso_path;
  };
  // Id -> shipped version and DSO path, captured before the first scan. Only an artifact matching id, version and DSO
  // signature earns the bundled tier: an id alone is artifact-controlled and would forge the highest trust tier.
  std::unordered_map<std::string, BundledPluginInfo> bundled_plugin_versions_;
  QHash<QString, QString> ad_hoc_owners_;  // registerAdHocOwner: id -> name; main thread

  // Serializes routing_ (externally synchronized). Order routing_mutex_ -> catalog_mutex_ in resolve/classify and in
  // reload(): outermost, so queued resolutions never hold the shared lock while a GUI reload waits for the exclusive
  // one (pthread rwlocks let new readers overtake a waiting writer). std::mutex is not FIFO: this bounds the readers,
  // not the wait.
  mutable std::mutex routing_mutex_;
  ParserRoutingService routing_;
};

// The DataSource a saved layout names among `candidates`: the one whose manifest id is
// `manifest_id`, else the one whose display name is `name` (the legacy identity); an empty
// hint skips its tier. Each tier sweeps every candidate, so an id match on a later candidate
// beats a name match on an earlier one. Null when neither matches; null candidates are skipped.
[[nodiscard]] const LoadedDataSource* findDataSourceByIdentity(
    const std::vector<const LoadedDataSource*>& candidates, const QString& manifest_id, const QString& name);

}  // namespace PJ
