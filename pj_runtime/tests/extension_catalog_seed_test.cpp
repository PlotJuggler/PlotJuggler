// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Bundled-plugin seeding: the bundled (share) dir is a seed source only —
// ExtensionCatalogService syncs it into the marketplace dir at construction and
// never scans it as a load path. Covers the seed rules (copy / version-aware
// refresh / never-downgrade / reseed-on-unreadable), the seed running in
// --plugin-dir mode, the default-mode-only core lock, and the override dir
// outranking a newer marketplace copy.

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QString>
#include <QTemporaryDir>
#include <memory>
#include <string>

#include "mock_data_source_vtable.h"
#include "pj_base/plugin_data_api.h"
#include "pj_marketplace/download_manager.hpp"
#include "pj_marketplace/extension_manager.hpp"
#include "pj_marketplace/loaded_profile.hpp"
#include "pj_marketplace/platform_utils.hpp"
#include "pj_plugins/host/plugin_catalog.hpp"
#include "pj_runtime/ExtensionCatalogService.h"
#include "plugin_test_utils.h"

namespace PJ {
namespace {

// Thin QString adapter over the shared platform-suffix helper.
QString pluginFileName(const QString& stem) {
  return QString::fromStdString(test::pluginFileName(stem.toStdString()));
}

// Both mock DSOs carry the manifest id "mock-data-source": v1 reports version
// 1.0.0, v2 reports 2.0.0.
constexpr const char* kMockId = "mock-data-source";

class ExtensionCatalogSeedTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(bundled_.isValid());
    ASSERT_TRUE(marketplace_.isValid());
    ASSERT_TRUE(override_.isValid());
  }

  // Copies a mock plugin DSO into dir under the given file name.
  static void placePlugin(const QString& dir, const char* source_path, const QString& file_name) {
    ASSERT_TRUE(QDir().mkpath(dir));
    ASSERT_TRUE(QFile::copy(QString::fromUtf8(source_path), dir + "/" + file_name));
  }

  // Service in default mode (no --plugin-dir): the marketplace dir is both the
  // managed install dir and the seed destination.
  [[nodiscard]] std::unique_ptr<ExtensionCatalogService> makeDefaultModeService() const {
    return std::make_unique<ExtensionCatalogService>(
        ExtensionCatalogService::Paths{{}, marketplace_.path(), bundled_.path()}, DiagnosticSink{}, nullptr);
  }

  // Service in --plugin-dir mode: override_ is the managed install dir.
  [[nodiscard]] std::unique_ptr<ExtensionCatalogService> makeOverrideModeService() const {
    return std::make_unique<ExtensionCatalogService>(
        ExtensionCatalogService::Paths{override_.path(), marketplace_.path(), bundled_.path()}, DiagnosticSink{},
        nullptr);
  }

  // The single loaded DataSource matching the mock id, or nullptr.
  static const LoadedDataSource* findMock(const ExtensionCatalogService& service) {
    for (const LoadedDataSource& plugin : service.dataSources()) {
      if (plugin.id == kMockId) {
        return &plugin;
      }
    }
    return nullptr;
  }

  QString seededDir() const {
    return marketplace_.path() + "/" + kMockId;
  }

  QTemporaryDir bundled_;
  QTemporaryDir marketplace_;
  QTemporaryDir override_;
};

TEST_F(ExtensionCatalogSeedTest, FirstLaunchSeedsBundledIntoMarketplaceDir) {
  placePlugin(bundled_.path(), PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, pluginFileName("ds"));

  const auto service = makeDefaultModeService();

  EXPECT_TRUE(QDir(seededDir()).exists()) << "bundled plugin must be copied under <marketplace>/<id>";
  const LoadedDataSource* mock = findMock(*service);
  ASSERT_NE(mock, nullptr) << "seeded plugin must load from the marketplace dir";
  EXPECT_EQ(mock->version, "1.0.0");
  // Default mode: the bundled id -> version map reaches the ExtensionManager
  // (uninstall lock + downgrade-to-bundled).
  EXPECT_TRUE(service->extensionManager().isBundled(QString::fromUtf8(kMockId)));
  EXPECT_EQ(service->extensionManager().bundledVersion(QString::fromUtf8(kMockId)), "1.0.0");
}

// Regression: a plugin whose directory appears in the managed dir AFTER the
// service is built — an install done while running — must load on reload()
// without a restart. The managed scan tier is one directory per installed
// extension and is rebuilt on every reload, so a mid-session install is picked
// up; a snapshot frozen at construction would miss it.
TEST_F(ExtensionCatalogSeedTest, ReloadPicksUpAPluginInstalledAfterConstruction) {
  const auto service = makeDefaultModeService();
  ASSERT_EQ(findMock(*service), nullptr) << "nothing installed yet";

  // Simulate an install landing in the managed dir after construction.
  placePlugin(seededDir(), PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, pluginFileName("ds"));
  ASSERT_EQ(findMock(*service), nullptr) << "an unscanned on-disk plugin is not loaded until reload";

  service->reload();

  const LoadedDataSource* mock = findMock(*service);
  ASSERT_NE(mock, nullptr) << "reload must pick up a plugin installed since construction";
  EXPECT_EQ(mock->version, "1.0.0");
}

// Regression: a bundled refresh writes extensions/<id>/ outside the adopt path, so
// a store object recorded for that id by an earlier registry install goes stale.
// The seed must drop the mapping, or the runtime keeps loading the old store
// version instead of the freshly-seeded bundled bytes.
TEST_F(ExtensionCatalogSeedTest, BundledRefreshDropsStaleStoreMapping) {
  const QString store_root = PlatformUtils::canonicalStoreRoot(marketplace_.path());
  const QString profiles_root = store_root + ".profiles";

  // Installed v1 in the managed dir, a newer v2 bundled to refresh it.
  placePlugin(seededDir(), PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, pluginFileName("old_v1"));
  placePlugin(bundled_.path(), PJ_MOCK_DATA_SOURCE_V2_PLUGIN_PATH, pluginFileName("ds"));

  // A stale store object + profile mapping, as an earlier adopt of the v1 install
  // would have left. Without the fix managedPluginDirs would prefer this v1 object.
  const QString digest_hex = QStringLiteral("a").repeated(64);
  placePlugin(store_root + ".artifacts/" + digest_hex, PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, pluginFileName("ds"));
  ASSERT_TRUE(writeLoadedProfile(profiles_root, {{QString::fromUtf8(kMockId), "sha256:" + digest_hex}}));

  const auto service = makeDefaultModeService();

  const LoadedDataSource* mock = findMock(*service);
  ASSERT_NE(mock, nullptr);
  EXPECT_EQ(mock->version, "2.0.0") << "the refreshed bundled version must load, not the stale store object";
  EXPECT_FALSE(readLoadedProfile(profiles_root).contains(QString::fromUtf8(kMockId)))
      << "the seed must drop the stale store mapping";
}

TEST_F(ExtensionCatalogSeedTest, RefreshesSeededCopyWhenBundledIsNewer) {
  placePlugin(seededDir(), PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, pluginFileName("old_v1"));     // installed 1.0.0
  placePlugin(bundled_.path(), PJ_MOCK_DATA_SOURCE_V2_PLUGIN_PATH, pluginFileName("ds"));  // bundled 2.0.0

  const auto service = makeDefaultModeService();

  const LoadedDataSource* mock = findMock(*service);
  ASSERT_NE(mock, nullptr);
  EXPECT_EQ(mock->version, "2.0.0") << "an older installed copy must be refreshed to the bundled version";
  // The refresh replaces the payload; the old version's files do not linger.
  EXPECT_FALSE(QFile::exists(seededDir() + "/" + pluginFileName("old_v1")));
  // The staging area (a sibling of the marketplace dir, never inside it) is
  // removed once the refresh lands.
  EXPECT_FALSE(QDir(marketplace_.path() + ".seed_stage").exists());
  EXPECT_FALSE(QDir(marketplace_.path() + "/.seed_stage").exists());
}

TEST_F(ExtensionCatalogSeedTest, NeverDowngradesAnInstallAheadOfBundled) {
  placePlugin(seededDir(), PJ_MOCK_DATA_SOURCE_V2_PLUGIN_PATH, pluginFileName("user_v2"));  // installed 2.0.0
  placePlugin(bundled_.path(), PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, pluginFileName("ds"));      // bundled 1.0.0

  const auto service = makeDefaultModeService();

  const LoadedDataSource* mock = findMock(*service);
  ASSERT_NE(mock, nullptr);
  EXPECT_EQ(mock->version, "2.0.0") << "a marketplace update above the bundled version must survive the seed";
  EXPECT_TRUE(QFile::exists(seededDir() + "/" + pluginFileName("user_v2")));
}

TEST_F(ExtensionCatalogSeedTest, EqualVersionNeverRecopies) {
  placePlugin(seededDir(), PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, pluginFileName("user_named"));  // installed 1.0.0
  placePlugin(bundled_.path(), PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, pluginFileName("ds"));      // bundled 1.0.0

  const auto service = makeDefaultModeService();

  // Version-only comparison: an equal version is left byte-for-byte untouched
  // (shipping a change requires bumping the plugin version).
  EXPECT_TRUE(QFile::exists(seededDir() + "/" + pluginFileName("user_named")));
  EXPECT_FALSE(QFile::exists(seededDir() + "/" + pluginFileName("ds")));
}

TEST_F(ExtensionCatalogSeedTest, ReseedsWhenInstalledCopyIsUnreadable) {
  ASSERT_TRUE(QDir().mkpath(seededDir()));  // gutted: the id folder exists but holds no plugin
  placePlugin(bundled_.path(), PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, pluginFileName("ds"));

  const auto service = makeDefaultModeService();

  const LoadedDataSource* mock = findMock(*service);
  ASSERT_NE(mock, nullptr) << "a gutted install must be treated as absent and reseeded";
  EXPECT_EQ(mock->version, "1.0.0");
}

TEST_F(ExtensionCatalogSeedTest, OverrideModeSeedsMarketplaceDirWithoutCoreLock) {
  placePlugin(bundled_.path(), PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, pluginFileName("ds"));

  const auto service = makeOverrideModeService();

  // The seed targets the marketplace dir in every mode — never the override dir.
  EXPECT_TRUE(QDir(seededDir()).exists());
  EXPECT_FALSE(QDir(override_.path() + "/" + kMockId).exists());
  const LoadedDataSource* mock = findMock(*service);
  ASSERT_NE(mock, nullptr) << "bundled plugins load via the marketplace tier in --plugin-dir mode";
  // The core lock is scoped to default mode: the manager governs the override
  // dir here, where a same-id copy is user-owned and must stay uninstallable.
  EXPECT_FALSE(service->extensionManager().isBundled(QString::fromUtf8(kMockId)));
}

// What the seed wrote is reported to the marketplace in EVERY mode, unlike the
// bundled id -> version map, which stays scoped to default mode. The marketplace
// reads the managed dir, and the seed writes that dir whether or not
// --plugin-dir reorders the load hierarchy.
//
// This is the case no rule over (scanned, bundled) can satisfy: the core map is
// empty here, and the scan snapshot was taken before the refresh, so the seed's
// own report is the only thing that can name the version now on disk.
TEST_F(ExtensionCatalogSeedTest, OverrideModeReportsTheSeededVersionToTheMarketplace) {
  placePlugin(seededDir(), PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, pluginFileName("old_v1"));     // installed 1.0.0
  placePlugin(bundled_.path(), PJ_MOCK_DATA_SOURCE_V2_PLUGIN_PATH, pluginFileName("ds"));  // bundled 2.0.0

  const auto service = makeOverrideModeService();
  const QString id = QString::fromUtf8(kMockId);

  const LoadedDataSource* mock = findMock(*service);
  ASSERT_NE(mock, nullptr);
  ASSERT_EQ(mock->version, "2.0.0") << "precondition: the seed refreshed the managed copy";
  ASSERT_TRUE(service->extensionManager().bundledVersion(id).isEmpty())
      << "precondition: the core map is default-mode only, so there is nothing to infer from";

  EXPECT_EQ(service->extensionManager().installedVersion(id), "2.0.0");
}

TEST_F(ExtensionCatalogSeedTest, OverrideDirOutranksNewerMarketplaceCopy) {
  placePlugin(override_.path(), PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, pluginFileName("ds"));    // override 1.0.0
  placePlugin(bundled_.path(), PJ_MOCK_DATA_SOURCE_V2_PLUGIN_PATH, pluginFileName("ds"));  // seeds 2.0.0

  const auto service = makeOverrideModeService();

  const LoadedDataSource* mock = findMock(*service);
  ASSERT_NE(mock, nullptr);
  EXPECT_EQ(mock->version, "1.0.0")
      << "--plugin-dir is authoritative: its copy wins even against a newer marketplace version";
  int mock_count = 0;
  for (const LoadedDataSource& plugin : service->dataSources()) {
    mock_count += plugin.id == kMockId ? 1 : 0;
  }
  EXPECT_EQ(mock_count, 1);
}

// The service hop of static registration: a StaticPluginSet handed to the
// constructor must reach the catalog (registered before the scan) and shadow a
// same-id DSO installed in the marketplace dir — pinning the pass-through that
// the delegating overloads (empty set) cannot observe.
TEST_F(ExtensionCatalogSeedTest, ConstructorStaticPluginsRegisterAndShadowSameIdDso) {
  static const PJ_data_source_vtable_t vt =
      pj_mock::makeMockDataSourceVtable(R"({"id":"mock-data-source","name":"Static Mock","version":"0.1.0"})");
  placePlugin(marketplace_.path(), PJ_MOCK_DATA_SOURCE_V2_PLUGIN_PATH, pluginFileName("installed"));

  StaticPluginSet static_set;
  static_set.data_sources.push_back({&vt, nullptr});
  const ExtensionCatalogService service(
      ExtensionCatalogService::Paths{{}, marketplace_.path(), bundled_.path()}, DiagnosticSink{}, std::move(static_set),
      nullptr);

  const LoadedDataSource* mock = findMock(service);
  ASSERT_NE(mock, nullptr) << "the constructor set must reach the catalog";
  EXPECT_EQ(mock->version, "0.1.0") << "the static plugin must shadow the same-id marketplace DSO";
  int mock_count = 0;
  for (const LoadedDataSource& plugin : service.dataSources()) {
    mock_count += plugin.id == kMockId ? 1 : 0;
  }
  EXPECT_EQ(mock_count, 1);
}

// --plugin-dir mode: the CLI override reorders the LOAD scan hierarchy, but the
// marketplace UI must keep tracking the plugins it INSTALLED (the marketplace
// dir), not the override dir. A regression would swap the ExtensionManager's
// root to the override dir, so an extension installed via the marketplace
// disappears from installedExtensions() the moment --plugin-dir is used.
//
// The mocked v1 and v2 DSOs share the id "mock-data-source" but differ by
// version, giving us a two-value discriminator on the same key: place v1 under
// the marketplace dir (what the user "installed"), v2 under --plugin-dir (the
// dev override), then read back the version the marketplace reports installed.
// Marketplace-tracks-marketplace-dir ⇒ v1; regression to override-dir ⇒ v2.
TEST_F(ExtensionCatalogSeedTest, OverrideModeExtensionManagerTracksMarketplaceDirNotOverrideDir) {
  const QString installed_root = marketplace_.path() + "/" + kMockId;
  placePlugin(installed_root, PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, pluginFileName("ds"));  // v1 in marketplace
  const QString override_root = override_.path() + "/" + kMockId;
  placePlugin(override_root, PJ_MOCK_DATA_SOURCE_V2_PLUGIN_PATH, pluginFileName("ds"));  // v2 under --plugin-dir

  const auto service = makeOverrideModeService();

  const QMap<QString, InstalledExtension> installed = service->extensionManager().installedExtensions();
  const QString mock_id = QString::fromUtf8(kMockId);
  ASSERT_TRUE(installed.contains(mock_id))
      << "an extension installed under the marketplace dir must remain visible to the marketplace "
         "even when --plugin-dir is used";
  EXPECT_EQ(installed.value(mock_id).version, "1.0.0")
      << "installedExtensions() must reflect the marketplace-managed copy, not the --plugin-dir override";
}

// The seed is the one writer into the marketplace dir that does not go through
// ExtensionManager's own mutating API: it removes the shared seed-stage tree and
// renames payloads straight into the managed dir. A session that could not take the
// store's writer lease must therefore skip it, or "read-only" would be a claim the
// second instance breaks on every launch.
TEST_F(ExtensionCatalogSeedTest, SecondInstanceDoesNotReseedBundledPlugins) {
  placePlugin(bundled_.path(), PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, pluginFileName("ds"));

  // Stands in for the first PlotJuggler process: it owns the store for as long as
  // it lives, exactly like the ExtensionManager inside a running app session.
  DownloadManager holder_downloader;
  ExtensionManager holder(&holder_downloader, marketplace_.path(), marketplace_.path() + "/.pending");
  ASSERT_TRUE(holder.hasStoreWriteAccess()) << "the first instance must own the store for this test to mean anything";

  const auto service = makeDefaultModeService();

  EXPECT_FALSE(service->extensionManager().hasStoreWriteAccess()) << "the second instance must be read-only";
  EXPECT_FALSE(QDir(seededDir()).exists())
      << "a read-only session must not copy bundled payloads into the store another process is managing";
  EXPECT_FALSE(QDir(marketplace_.path() + ".seed_stage").exists())
      << "a read-only session must not create or clear the shared seed-stage tree";
}

// The seed stages into a SIBLING of the marketplace dir and promotes by rename, so
// the sibling has to be derived from the resolved store: beside a symlink it can sit
// on another filesystem, and then the rename is EXDEV rather than an atomic move.
// Same policy the writer lease and the install staging area follow.
TEST_F(ExtensionCatalogSeedTest, SeedStageFollowsTheCanonicalMarketplaceDir) {
#ifdef Q_OS_WIN
  GTEST_SKIP() << "symlink creation needs elevation on Windows; the canonical-path rule is platform-neutral";
#else
  QTemporaryDir root;
  ASSERT_TRUE(root.isValid());
  const QString real_store = QDir(root.path()).absoluteFilePath("real_marketplace");
  const QString link_store = QDir(root.path()).absoluteFilePath("link_marketplace");
  ASSERT_TRUE(QDir().mkpath(real_store));
  ASSERT_TRUE(QFile::link(real_store, link_store));
  placePlugin(bundled_.path(), PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, pluginFileName("ds"));

  // A stale stage beside the RESOLVED store: the seed clears its staging area
  // before it does anything else, so a run that resolves the path correctly
  // removes this, and one that works textually leaves it untouched.
  const QString canonical_stage = real_store + ".seed_stage";
  ASSERT_TRUE(QDir().mkpath(canonical_stage + "/leftover"));

  const ExtensionCatalogService service(
      ExtensionCatalogService::Paths{{}, link_store, bundled_.path()}, DiagnosticSink{}, nullptr);

  EXPECT_TRUE(QDir(real_store + "/" + kMockId).exists()) << "the seed must land in the resolved store";
  EXPECT_FALSE(QDir(canonical_stage).exists())
      << "the seed's staging sibling must be derived from the resolved store, so its own cleanup finds it";
  EXPECT_FALSE(QDir(link_store + ".seed_stage").exists())
      << "a staging area beside the symlink can resolve onto another filesystem, which breaks promote-by-rename";
#endif
}

// The seed still runs, unchanged, once the store is free again: the gate is about
// who owns the store, not a permanent opt-out.
TEST_F(ExtensionCatalogSeedTest, SeedRunsOnceTheStoreLeaseIsFree) {
  placePlugin(bundled_.path(), PJ_MOCK_DATA_SOURCE_PLUGIN_PATH, pluginFileName("ds"));

  {
    DownloadManager holder_downloader;
    ExtensionManager holder(&holder_downloader, marketplace_.path(), marketplace_.path() + "/.pending");
    ASSERT_TRUE(holder.hasStoreWriteAccess());
    const auto blocked = makeDefaultModeService();
    ASSERT_FALSE(QDir(seededDir()).exists());
  }

  const auto service = makeDefaultModeService();

  EXPECT_TRUE(service->extensionManager().hasStoreWriteAccess());
  EXPECT_TRUE(QDir(seededDir()).exists()) << "the seed must resume for the session that owns the store";
}

// -----------------------------------------------------------------------------
// Host-compatibility helpers used by the seed's rescue path
// -----------------------------------------------------------------------------

// The seed uses descriptorIsCompatibleWithHost() to decide whether an installed
// copy above the bundled version can survive: when the answer is false, the
// bundled build overwrites it (rescue). These tests pin the compat semantics
// the seed relies on — end-to-end coverage of the rescue itself needs a mock
// DSO that declares incompatible metadata, which requires an SDK-side test
// asset; the follow-up lands separately.

PluginDescriptor makeDescriptor(uint32_t abi_major, std::string min_sdk = {}) {
  PluginDescriptor d;
  d.id = "any-id";
  d.name = "Any";
  d.version = "1.0.0";
  d.abi_major = abi_major;
  d.min_sdk_required = std::move(min_sdk);
  return d;
}

TEST(ExtensionCatalogCompatTest, MatchingAbiAndEmptyMinIsCompatible) {
  const auto d = makeDescriptor(PJ_ABI_VERSION);
  EXPECT_TRUE(ExtensionCatalogService::descriptorIsCompatibleWithHost(d));
  EXPECT_TRUE(ExtensionCatalogService::descriptorIncompatReason(d).isEmpty());
}

TEST(ExtensionCatalogCompatTest, ZeroAbiIsAssumedCompatible) {
  // A manifest predating the abi_major field decodes as 0 — the seed defers to
  // the load path's own abi symbol check instead of falsely marking it incompat.
  const auto d = makeDescriptor(0);
  EXPECT_TRUE(ExtensionCatalogService::descriptorIsCompatibleWithHost(d));
}

TEST(ExtensionCatalogCompatTest, MismatchedAbiIsIncompatible) {
  const auto d = makeDescriptor(PJ_ABI_VERSION + 1);
  EXPECT_FALSE(ExtensionCatalogService::descriptorIsCompatibleWithHost(d));
  const QString reason = ExtensionCatalogService::descriptorIncompatReason(d);
  EXPECT_TRUE(reason.contains("ABI")) << reason.toStdString();
}

TEST(ExtensionCatalogCompatTest, DeprecatedApplicationFloorNeverGates) {
  // min_plotjuggler_version is decoded for back-compat but no longer gates:
  // abi_major is the generation gate and app-level feature gaps degrade.
  auto d = makeDescriptor(PJ_ABI_VERSION);
  d.min_plotjuggler_version = "999.0.0";
  EXPECT_TRUE(ExtensionCatalogService::descriptorIsCompatibleWithHost(d));
  d.min_plotjuggler_version = "not-a-version";
  EXPECT_TRUE(ExtensionCatalogService::descriptorIsCompatibleWithHost(d));
}

TEST(ExtensionCatalogCompatTest, NewerSdkRequirementIsIncompatible) {
  const auto d = makeDescriptor(PJ_ABI_VERSION, "99.0.0");
  EXPECT_FALSE(ExtensionCatalogService::descriptorIsCompatibleWithHost(d));
  const QString reason = ExtensionCatalogService::descriptorIncompatReason(d);
  EXPECT_TRUE(reason.contains("SDK", Qt::CaseInsensitive)) << reason.toStdString();
  EXPECT_TRUE(reason.contains("99.0.0")) << reason.toStdString();
}

TEST(ExtensionCatalogCompatTest, MalformedSdkFloorIsIncompatible) {
  const auto d = makeDescriptor(PJ_ABI_VERSION, "4.1");
  EXPECT_FALSE(ExtensionCatalogService::descriptorIsCompatibleWithHost(d));
  const QString reason = ExtensionCatalogService::descriptorIncompatReason(d);
  EXPECT_TRUE(reason.contains("invalid", Qt::CaseInsensitive)) << reason.toStdString();
  EXPECT_TRUE(reason.contains("4.1")) << reason.toStdString();
}

}  // namespace
}  // namespace PJ
