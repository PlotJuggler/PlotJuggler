// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "mock_data_source_vtable.h"
#include "mock_message_parser_vtable.h"
#include "mock_toolbox_vtable.h"
#include "pj_base/data_source_protocol.h"
#include "pj_base/toolbox_protocol.h"
#include "pj_plugins/dialog_protocol.h"
#include "pj_plugins/host/plugin_catalog.hpp"
#include "pj_runtime/PluginRuntimeCatalog.h"
#include "plugin_test_utils.h"

namespace PJ {
namespace {

using test::pluginFileName;

void* createStaticDialog() noexcept {
  return new int(0);
}
void destroyStaticDialog(void* ctx) noexcept {
  delete static_cast<int*>(ctx);
}
const char* staticDialogManifest(void*) noexcept {
  return R"({"id":"static-dialog","name":"Static Dialog","version":"1.0.0"})";
}
const char* staticDialogUi(void*) noexcept {
  return "";
}
const char* staticDialogData(void*) noexcept {
  return "{}";
}
bool staticDialogEvent(void*, const char*, const char*, PJ_error_t*) noexcept {
  return false;
}
bool staticDialogTick(void*, PJ_error_t*) noexcept {
  return false;
}
void staticDialogAccepted(void*, const char*) noexcept {}
void staticDialogRejected(void*) noexcept {}
bool staticDialogSave(void*, PJ_string_view_t* out, PJ_error_t*) noexcept {
  *out = PJ_string_view_t{"{}", 2};
  return true;
}
bool staticDialogLoad(void*, PJ_string_view_t, PJ_error_t*) noexcept {
  return true;
}

const PJ_dialog_vtable_t kStaticDialogVtable = {
    .protocol_version = PJ_DIALOG_PROTOCOL_VERSION,
    .struct_size = sizeof(PJ_dialog_vtable_t),
    .create = createStaticDialog,
    .destroy = destroyStaticDialog,
    .get_manifest = staticDialogManifest,
    .get_ui_content = staticDialogUi,
    .get_widget_data = staticDialogData,
    .on_widget_event = staticDialogEvent,
    .on_tick = staticDialogTick,
    .on_accepted = staticDialogAccepted,
    .on_rejected = staticDialogRejected,
    .save_config = staticDialogSave,
    .load_config = staticDialogLoad,
    .manifest_json = R"({"id":"static-dialog","name":"Static Dialog","version":"1.0.0"})",
    .set_host_info = nullptr,
};

class PluginCatalogTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("pj_catalog_test_" +
            std::to_string(static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())));
    std::filesystem::create_directories(dir_);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::filesystem::path dir_;
};

TEST_F(PluginCatalogTest, RuntimeCatalogDedupsDuplicateIdFirstFolderWins) {
  // The same data-source DSO (manifest id "mock-data-source") placed in two
  // folders: setPluginDirs scans them in order and must load it exactly once,
  // from the first (higher-priority) folder.
  const std::filesystem::path dir_a = dir_ / "a";
  const std::filesystem::path dir_b = dir_ / "b";
  std::filesystem::create_directories(dir_a);
  std::filesystem::create_directories(dir_b);
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, dir_a / pluginFileName("ds"));
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, dir_b / pluginFileName("ds"));

  PluginRuntimeCatalog catalog;
  catalog.setPluginDirs({{dir_a}, {dir_b}});
  catalog.scanDirectory();

  ASSERT_EQ(catalog.dataSources().size(), 1U);
  EXPECT_EQ(catalog.dataSources()[0].id, "mock-data-source");
  // The winner must come from the first folder. Compare at the filesystem level
  // (std::filesystem::equivalent) so it holds regardless of how the stored path
  // is spelled — Windows back/forward slashes, drive-letter case, canonicalisation.
  const std::filesystem::path winner(catalog.dataSources()[0].path);
  EXPECT_TRUE(std::filesystem::equivalent(winner.parent_path(), dir_a))
      << "winner " << winner << " is not in the first folder " << dir_a;
}

TEST_F(PluginCatalogTest, RuntimeCatalogSkipsDuplicateScanFolders) {
  // The same folder listed twice in setPluginDirs is scanned once: the second pass is
  // skipped, so no plugin is ever compared against itself — which would otherwise emit a
  // confusing "ignoring duplicate id … already loaded from <same path>" diagnostic.
  const std::filesystem::path dir_a = dir_ / "a";
  std::filesystem::create_directories(dir_a);
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, dir_a / pluginFileName("ds"));

  std::vector<std::string> messages;
  PluginRuntimeCatalog catalog({}, [&](const Diagnostic& d) { messages.push_back(d.message); });
  catalog.setPluginDirs({{dir_a}, {dir_a}});  // same folder twice
  catalog.scanDirectory();

  ASSERT_EQ(catalog.dataSources().size(), 1U);
  for (const std::string& message : messages) {
    EXPECT_EQ(message.find("ignoring duplicate"), std::string::npos)
        << "a folder listed twice must not trigger a self-dedup diagnostic: " << message;
  }
}

TEST_F(PluginCatalogTest, RuntimeCatalogPrefersHigherVersionOverFolderPriority) {
  // Same plugin id in two folders, but the LOWER-priority folder ships a newer
  // version (v2.0.0) than the higher-priority one (v1.0.0). Version wins over
  // folder priority, so the v2 DSO from the second folder is the one loaded.
  const std::filesystem::path dir_a = dir_ / "a";
  const std::filesystem::path dir_b = dir_ / "b";
  std::filesystem::create_directories(dir_a);
  std::filesystem::create_directories(dir_b);
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, dir_a / pluginFileName("ds"));     // v1.0.0
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_V2_PLUGIN_PATH, dir_b / pluginFileName("ds"));  // v2.0.0

  PluginRuntimeCatalog catalog;
  catalog.setPluginDirs({{dir_a}, {dir_b}});
  catalog.scanDirectory();

  ASSERT_EQ(catalog.dataSources().size(), 1U);
  EXPECT_EQ(catalog.dataSources()[0].id, "mock-data-source");
  EXPECT_EQ(catalog.dataSources()[0].version, "2.0.0");
  const std::filesystem::path winner(catalog.dataSources()[0].path);
  EXPECT_TRUE(std::filesystem::equivalent(winner.parent_path(), dir_b))
      << "winner " << winner << " should be the newer v2 in " << dir_b;
}

TEST_F(PluginCatalogTest, RuntimeCatalogKeepsHigherPriorityWhenNewerVersionIsHigherPriority) {
  // Mirror of the above: the newer version now sits in the HIGHER-priority folder.
  // The winner is still v2.0.0, confirming the tie-break never demotes it.
  const std::filesystem::path dir_a = dir_ / "a";
  const std::filesystem::path dir_b = dir_ / "b";
  std::filesystem::create_directories(dir_a);
  std::filesystem::create_directories(dir_b);
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_V2_PLUGIN_PATH, dir_a / pluginFileName("ds"));  // v2.0.0
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, dir_b / pluginFileName("ds"));     // v1.0.0

  PluginRuntimeCatalog catalog;
  catalog.setPluginDirs({{dir_a}, {dir_b}});
  catalog.scanDirectory();

  ASSERT_EQ(catalog.dataSources().size(), 1U);
  EXPECT_EQ(catalog.dataSources()[0].version, "2.0.0");
  const std::filesystem::path winner(catalog.dataSources()[0].path);
  EXPECT_TRUE(std::filesystem::equivalent(winner.parent_path(), dir_a));
}

TEST_F(PluginCatalogTest, RuntimeCatalogPrefersCompatibleOverHigherIncompatibleVersion) {
  // Higher-priority folder ships an incompatible v3.0.0 (needs PlotJuggler 5.0.0);
  // lower-priority folder ships a compatible v2.0.0. With a 4.0.0 host, the
  // compatible build wins even though it is both lower version and lower priority.
  const std::filesystem::path dir_a = dir_ / "a";
  const std::filesystem::path dir_b = dir_ / "b";
  std::filesystem::create_directories(dir_a);
  std::filesystem::create_directories(dir_b);
  std::filesystem::copy_file(
      PJ_MOCK_DATA_SOURCE_INCOMPATIBLE_PLUGIN_PATH, dir_a / pluginFileName("ds"));               // v3, min 5.0.0
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_V2_PLUGIN_PATH, dir_b / pluginFileName("ds"));  // v2, no min

  PluginRuntimeCatalog catalog;
  catalog.setHostVersion("4.0.0");
  catalog.setPluginDirs({{dir_a}, {dir_b}});
  catalog.scanDirectory();

  ASSERT_EQ(catalog.dataSources().size(), 1U);
  EXPECT_EQ(catalog.dataSources()[0].version, "2.0.0");
  const std::filesystem::path winner(catalog.dataSources()[0].path);
  EXPECT_TRUE(std::filesystem::equivalent(winner.parent_path(), dir_b))
      << "winner " << winner << " should be the compatible v2 in " << dir_b;
}

TEST_F(PluginCatalogTest, RuntimeCatalogRejectsLoneIncompatiblePlugin) {
  // Compatibility is an admission gate, not only a duplicate tie-break. A lone
  // plugin requiring a newer host must remain unloaded.
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_INCOMPATIBLE_PLUGIN_PATH, dir_ / pluginFileName("ds"));

  PluginRuntimeCatalog catalog;
  catalog.setHostVersion("4.0.0");
  catalog.setPluginDir(dir_);
  catalog.scanDirectory();

  EXPECT_TRUE(catalog.dataSources().empty());
}

TEST_F(PluginCatalogTest, RuntimeCatalogRejectsMalformedApplicationFloor) {
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_MALFORMED_FLOOR_PLUGIN_PATH, dir_ / pluginFileName("ds"));

  PluginRuntimeCatalog catalog;
  catalog.setHostVersion("4.0.0");
  catalog.setPluginDir(dir_);
  catalog.scanDirectory();

  EXPECT_TRUE(catalog.dataSources().empty());
}

TEST_F(PluginCatalogTest, RuntimeCatalogRejectsMalformedPluginVersion) {
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_MALFORMED_VERSION_PLUGIN_PATH, dir_ / pluginFileName("ds"));

  std::vector<Diagnostic> diagnostics;
  PluginRuntimeCatalog catalog({}, [&](const Diagnostic& diagnostic) { diagnostics.push_back(diagnostic); });
  catalog.setHostVersion("4.0.0");
  catalog.setPluginDir(dir_);
  catalog.scanDirectory();

  EXPECT_TRUE(catalog.dataSources().empty());
  EXPECT_TRUE(std::ranges::any_of(diagnostics, [](const Diagnostic& diagnostic) {
    return diagnostic.level == DiagnosticLevel::kError &&
           diagnostic.message.find("Invalid plugin version") != std::string::npos;
  }));
}

TEST_F(PluginCatalogTest, RuntimeCatalogRejectsNewerSdkRequirement) {
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_NEWER_SDK_PLUGIN_PATH, dir_ / pluginFileName("ds"));

  PluginRuntimeCatalog catalog;
  catalog.setHostVersion("4.0.0");
  catalog.setPluginDir(dir_);
  catalog.scanDirectory();

  EXPECT_TRUE(catalog.dataSources().empty());
}

TEST_F(PluginCatalogTest, RuntimeCatalogPrefersTheOnlyCompatibleAmongMixedCandidates) {
  // Three folders, same id, mixed compatibility: incompatible v3.0.0 (min 5.0.0) in
  // the highest-priority folder, compatible v2.0.0 in the middle, incompatible v1.5.0
  // (min 5.0.0) in the lowest. With a 4.0.0 host the compatible v2.0.0 wins — it is
  // neither the highest version nor the highest-priority folder.
  const std::filesystem::path dir_a = dir_ / "a";
  const std::filesystem::path dir_b = dir_ / "b";
  const std::filesystem::path dir_c = dir_ / "c";
  std::filesystem::create_directories(dir_a);
  std::filesystem::create_directories(dir_b);
  std::filesystem::create_directories(dir_c);
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_INCOMPATIBLE_PLUGIN_PATH, dir_a / pluginFileName("ds"));  // v3, min 5
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_V2_PLUGIN_PATH, dir_b / pluginFileName("ds"));            // v2, compat
  std::filesystem::copy_file(
      PJ_MOCK_DATA_SOURCE_INCOMPATIBLE_LOW_PLUGIN_PATH, dir_c / pluginFileName("ds"));  // v1.5, min 5

  PluginRuntimeCatalog catalog;
  catalog.setHostVersion("4.0.0");
  catalog.setPluginDirs({{dir_a}, {dir_b}, {dir_c}});
  catalog.scanDirectory();

  ASSERT_EQ(catalog.dataSources().size(), 1U);
  EXPECT_EQ(catalog.dataSources()[0].version, "2.0.0");
  const std::filesystem::path winner(catalog.dataSources()[0].path);
  EXPECT_TRUE(std::filesystem::equivalent(winner.parent_path(), dir_b))
      << "winner " << winner << " should be the only compatible candidate in " << dir_b;
}

TEST_F(PluginCatalogTest, RuntimeCatalogAuthoritativeFolderOverridesHigherVersion) {
  // An authoritative (user-explicit) folder is a hard override: its v1.0.0 wins over
  // a higher v2.0.0 in a lower-priority managed folder. Without the authoritative
  // mark, version would pick v2.0.0 — this isolates the override.
  const std::filesystem::path dir_a = dir_ / "a";  // authoritative
  const std::filesystem::path dir_b = dir_ / "b";  // managed
  std::filesystem::create_directories(dir_a);
  std::filesystem::create_directories(dir_b);
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, dir_a / pluginFileName("ds"));     // v1.0.0
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_V2_PLUGIN_PATH, dir_b / pluginFileName("ds"));  // v2.0.0

  PluginRuntimeCatalog catalog;
  catalog.setPluginDirs({{dir_a, true}, {dir_b}});
  catalog.scanDirectory();

  ASSERT_EQ(catalog.dataSources().size(), 1U);
  EXPECT_EQ(catalog.dataSources()[0].version, "1.0.0");
  const std::filesystem::path winner(catalog.dataSources()[0].path);
  EXPECT_TRUE(std::filesystem::equivalent(winner.parent_path(), dir_a));
}

TEST_F(PluginCatalogTest, RuntimeCatalogIncompatibleAuthoritativeCopyShadowsManagedFallback) {
  // --plugin-dir remains a strict override: its id shadows the managed copy.
  // Compatibility is still a safety gate, so an incompatible override yields no
  // loaded plugin rather than silently falling back to a different build.
  const std::filesystem::path dir_a = dir_ / "a";  // authoritative, incompatible v3.0.0
  const std::filesystem::path dir_b = dir_ / "b";  // managed, compatible v2.0.0
  std::filesystem::create_directories(dir_a);
  std::filesystem::create_directories(dir_b);
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_INCOMPATIBLE_PLUGIN_PATH, dir_a / pluginFileName("ds"));  // v3, min 5
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_V2_PLUGIN_PATH, dir_b / pluginFileName("ds"));            // v2, compat

  PluginRuntimeCatalog catalog;
  catalog.setHostVersion("4.0.0");
  catalog.setPluginDirs({{dir_a, true}, {dir_b}});
  catalog.scanDirectory();

  EXPECT_TRUE(catalog.dataSources().empty());
}

TEST_F(PluginCatalogTest, RuntimeCatalogAuthoritativeFolderMayBeASymlink) {
  // The authoritative tier is a per-entry flag, so it must hold regardless of how
  // the folder is spelled — here the scan entry is a symlink to the real folder.
  const std::filesystem::path real_dir = dir_ / "real";  // symlink target, authoritative v1.0.0
  const std::filesystem::path link_dir = dir_ / "link";  // authoritative spelling (a symlink)
  const std::filesystem::path dir_b = dir_ / "b";        // managed, v2.0.0

  std::filesystem::create_directories(real_dir);
  std::filesystem::create_directories(dir_b);
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, real_dir / pluginFileName("ds"));  // v1.0.0
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_V2_PLUGIN_PATH, dir_b / pluginFileName("ds"));  // v2.0.0
  std::error_code ec;
  std::filesystem::create_directory_symlink(real_dir, link_dir, ec);
  if (ec) {
    GTEST_SKIP() << "filesystem does not support directory symlinks: " << ec.message();
  }

  PluginRuntimeCatalog catalog;
  catalog.setPluginDirs({{link_dir, true}, {dir_b}});
  catalog.scanDirectory();

  ASSERT_EQ(catalog.dataSources().size(), 1U);
  EXPECT_EQ(catalog.dataSources()[0].version, "1.0.0") << "authoritative override must hold for a symlinked scan entry";
  const std::filesystem::path winner(catalog.dataSources()[0].path);
  EXPECT_TRUE(std::filesystem::equivalent(winner.parent_path(), real_dir));
}

TEST_F(PluginCatalogTest, RuntimeCatalogAuthoritativeFolderSupersedesEarlierManagedIncumbent) {
  // The authoritative folder is scanned AFTER a managed one (it sits lower in the scan
  // list), so a managed incumbent is already recorded when the authoritative copy arrives.
  // The authoritative candidate must still supersede it — the override is not order
  // dependent. Complements the existing tests, which place the authoritative folder first.
  const std::filesystem::path dir_managed = dir_ / "managed";  // higher priority, managed, v2.0.0
  const std::filesystem::path dir_auth = dir_ / "auth";        // lower priority, authoritative, v1.0.0
  std::filesystem::create_directories(dir_managed);
  std::filesystem::create_directories(dir_auth);
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_V2_PLUGIN_PATH, dir_managed / pluginFileName("ds"));  // v2.0.0
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, dir_auth / pluginFileName("ds"));        // v1.0.0

  PluginRuntimeCatalog catalog;
  catalog.setPluginDirs({{dir_managed}, {dir_auth, true}});  // managed scanned first, authoritative second
  catalog.scanDirectory();

  ASSERT_EQ(catalog.dataSources().size(), 1U);
  EXPECT_EQ(catalog.dataSources()[0].version, "1.0.0")
      << "an authoritative folder must override an already-recorded managed incumbent";
  const std::filesystem::path winner(catalog.dataSources()[0].path);
  EXPECT_TRUE(std::filesystem::equivalent(winner.parent_path(), dir_auth));
}

TEST_F(PluginCatalogTest, RuntimeCatalogLoadsDistinctIdsFromMultipleFolders) {
  // Different plugins in different folders all load.
  const std::filesystem::path dir_a = dir_ / "a";
  const std::filesystem::path dir_b = dir_ / "b";
  std::filesystem::create_directories(dir_a);
  std::filesystem::create_directories(dir_b);
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, dir_a / pluginFileName("ds"));
  std::filesystem::copy_file(PJ_MOCK_TOOLBOX_PLUGIN_PATH, dir_b / pluginFileName("tb"));

  PluginRuntimeCatalog catalog;
  catalog.setPluginDirs({{dir_a}, {dir_b}});
  catalog.scanDirectory();

  EXPECT_EQ(catalog.dataSources().size(), 1U);
  EXPECT_EQ(catalog.toolboxes().size(), 1U);
}

TEST_F(PluginCatalogTest, RuntimeCatalogStaticRegistrationSurvivesScanAndReload) {
  // A statically registered plugin has no backing file, so the disk
  // reconciliation in scanDirectory()/reload() must leave it loaded — its
  // synthetic "static://" path never appears among the scanned DSO paths.
  static const PJ_data_source_vtable_t vt =
      pj_mock::makeMockDataSourceVtable(R"({"id":"static-mock","name":"Static Mock","version":"1.0.0"})");

  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, dir_ / pluginFileName("ds"));

  PluginRuntimeCatalog catalog;
  catalog.setPluginDir(dir_);
  ASSERT_TRUE(catalog.registerStaticDataSource(&vt));
  ASSERT_EQ(catalog.dataSources().size(), 1U);

  catalog.scanDirectory();
  ASSERT_EQ(catalog.dataSources().size(), 2U) << "scanDirectory() must keep the static registration";

  EXPECT_FALSE(catalog.reload()) << "an unchanged disk state must not count as a change";
  ASSERT_EQ(catalog.dataSources().size(), 2U) << "reload() must not evict the static registration";
  EXPECT_TRUE(std::ranges::any_of(catalog.dataSources(), [](const RuntimeDataSourcePlugin& plugin) {
    return plugin.id == "static-mock";
  }));
}

TEST_F(PluginCatalogTest, StaticPluginSetParticipatesInFilteringAndHandleCreation) {
  // The application supplies entry points as data; the runtime owns the
  // registration policy. Prove the composed path, rather than only the
  // individual registerStaticDataSource() primitive.
  static const PJ_data_source_vtable_t vt = [] {
    auto value = pj_mock::makeMockDataSourceVtable(
        R"({"id":"static-file","name":"Static File","version":"1.0.0","file_extensions":[".probe"]})");
    value.capabilities = [](void*) noexcept -> uint64_t {
      return PJ_DATA_SOURCE_CAPABILITY_FINITE_IMPORT | PJ_DATA_SOURCE_CAPABILITY_DIRECT_INGEST;
    };
    return value;
  }();

  PluginRuntimeCatalog catalog;
  StaticPluginSet plugins;
  plugins.data_sources.emplace_back(&vt, &kStaticDialogVtable);
  ASSERT_TRUE(catalog.registerStaticPlugins(plugins));

  const auto matches = catalog.findSourcesForExtension(".PROBE");
  ASSERT_EQ(matches.size(), 1U);
  EXPECT_EQ(matches.front()->id, "static-file");
  EXPECT_TRUE(matches.front()->library.createHandle().valid());
  const auto dialog = matches.front()->library.resolveDialogVtable();
  ASSERT_TRUE(dialog.has_value());
  EXPECT_EQ(*dialog, &kStaticDialogVtable);
  EXPECT_NE(catalog.buildFileFilter().find("*.probe"), std::string::npos);

  catalog.scanDirectory();
  ASSERT_EQ(catalog.dataSources().size(), 1U);
  EXPECT_EQ(catalog.dataSources().front().path, "static://static-file");
}

TEST_F(PluginCatalogTest, RuntimeCatalogStaticRegistrationShadowsDsoWithSameId) {
  // A statically registered id outranks every scan folder: the DSO sharing the
  // id ("mock-data-source") must be skipped by scanDirectory(), and a later
  // static registration of an id already loaded from a DSO must evict the DSO.
  static const PJ_data_source_vtable_t vt =
      pj_mock::makeMockDataSourceVtable(R"({"id":"mock-data-source","name":"Static Mock","version":"0.1.0"})");

  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_V2_PLUGIN_PATH, dir_ / pluginFileName("ds"));  // v2.0.0 DSO

  PluginRuntimeCatalog catalog;
  catalog.setPluginDir(dir_);
  ASSERT_TRUE(catalog.registerStaticDataSource(&vt));

  catalog.scanDirectory();
  ASSERT_EQ(catalog.dataSources().size(), 1U) << "the same-id DSO must be shadowed by the static registration";
  EXPECT_EQ(catalog.dataSources()[0].version, "0.1.0");

  // Order-independence: load the DSO first, then register the static copy.
  PluginRuntimeCatalog catalog_dso_first;
  catalog_dso_first.setPluginDir(dir_);
  catalog_dso_first.scanDirectory();
  ASSERT_EQ(catalog_dso_first.dataSources().size(), 1U);
  EXPECT_EQ(catalog_dso_first.dataSources()[0].version, "2.0.0");
  ASSERT_TRUE(catalog_dso_first.registerStaticDataSource(&vt));
  ASSERT_EQ(catalog_dso_first.dataSources().size(), 1U) << "the static registration must evict the same-id DSO";
  EXPECT_EQ(catalog_dso_first.dataSources()[0].version, "0.1.0");

  // A second static registration of the same id is a caller bug: rejected.
  EXPECT_FALSE(catalog_dso_first.registerStaticDataSource(&vt));
  EXPECT_EQ(catalog_dso_first.dataSources().size(), 1U);

  // reload() rescans the folders; the same-id DSO on disk must stay shadowed
  // and the unchanged catalog must report "no change".
  EXPECT_FALSE(catalog_dso_first.reload());
  ASSERT_EQ(catalog_dso_first.dataSources().size(), 1U);
  EXPECT_EQ(catalog_dso_first.dataSources()[0].version, "0.1.0");
}

TEST_F(PluginCatalogTest, RejectedStaticRegistrationKeepsSameIdDsoLoaded) {
  // Replacing a DSO with a static plugin is a transaction: the incumbent must
  // remain usable unless the static candidate passes all validation. In
  // particular, a candidate whose create() probe fails must not evict the DSO.
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_V2_PLUGIN_PATH, dir_ / pluginFileName("ds"));

  PluginRuntimeCatalog catalog;
  catalog.setPluginDir(dir_);
  catalog.scanDirectory();

  ASSERT_EQ(catalog.dataSources().size(), 1U);
  ASSERT_EQ(catalog.dataSources().front().id, "mock-data-source");
  ASSERT_EQ(catalog.dataSources().front().version, "2.0.0");
  const auto original_path = catalog.dataSources().front().path;

  static const PJ_data_source_vtable_t invalid_static = pj_mock::makeMockDataSourceVtable(
      R"({"id":"mock-data-source","name":"Invalid Static","version":"3.0.0"})", pj_mock::detail::createNull);

  EXPECT_FALSE(catalog.registerStaticDataSource(&invalid_static));

  ASSERT_EQ(catalog.dataSources().size(), 1U)
      << "rejecting an invalid static replacement must not evict the same-id DSO";
  EXPECT_EQ(catalog.dataSources().front().id, "mock-data-source");
  EXPECT_EQ(catalog.dataSources().front().version, "2.0.0");
  EXPECT_EQ(catalog.dataSources().front().path, original_path);
  EXPECT_TRUE(catalog.dataSources().front().library.createHandle().valid());
}

TEST_F(PluginCatalogTest, RuntimeCatalogStaticRegistrationRejectsMistypedManifest) {
  // A well-formed manifest with mistyped fields ("id":123) must fail with a
  // diagnostic, not escape as a JSON exception (the documented contract).
  static const PJ_data_source_vtable_t vt =
      pj_mock::makeMockDataSourceVtable(R"({"id":123,"name":"Bad Types","version":"1.0.0"})");

  std::vector<std::string> messages;
  PluginRuntimeCatalog catalog({}, [&](const Diagnostic& d) { messages.push_back(d.message); });
  EXPECT_FALSE(catalog.registerStaticDataSource(&vt));
  EXPECT_TRUE(catalog.dataSources().empty());
  EXPECT_FALSE(messages.empty());
}

TEST_F(PluginCatalogTest, RuntimeCatalogStaticRegistrationRejectsNewerSdkRequirement) {
  static const PJ_data_source_vtable_t vt = pj_mock::makeMockDataSourceVtable(
      R"({"id":"future-static","name":"Future Static","version":"1.0.0","min_sdk_required":"99.0.0"})");

  std::vector<std::string> messages;
  PluginRuntimeCatalog catalog({}, [&](const Diagnostic& d) { messages.push_back(d.message); });
  catalog.setHostVersion("4.0.0");

  EXPECT_FALSE(catalog.registerStaticDataSource(&vt));
  EXPECT_TRUE(catalog.dataSources().empty());
  EXPECT_TRUE(std::ranges::any_of(messages, [](const std::string& message) {
    return message.find("SDK") != std::string::npos && message.find("99.0.0") != std::string::npos;
  }));
}

TEST_F(PluginCatalogTest, RuntimeCatalogStaticRegistrationRejectsNullCreate) {
  // A plugin whose create() returns null must be rejected by the validity
  // probe, never handed a null context via capabilities().
  static const PJ_data_source_vtable_t vt = pj_mock::makeMockDataSourceVtable(
      R"({"id":"null-create","name":"Null Create","version":"1.0.0"})", pj_mock::detail::createNull);

  std::vector<std::string> messages;
  PluginRuntimeCatalog catalog({}, [&](const Diagnostic& d) { messages.push_back(d.message); });
  EXPECT_FALSE(catalog.registerStaticDataSource(&vt));
  EXPECT_TRUE(catalog.dataSources().empty());
  EXPECT_FALSE(messages.empty());
}

TEST_F(PluginCatalogTest, RuntimeCatalogStaticRegistrationRequiresManifestId) {
  // Static registration shares the DSO scan's full required-key set (the SDK's
  // decodeManifest): id — the only identity a static plugin has, its synthetic
  // path derives from it — plus name and version, each a non-empty string.
  static const PJ_data_source_vtable_t no_id =
      pj_mock::makeMockDataSourceVtable(R"({"name":"No Id","version":"1.0.0"})");
  static const PJ_data_source_vtable_t no_name = pj_mock::makeMockDataSourceVtable(R"({"id":"x","version":"1.0.0"})");
  static const PJ_data_source_vtable_t no_version = pj_mock::makeMockDataSourceVtable(R"({"id":"x","name":"X"})");

  std::vector<std::string> messages;
  PluginRuntimeCatalog catalog({}, [&](const Diagnostic& d) { messages.push_back(d.message); });
  EXPECT_FALSE(catalog.registerStaticDataSource(&no_id));
  EXPECT_FALSE(catalog.registerStaticDataSource(&no_name));
  EXPECT_FALSE(catalog.registerStaticDataSource(&no_version));
  EXPECT_TRUE(catalog.dataSources().empty());
  EXPECT_EQ(messages.size(), 3U);
}

TEST_F(PluginCatalogTest, RuntimeCatalogStaticDataSourceRejectsHasDialogWithoutVtable) {
  // Fail-fast parity with the dynamic (DSO) path: a plugin that advertises
  // HAS_DIALOG but has no resolvable dialog vtable must be rejected with a
  // kError diagnostic, never silently registered as "no dialog".
  static const PJ_data_source_vtable_t vt = [] {
    auto value = pj_mock::makeMockDataSourceVtable(R"({"id":"lying-dialog","name":"Lying Dialog","version":"1.0.0"})");
    value.capabilities = [](void*) noexcept -> uint64_t { return PJ_DATA_SOURCE_CAPABILITY_HAS_DIALOG; };
    return value;
  }();

  std::vector<Diagnostic> diagnostics;
  PluginRuntimeCatalog catalog({}, [&](const Diagnostic& d) { diagnostics.push_back(d); });
  EXPECT_FALSE(catalog.registerStaticDataSource(&vt, /*dialog_vtable=*/nullptr));
  EXPECT_TRUE(catalog.dataSources().empty());
  EXPECT_TRUE(std::ranges::any_of(diagnostics, [](const Diagnostic& d) {
    return d.level == DiagnosticLevel::kError && d.message.find("HasDialog") != std::string::npos;
  })) << "expected a kError diagnostic mentioning the HasDialog capability";
}

TEST_F(PluginCatalogTest, RuntimeCatalogStaticDataSourceAcceptsHasDialogWithVtable) {
  // Mirror of the above with a valid dialog vtable supplied: registration
  // succeeds and no error diagnostic is recorded.
  static const PJ_data_source_vtable_t vt = [] {
    auto value =
        pj_mock::makeMockDataSourceVtable(R"({"id":"honest-dialog","name":"Honest Dialog","version":"1.0.0"})");
    value.capabilities = [](void*) noexcept -> uint64_t { return PJ_DATA_SOURCE_CAPABILITY_HAS_DIALOG; };
    return value;
  }();

  std::vector<Diagnostic> diagnostics;
  PluginRuntimeCatalog catalog({}, [&](const Diagnostic& d) { diagnostics.push_back(d); });
  EXPECT_TRUE(catalog.registerStaticDataSource(&vt, &kStaticDialogVtable));
  ASSERT_EQ(catalog.dataSources().size(), 1U);
  EXPECT_EQ(catalog.dataSources()[0].id, "honest-dialog");
  EXPECT_TRUE(
      std::ranges::none_of(diagnostics, [](const Diagnostic& d) { return d.level == DiagnosticLevel::kError; }));
}

TEST_F(PluginCatalogTest, RuntimeCatalogStaticToolboxRejectsHasDialogWithoutVtable) {
  // Same fail-fast contract as DataSource, for the Toolbox static path.
  static const PJ_toolbox_vtable_t vt = [] {
    auto value =
        pj_mock::makeMockToolboxVtable(R"({"id":"lying-dialog-toolbox","name":"Lying Dialog","version":"1.0.0"})");
    value.capabilities = [](void*) noexcept -> uint64_t { return PJ_TOOLBOX_CAPABILITY_HAS_DIALOG; };
    return value;
  }();

  std::vector<Diagnostic> diagnostics;
  PluginRuntimeCatalog catalog({}, [&](const Diagnostic& d) { diagnostics.push_back(d); });
  EXPECT_FALSE(catalog.registerStaticToolbox(&vt, /*dialog_vtable=*/nullptr));
  EXPECT_TRUE(catalog.toolboxes().empty());
  EXPECT_TRUE(std::ranges::any_of(diagnostics, [](const Diagnostic& d) {
    return d.level == DiagnosticLevel::kError && d.message.find("HasDialog") != std::string::npos;
  })) << "expected a kError diagnostic mentioning the HasDialog capability";
}

TEST_F(PluginCatalogTest, RuntimeCatalogStaticToolboxAcceptsHasDialogWithVtable) {
  static const PJ_toolbox_vtable_t vt = [] {
    auto value =
        pj_mock::makeMockToolboxVtable(R"({"id":"honest-dialog-toolbox","name":"Honest Dialog","version":"1.0.0"})");
    value.capabilities = [](void*) noexcept -> uint64_t { return PJ_TOOLBOX_CAPABILITY_HAS_DIALOG; };
    return value;
  }();

  std::vector<Diagnostic> diagnostics;
  PluginRuntimeCatalog catalog({}, [&](const Diagnostic& d) { diagnostics.push_back(d); });
  EXPECT_TRUE(catalog.registerStaticToolbox(&vt, &kStaticDialogVtable));
  ASSERT_EQ(catalog.toolboxes().size(), 1U);
  EXPECT_EQ(catalog.toolboxes()[0].id, "honest-dialog-toolbox");
  const auto dialog = catalog.toolboxes()[0].library.resolveDialogVtable();
  ASSERT_TRUE(dialog.has_value()) << "the companion dialog vtable must resolve through the loaded library";
  EXPECT_EQ(*dialog, &kStaticDialogVtable);
  EXPECT_TRUE(
      std::ranges::none_of(diagnostics, [](const Diagnostic& d) { return d.level == DiagnosticLevel::kError; }));
}

TEST_F(PluginCatalogTest, RuntimeCatalogStaticRegistrationRejectsBadVtable) {
  // The loadStatic gate runs before any manifest work: a null vtable and a
  // protocol-version mismatch must both fail with a diagnostic, not register.
  static const PJ_data_source_vtable_t wrong_protocol = [] {
    auto value = pj_mock::makeMockDataSourceVtable(R"({"id":"bad","name":"Bad","version":"1.0.0"})");
    value.protocol_version = 999;
    return value;
  }();

  std::vector<std::string> messages;
  PluginRuntimeCatalog catalog({}, [&](const Diagnostic& d) { messages.push_back(d.message); });
  EXPECT_FALSE(catalog.registerStaticDataSource(nullptr));
  EXPECT_FALSE(catalog.registerStaticDataSource(&wrong_protocol));
  EXPECT_TRUE(catalog.dataSources().empty());
  EXPECT_EQ(messages.size(), 2U);
}

TEST_F(PluginCatalogTest, RuntimeCatalogStaticMessageParserRequiresEncoding) {
  // The manifest contract (message_parser_protocol.h) requires a non-empty
  // "encoding" array — it is how binding requests find a parser, so an
  // encoding-less static parser is useless. The SDK's shared decodeManifest
  // enforces this identically for DSO and static registration; a laxer static
  // path would let such a parser evict a working same-id DSO parser.
  static const PJ_message_parser_vtable_t no_encoding =
      pj_mock::makeMockMessageParserVtable(R"({"id":"static-parser","name":"Static Parser","version":"1.0.0"})");
  static const PJ_message_parser_vtable_t empty_encoding = pj_mock::makeMockMessageParserVtable(
      R"({"id":"static-parser","name":"Static Parser","version":"1.0.0","encoding":[]})");

  std::vector<Diagnostic> diagnostics;
  PluginRuntimeCatalog catalog({}, [&](const Diagnostic& d) { diagnostics.push_back(d); });
  EXPECT_FALSE(catalog.registerStaticMessageParser(&no_encoding));
  EXPECT_FALSE(catalog.registerStaticMessageParser(&empty_encoding));
  EXPECT_TRUE(catalog.messageParsers().empty());
  EXPECT_TRUE(std::ranges::any_of(diagnostics, [](const Diagnostic& d) {
    return d.level == DiagnosticLevel::kError && d.message.find("encoding") != std::string::npos;
  })) << "expected a kError diagnostic naming the missing encoding array";
}

TEST_F(PluginCatalogTest, RuntimeCatalogStaticMessageParserRegistersWithEncodings) {
  static const PJ_message_parser_vtable_t vt = pj_mock::makeMockMessageParserVtable(
      R"({"id":"static-parser","name":"Static Parser","version":"1.0.0","encoding":["ros2","cdr"]})");

  PluginRuntimeCatalog catalog;
  EXPECT_TRUE(catalog.registerStaticMessageParser(&vt));
  ASSERT_EQ(catalog.messageParsers().size(), 1U);
  EXPECT_EQ(catalog.messageParsers()[0].id, "static-parser");
  EXPECT_EQ(catalog.messageParsers()[0].encodings, (std::vector<std::string>{"ros2", "cdr"}));
  EXPECT_TRUE(catalog.messageParsers()[0].statically_registered);
  EXPECT_FALSE(catalog.messageParsers()[0].from_authoritative_dir);
  EXPECT_EQ(catalog.findParserById("static-parser"), &catalog.messageParsers()[0]);
  EXPECT_EQ(catalog.findParserById("missing"), nullptr);
}

TEST_F(PluginCatalogTest, RegisterStaticPluginsContinuesPastFailedEntryAndKeepsSuccesses) {
  // One bad entry in a StaticPluginSet must not abort the batch: the composed
  // application still gets every valid plugin, and the batch reports failure.
  static const PJ_data_source_vtable_t good_source =
      pj_mock::makeMockDataSourceVtable(R"({"id":"good-source","name":"Good Source","version":"1.0.0"})");
  static const PJ_data_source_vtable_t bad_source =
      pj_mock::makeMockDataSourceVtable(R"({"name":"No Id","version":"1.0.0"})");
  static const PJ_message_parser_vtable_t good_parser = pj_mock::makeMockMessageParserVtable(
      R"({"id":"good-parser","name":"Good Parser","version":"1.0.0","encoding":["ros2"]})");

  StaticPluginSet set;
  set.data_sources.push_back({&bad_source, nullptr});
  set.data_sources.push_back({&good_source, nullptr});
  set.message_parsers.push_back({&good_parser, nullptr});

  PluginRuntimeCatalog catalog;
  EXPECT_FALSE(catalog.registerStaticPlugins(set)) << "the failed entry must surface in the batch result";
  ASSERT_EQ(catalog.dataSources().size(), 1U);
  EXPECT_EQ(catalog.dataSources()[0].id, "good-source");
  ASSERT_EQ(catalog.messageParsers().size(), 1U);
  EXPECT_EQ(catalog.messageParsers()[0].id, "good-parser");
}

// ─── setDisabledIds (installed-but-not-loaded winners are dropped before load) ──

TEST_F(PluginCatalogTest, RuntimeCatalogDropsDisabledIdsFromWinners) {
  // A winner whose id sits in disabled_ids_ must not appear in the loaded set —
  // this is what turns the marketplace's "disable installed plugin" toggle into a
  // real runtime effect on the next launch. The diagnostic tells the user WHY
  // they don't see the plugin.
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, dir_ / pluginFileName("ds"));

  std::vector<Diagnostic> diagnostics;
  PluginRuntimeCatalog catalog({}, [&](const Diagnostic& d) { diagnostics.push_back(d); });
  catalog.setPluginDir(dir_);
  catalog.setDisabledIds({"mock-data-source"});
  catalog.scanDirectory();

  EXPECT_TRUE(catalog.dataSources().empty()) << "the disabled id must not be loaded";
  EXPECT_TRUE(std::ranges::any_of(diagnostics, [](const Diagnostic& d) {
    return d.level == DiagnosticLevel::kInfo && d.message.find("disabled by user") != std::string::npos;
  })) << "an info diagnostic must explain the skip";
}

TEST_F(PluginCatalogTest, RuntimeCatalogKeepsIdsAbsentFromDisabledSet) {
  // The filtering must be scoped: an unrelated id (or an empty disabled set)
  // leaves every winner in place. Otherwise a copy/paste bug in setDisabledIds
  // could quietly kill the loaded plugin set on every launch.
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, dir_ / pluginFileName("ds"));

  PluginRuntimeCatalog catalog;
  catalog.setPluginDir(dir_);
  catalog.setDisabledIds({"unrelated-id"});
  catalog.scanDirectory();

  ASSERT_EQ(catalog.dataSources().size(), 1U);
  EXPECT_EQ(catalog.dataSources()[0].id, "mock-data-source");
}

TEST_F(PluginCatalogTest, RuntimeCatalogReloadHonoursNewlyDisabledId) {
  // The disabled set is consulted on every scan, not baked in at construction:
  // toggling a plugin off after startup, then calling reload(), must remove it
  // from the loaded set. reload() reports the change (returns true), which is
  // the signal the host uses to re-notify the app runtime.
  std::filesystem::copy_file(PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, dir_ / pluginFileName("ds"));

  PluginRuntimeCatalog catalog;
  catalog.setPluginDir(dir_);
  catalog.scanDirectory();
  ASSERT_EQ(catalog.dataSources().size(), 1U);

  catalog.setDisabledIds({"mock-data-source"});
  EXPECT_TRUE(catalog.reload()) << "reload must report a change when a winner drops out";
  EXPECT_TRUE(catalog.dataSources().empty());
}

}  // namespace
}  // namespace PJ
