#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

/**
 * @file PluginRuntimeCatalog.h
 * @brief Shared host-side catalog that discovers, loads, and indexes plugin
 *        DSOs across the DataSource, MessageParser, Toolbox and Widget families.
 *
 * Wraps scanPluginDsos() discovery plus the per-family loaders; exposes the
 * loaded DataSource/MessageParser/Toolbox/Widget sets and lookup helpers (by file
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
#include "pj_plugins/host/widget_library.hpp"

namespace PJ {

/// No plugin id starts with this: the in-app MCP server names AI agents `agent-<client>` in the same
/// id namespace (tabs, derived series, layouts), so an agent never shares a plugin's identity.
inline constexpr std::string_view kAgentIdPrefix = "agent-";

/// Reads the Qt minor a library asked for out of a dynamic-loader failure such as
/// "version `Qt_6.13' not found (required by libfoo.so)" and words it for the user:
/// "needs Qt 6.13 (PlotJuggler uses 6.12)". Empty when the message is about anything else.
/// A Widget plugin built against a newer Qt fails this way before its build_info() can be read,
/// so this is the only place that failure can be explained. Linux loader wording; Windows reports
/// an error code without the version, which stays as is.
[[nodiscard]] std::string describeQtVersionLoadFailure(
    std::string_view loader_message, int host_qt_major, int host_qt_minor);

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

struct RuntimeMessageParserPlugin {
  MessageParserLibrary library;
  std::string path;
  std::string name;
  std::string id;
  std::string version;
  std::vector<std::string> encodings;
  // Load-time provenance, recorded by the catalog tier that selected the plugin, not re-derived from path strings.
  bool from_authoritative_dir = false;
  bool statically_registered = false;
  std::filesystem::file_time_type loaded_mtime;
};

/// Manifest `capabilities` tag of a Custom Topics editor that shows other owners' recipes read-only. A host-side tag
/// (a host ignores tags it does not know), so an editor that lacks it is never handed a view it would make editable.
inline constexpr std::string_view kCustomTopicsViewerCapability = "custom_topics_viewer";

struct RuntimeToolboxPlugin {
  ToolboxLibrary library;
  std::string path;
  std::string name;
  std::string id;
  std::string version;
  std::string badge;  ///< the manifest's optional short label for what it creates; empty when undeclared
  /// The manifest lists `custom_topics_editor` in its `capabilities` tags: the editor of the host's Custom Topics.
  bool custom_topics_editor = false;
  /// The manifest lists `kCustomTopicsViewerCapability`: as the editor, it also opens a recipe it does not own as a
  /// read-only view (the `view` config, see ExtensionCatalogService::customTopicsEditorViews).
  bool custom_topics_viewer = false;
  uint64_t capabilities = 0;
  std::filesystem::file_time_type loaded_mtime;
};

// Loaded Widget plugin plus the metadata the "Custom Widget" picker and the dock need. Stays in the catalog until the
// user disables it (see reload()); the SDK loader keeps its library mapped for the process lifetime.
struct RuntimeWidgetPlugin {
  WidgetLibrary library;
  std::string path;
  std::string name;
  std::string id;
  std::string version;
  std::vector<WidgetTypeDescriptor> widgets;  ///< one entry per widget type the library offers
  // False when build_info() says the library was built against a Qt this host cannot share (other major.minor, other
  // compiler family, a debug/release mix on MSVC). The library stays registered so the dock can explain the missing
  // widget; create_qwidget() must NOT be called for it.
  bool qt_compatible = true;
  std::string qt_mismatch_reason;  ///< user-facing sentence when !qt_compatible
  std::filesystem::file_time_type loaded_mtime;
};

// Declarative set of plugin entry points compiled into a static application. The application owns composition (which
// archives are linked); the runtime owns registration, validation, diagnostics and duplicate handling.
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
  std::vector<StaticPluginEntry<PJ_widget_vtable_t>> widgets;  ///< `dialog` is unused for widgets
};

// One scan folder plus its duplicate-resolution tier: authoritative folders (user-explicit: custom folders,
// --plugin-dir) override outright when a plugin id appears in several folders; managed folders (marketplace, bundled)
// resolve by compatibility, then version, then folder priority. See setPluginDirs.
struct PluginDirEntry {
  std::filesystem::path dir;
  bool authoritative = false;
};

// Host-side catalog of loaded plugin DSOs for one plugin directory. Owns the loaded libraries; the Runtime*Plugin
// structs are non-owning views plus the metadata host UIs and sessions need. Not thread-safe.
class PluginRuntimeCatalog {
 public:
  explicit PluginRuntimeCatalog(
      std::filesystem::path plugin_dir = {}, DiagnosticSink sink = {},
      std::string diagnostic_source = "PluginRuntimeCatalog");

  // Replaces the scan list with one managed directory (see setPluginDirs).
  void setPluginDir(std::filesystem::path plugin_dir);

  // Replaces the ordered list of scanned directories (descending priority), de-duplicated by manifest id. A statically
  // registered plugin outranks every folder tier, authoritative included (--plugin-dir cannot override a compiled-in
  // plugin); otherwise the highest-priority authoritative entry claims the id outright, ignoring other copies'
  // versions; among managed entries the higher version wins, then directory priority. An incompatible copy (ABI or SDK
  // floor) is rejected first and never claims its id, so an incompatible --plugin-dir build falls back to the managed
  // copy. Empty entries are ignored.
  void setPluginDirs(std::vector<PluginDirEntry> plugin_dirs);

  void setDiagnosticSink(DiagnosticSink sink);

  // Ids of extensions the user disabled (installed but not loaded): a matching
  // winner is skipped at load time with an info diagnostic. Read on the scan
  // thread, so set it before the first scanDirectory()/reload().
  void setDisabledIds(std::unordered_set<std::string> disabled_ids);

  // Rebuilds the DSO-backed plugin set from the scan folders. Statically
  // registered plugins (registerStatic*) are permanent and survive rescans.
  void scanDirectory();

  // Reconciles the DSO-backed plugin set with disk; true if it changed. Static plugins are never evicted. Widget
  // plugins are never evicted either, whether the file vanished or was rewritten: a changed file reports "updated on
  // disk; restart required" and keeps the loaded copy; a first-time library is loaded. A Widget plugin (static or not)
  // whose id is now disabled leaves widgets(); panels already running it keep working.
  [[nodiscard]] bool reload();

  // Static plugin registration (WASM/static builds; no dlopen). Registers a statically linked plugin by vtable, which
  // must outlive this catalog; the manifest comes from vtable->manifest_json and must carry a non-empty "id". Returns
  // false (with a diagnostic) on failure. Static registrations have no backing file, so scans and reload() leave them
  // untouched, and they outrank every scan folder: a DSO sharing the id is skipped by scans, an already-loaded one is
  // evicted, a second static registration of the id is rejected.
  bool registerStaticDataSource(
      const PJ_data_source_vtable_t* vtable, const PJ_dialog_vtable_t* dialog_vtable = nullptr);
  bool registerStaticMessageParser(
      const PJ_message_parser_vtable_t* vtable, const PJ_dialog_vtable_t* dialog_vtable = nullptr);
  bool registerStaticToolbox(const PJ_toolbox_vtable_t* vtable, const PJ_dialog_vtable_t* dialog_vtable = nullptr);
  // Widget plugins have no companion dialog. A second registration of the same
  // id is rejected, and so is an id already loaded from a DSO (a loaded Widget
  // library cannot be superseded: it is never unloaded).
  bool registerStaticWidget(const PJ_widget_vtable_t* vtable);

  // Registers one application-composed static set, attempting every entry so a bad optional plugin does not hide the
  // others' diagnostics. True only when every entry registered.
  bool registerStaticPlugins(const StaticPluginSet& plugins);

  [[nodiscard]] const std::vector<RuntimeDataSourcePlugin>& dataSources() const {
    return data_sources_;
  }

  [[nodiscard]] const std::vector<RuntimeMessageParserPlugin>& messageParsers() const {
    return message_parsers_;
  }

  [[nodiscard]] const std::vector<RuntimeToolboxPlugin>& toolboxes() const {
    return toolbox_plugins_;
  }

  // Returns loaded Widget plugins, including those with qt_compatible == false.
  [[nodiscard]] const std::vector<RuntimeWidgetPlugin>& widgets() const {
    return widget_plugins_;
  }

  [[nodiscard]] std::vector<RuntimeDataSourcePlugin*> fileImportSources();

  [[nodiscard]] std::vector<const RuntimeDataSourcePlugin*> fileImportSources() const;

  [[nodiscard]] std::vector<RuntimeDataSourcePlugin*> streamSources();

  [[nodiscard]] std::vector<const RuntimeDataSourcePlugin*> streamSources() const;

  // Finds file-import DataSources that handle ext. The extension may be given
  // with or without the leading dot; matching is ASCII case-insensitive.
  [[nodiscard]] std::vector<RuntimeDataSourcePlugin*> findSourcesForExtension(std::string_view ext);

  [[nodiscard]] std::vector<const RuntimeDataSourcePlugin*> findSourcesForExtension(std::string_view ext) const;

  [[nodiscard]] RuntimeMessageParserPlugin* findParserByEncoding(std::string_view encoding);

  [[nodiscard]] const RuntimeMessageParserPlugin* findParserByEncoding(std::string_view encoding) const;

  [[nodiscard]] const RuntimeMessageParserPlugin* findParserById(std::string_view id) const;

  // Builds a QFileDialog-compatible filter string.
  [[nodiscard]] std::string buildFileFilter() const;

  // Lists parser encodings as a JSON string array.
  [[nodiscard]] std::string listAvailableEncodings() const;

  // True for a plugin compiled into this build (registered without a library file).
  [[nodiscard]] bool isStaticallyRegisteredId(const std::string& id) const;

 private:
  // Scans every entry in plugin_dirs_ and returns the loadable descriptors de-duplicated by manifest id, choosing each
  // id's winner by static-registration tier, then authoritative tier, then compatibility, then version, then
  // directory priority (see setPluginDirs). Reports scan diagnostics and one info diagnostic per skipped duplicate.
  [[nodiscard]] std::vector<PluginDescriptor> collectDeduplicatedPlugins() const;

  bool loadAndRegister(const PluginDescriptor& descriptor);

  bool loadAndRegisterDataSource(const PluginDescriptor& descriptor);

  bool loadAndRegisterMessageParser(const PluginDescriptor& descriptor);

  bool loadAndRegisterToolbox(const PluginDescriptor& descriptor);

  // Loads and records one Widget plugin. A library whose id or path is already
  // loaded is skipped: Widget libraries are never replaced at runtime.
  bool loadAndRegisterWidget(const PluginDescriptor& descriptor);

  // Fills the Qt-build fields of `loaded` from the library's build_info() and,
  // when the build is compatible, cross-checks the declared qt_modules against
  // the modules PlotJuggler ships. Reports the diagnostics.
  void admitWidgetBuild(RuntimeWidgetPlugin& loaded, const PluginDescriptor& descriptor) const;

  // Commits a validated static registration's id claim: rejects id (with an
  // error diagnostic) when another static registration holds it, otherwise
  // evicts any DSO-backed plugin with the same id. Call only after all candidate
  // validation succeeds so a rejected replacement leaves the DSO intact.
  bool claimStaticId(const std::string& id, const char* family);

  bool evictByPath(const std::string& path);

  [[nodiscard]] std::filesystem::file_time_type loadedMtimeForPath(const std::string& path) const;

  void report(DiagnosticLevel level, const std::string& id, std::string message) const;

  // report() for one plugin, prefixed with its DSO path ("<path>: <message>").
  void reportAt(DiagnosticLevel level, const PluginDescriptor& descriptor, const std::string& message) const;

  void reportScanDiagnostics(const PluginScanResult& scan) const;

  std::vector<PluginDirEntry> plugin_dirs_;
  DiagnosticSink sink_;
  std::string diagnostic_source_;
  std::unordered_set<std::string> disabled_ids_;  ///< winners with these ids are not loaded
  // Canonical paths of authoritative winners from the most recent scan; recorded by the collector (which has the
  // PluginDirEntry context) and consumed by the family loaders when building Runtime* metadata.
  mutable std::unordered_set<std::string> authoritative_plugin_paths_;
  std::vector<RuntimeDataSourcePlugin> data_sources_;
  std::vector<RuntimeMessageParserPlugin> message_parsers_;
  std::vector<RuntimeToolboxPlugin> toolbox_plugins_;
  std::vector<RuntimeWidgetPlugin> widget_plugins_;
  // Widget "restart required" notices already reported, so a periodic reload() says each once.
  std::unordered_set<std::string> widget_notices_;
};

}  // namespace PJ
