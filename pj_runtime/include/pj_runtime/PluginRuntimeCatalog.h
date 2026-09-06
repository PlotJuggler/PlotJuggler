#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

/**
 * @file PluginRuntimeCatalog.h
 * @brief Shared host-side catalog that discovers, loads, and indexes plugin
 *        DSOs across all four families.
 *
 * Wraps scanPluginDsos() discovery plus the per-family loaders; exposes the
 * loaded DataSource/MessageParser/Toolbox sets and lookup helpers (by file
 * extension, by parser encoding). reload() reconciles loaded state with disk.
 */

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "pj_base/diagnostic_sink.hpp"
#include "pj_plugins/host/data_source_library.hpp"
#include "pj_plugins/host/message_parser_library.hpp"
#include "pj_plugins/host/plugin_catalog.hpp"
#include "pj_plugins/host/toolbox_library.hpp"

namespace PJ {

// Loaded DataSource plugin plus metadata used by host UIs and sessions.
struct RuntimeDataSourcePlugin {
  DataSourceLibrary library;
  std::string path;
  std::string name;
  std::string id;
  std::string version;
  std::vector<std::string> file_extensions;
  uint64_t capabilities = 0;
  std::filesystem::file_time_type loaded_mtime;
};

// Loaded MessageParser plugin plus lookup metadata.
struct RuntimeMessageParserPlugin {
  MessageParserLibrary library;
  std::string path;
  std::string name;
  std::string id;
  std::string version;
  std::vector<std::string> encodings;
  // Load-time provenance inputs. They are recorded by the catalog tier that
  // selected the plugin rather than re-derived from artifact path strings.
  bool from_authoritative_dir = false;
  bool statically_registered = false;
  std::filesystem::file_time_type loaded_mtime;
};

// Loaded Toolbox plugin plus metadata used by Tools menus.
struct RuntimeToolboxPlugin {
  ToolboxLibrary library;
  std::string path;
  std::string name;
  std::string id;
  std::string version;
  uint64_t capabilities = 0;
  std::filesystem::file_time_type loaded_mtime;
};

// Declarative set of plugin entry points compiled into a static application.
// The application owns composition (which archives are linked); the runtime
// owns registration, validation, diagnostics, and duplicate handling. Keeping
// this as data avoids a second app-side catalog/registry implementation.
template <typename PluginVtable>
struct StaticPluginEntry {
  const PluginVtable* plugin = nullptr;
  // Optional companion UI exported from the same DSO on desktop. Static builds
  // cannot dlsym it, so composition passes the unique class-keyed getter here.
  const PJ_dialog_vtable_t* dialog = nullptr;

  StaticPluginEntry() = default;
  StaticPluginEntry(const PluginVtable* plugin_vtable, const PJ_dialog_vtable_t* dialog_vtable = nullptr)
      : plugin(plugin_vtable), dialog(dialog_vtable) {}
};

struct StaticPluginSet {
  std::vector<StaticPluginEntry<PJ_data_source_vtable_t>> data_sources;
  std::vector<StaticPluginEntry<PJ_message_parser_vtable_t>> message_parsers;
  std::vector<StaticPluginEntry<PJ_toolbox_vtable_t>> toolboxes;
};

// One scan folder plus its duplicate-resolution tier. Authoritative folders
// (the host's user-explicit tiers: custom folders + --plugin-dir) are a hard
// override when the same plugin id appears in several folders; managed folders
// (marketplace + bundled) resolve by compatibility, then version, then folder
// priority. See setPluginDirs.
struct PluginDirEntry {
  std::filesystem::path dir;
  bool authoritative = false;
};

// Host-side catalog of loaded plugin DSOs for one plugin directory. Owns the
// loaded libraries; the Runtime*Plugin structs above are non-owning views into
// the loaded set plus the metadata host UIs/sessions need. Not thread-safe.
class PluginRuntimeCatalog {
 public:
  // Creates a catalog rooted at plugin_dir and reporting through sink.
  explicit PluginRuntimeCatalog(
      std::filesystem::path plugin_dir = {}, DiagnosticSink sink = {},
      std::string diagnostic_source = "PluginRuntimeCatalog");

  // Replaces the scan list with one managed directory (see setPluginDirs).
  void setPluginDir(std::filesystem::path plugin_dir);

  // Replaces the ordered list of directories scanned by scanDirectory() and
  // reload(). Directories are scanned in descending priority order and
  // de-duplicated by manifest id: a statically registered plugin
  // (registerStatic*) outranks every folder tier — even an authoritative one,
  // so --plugin-dir cannot override a compiled-in plugin; otherwise the
  // highest-priority *authoritative* entry claims the id outright (ignoring
  // version and compatibility of every other copy); among managed entries the
  // winner is chosen by compatibility first (see setHostVersion), then by higher
  // version, then by directory priority. Compatibility is a hard load gate after
  // selection. Thus an incompatible authoritative copy shadows managed fallbacks
  // but is itself rejected — preserving strict developer override semantics.
  // Empty entries are ignored.
  void setPluginDirs(std::vector<PluginDirEntry> plugin_dirs);

  // Replaces the optional diagnostic sink.
  void setDiagnosticSink(DiagnosticSink sink);

  // Sets the host ("PlotJuggler") version used by the unified hard compatibility
  // gate. That gate validates ABI, the SDK contract floor, and the manifest's
  // `min_plotjuggler_version`; a compatible managed duplicate is preferred and an
  // incompatible final winner is never loaded. Empty is valid only for hosts that
  // load no plugin declaring an application floor: such a declaration then fails
  // closed because the host version cannot be established.
  //
  // Call before the first scanDirectory()/reload(): the value is read on the
  // scan thread and this class is not thread-safe, so it must not change
  // concurrently with a scan.
  void setHostVersion(std::string host_version);

  // Ids of extensions the user disabled (installed but not loaded): a matching
  // winner is skipped at load time with an info diagnostic. Read on the scan
  // thread like setHostVersion, so set it before the first scanDirectory()/reload().
  void setDisabledIds(std::unordered_set<std::string> disabled_ids);

  // Rebuilds the DSO-backed plugin set from the scan folders. Statically
  // registered plugins (registerStatic*) are permanent and survive rescans.
  void scanDirectory();

  // Reconciles the DSO-backed plugin set with disk and returns true if it
  // changed. Statically registered plugins are never evicted.
  [[nodiscard]] bool reload();

  // --- Static plugin registration (WASM/static builds; no dlopen) -------------
  // Register a statically-linked plugin by its vtable. The vtable must outlive
  // this catalog (static storage duration); the manifest is read from
  // vtable->manifest_json and must carry a non-empty "id". Returns false (and
  // reports a diagnostic) on failure. Static registrations have no backing
  // file, so scanDirectory()/reload() leave them untouched — and they outrank
  // every scan folder: a DSO sharing the id is skipped by scans, an
  // already-loaded one is evicted, and a second static registration of the
  // same id is rejected.
  bool registerStaticDataSource(
      const PJ_data_source_vtable_t* vtable, const PJ_dialog_vtable_t* dialog_vtable = nullptr);
  bool registerStaticMessageParser(
      const PJ_message_parser_vtable_t* vtable, const PJ_dialog_vtable_t* dialog_vtable = nullptr);
  bool registerStaticToolbox(const PJ_toolbox_vtable_t* vtable, const PJ_dialog_vtable_t* dialog_vtable = nullptr);

  // Registers one application-composed static set. Attempts every entry so a
  // bad optional plugin does not hide diagnostics for the remaining entries.
  // Returns true only when every entry registered successfully.
  bool registerStaticPlugins(const StaticPluginSet& plugins);

  // Returns loaded DataSource plugins.
  [[nodiscard]] const std::vector<RuntimeDataSourcePlugin>& dataSources() const {
    return data_sources_;
  }

  // Returns loaded MessageParser plugins.
  [[nodiscard]] const std::vector<RuntimeMessageParserPlugin>& messageParsers() const {
    return message_parsers_;
  }

  // Returns loaded Toolbox plugins.
  [[nodiscard]] const std::vector<RuntimeToolboxPlugin>& toolboxes() const {
    return toolbox_plugins_;
  }

  // Returns file-import capable DataSource plugins.
  [[nodiscard]] std::vector<RuntimeDataSourcePlugin*> fileImportSources();

  // Returns file-import capable DataSource plugins.
  [[nodiscard]] std::vector<const RuntimeDataSourcePlugin*> fileImportSources() const;

  // Returns streaming-capable DataSource plugins.
  [[nodiscard]] std::vector<RuntimeDataSourcePlugin*> streamSources();

  // Returns streaming-capable DataSource plugins.
  [[nodiscard]] std::vector<const RuntimeDataSourcePlugin*> streamSources() const;

  // Finds file-import DataSources that handle ext. The extension may be given
  // with or without the leading dot; matching is ASCII case-insensitive.
  [[nodiscard]] std::vector<RuntimeDataSourcePlugin*> findSourcesForExtension(std::string_view ext);

  // Finds file-import DataSources that handle ext (see the overload above).
  [[nodiscard]] std::vector<const RuntimeDataSourcePlugin*> findSourcesForExtension(std::string_view ext) const;

  // Finds a MessageParser by encoding name.
  [[nodiscard]] RuntimeMessageParserPlugin* findParserByEncoding(std::string_view encoding);

  // Finds a MessageParser by encoding name.
  [[nodiscard]] const RuntimeMessageParserPlugin* findParserByEncoding(std::string_view encoding) const;

  // Finds a MessageParser by manifest provider id.
  [[nodiscard]] const RuntimeMessageParserPlugin* findParserById(std::string_view id) const;

  // Builds a QFileDialog-compatible filter string.
  [[nodiscard]] std::string buildFileFilter() const;

  // Lists parser encodings as a JSON string array.
  [[nodiscard]] std::string listAvailableEncodings() const;

 private:
  // Scans every entry in plugin_dirs_ (in priority order) and returns the
  // loadable descriptors de-duplicated by manifest id, choosing each id's winner
  // by static-registration tier, then authoritative tier, then compatibility,
  // then version, then directory priority (see setPluginDirs / setHostVersion).
  // Reports scan diagnostics and one info diagnostic per skipped duplicate.
  [[nodiscard]] std::vector<PluginDescriptor> collectDeduplicatedPlugins() const;

  // Loads a descriptor using the family-specific loader.
  bool loadAndRegister(const PluginDescriptor& descriptor);

  // Loads and records one DataSource plugin.
  bool loadAndRegisterDataSource(const PluginDescriptor& descriptor);

  // Loads and records one MessageParser plugin.
  bool loadAndRegisterMessageParser(const PluginDescriptor& descriptor);

  // Loads and records one Toolbox plugin.
  bool loadAndRegisterToolbox(const PluginDescriptor& descriptor);

  // True if id is provided by a statically registered plugin in any family.
  [[nodiscard]] bool isStaticallyRegisteredId(const std::string& id) const;

  // Commits a validated static registration's id claim: rejects id (with an
  // error diagnostic) when another static registration holds it, otherwise
  // evicts any DSO-backed plugin with the same id. Call only after all candidate
  // validation succeeds so a rejected replacement leaves the DSO intact.
  bool claimStaticId(const std::string& id, const char* family);

  // Removes any loaded plugin whose path matches path.
  bool evictByPath(const std::string& path);

  // Returns the loaded mtime for path, or a default value.
  [[nodiscard]] std::filesystem::file_time_type loadedMtimeForPath(const std::string& path) const;

  // Emits one diagnostic through the optional sink.
  void report(DiagnosticLevel level, const std::string& id, std::string message) const;

  // Emits diagnostics produced by DSO discovery.
  void reportScanDiagnostics(const PluginScanResult& scan) const;

  std::vector<PluginDirEntry> plugin_dirs_;
  DiagnosticSink sink_;
  std::string diagnostic_source_;
  std::string host_version_;                      ///< host version for compatibility ties; "" disables the check
  std::unordered_set<std::string> disabled_ids_;  ///< winners with these ids are not loaded
  // Canonical paths of authoritative winners from the most recent scan. The
  // collector records the tier while it still has the PluginDirEntry context;
  // family loaders consume it when constructing Runtime* metadata.
  mutable std::unordered_set<std::string> authoritative_plugin_paths_;
  std::vector<RuntimeDataSourcePlugin> data_sources_;
  std::vector<RuntimeMessageParserPlugin> message_parsers_;
  std::vector<RuntimeToolboxPlugin> toolbox_plugins_;
};

}  // namespace PJ
