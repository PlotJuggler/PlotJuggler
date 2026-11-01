// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The "pj.feature_catalog.v1" document, composed from fakes: which list each
// extension lands in, why an installed one is not loaded, where the host sends
// the user when no override is declared, and what the registry state says
// about the fetches the app made on its own.

#include <gtest/gtest.h>

#include <QTemporaryDir>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

#include "hermetic_catalog.h"
#include "mock_data_source_vtable.h"
#include "mock_message_parser_vtable.h"
#include "mock_toolbox_vtable.h"
#include "pj_base/data_source_protocol.h"
#include "pj_marketplace/extension_manager.hpp"
#include "pj_marketplace/platform_utils.hpp"
#include "pj_runtime/FeatureCatalogHost.h"
#include "recording_test_utils.h"

using namespace Qt::StringLiterals;
using PJ::FeatureCatalogHost;
using Json = nlohmann::json;

namespace {

const QUrl kRegistryUrl(u"https://registry.example/registry.json"_s);

// ExtensionManager persists the disabled list in QSettings: keep it away from
// the real preferences and from parallel cases.
class FeatureCatalogHostTest : public ::testing::Test {
 protected:
  PJ::test::IsolatedQtSettings settings_{u"PlotJugglerTest"_s, u"feature_catalog_host_test"_s};
};

FeatureCatalogHost::Plugin plugin(
    PJ::PluginFamily family, const std::string& id, const std::string& category = {},
    const std::string& entry_point = {}) {
  FeatureCatalogHost::Plugin p;
  p.family = family;
  p.id = id;
  p.name = id + " name";
  p.version = "1.0.0";
  p.category = category;
  p.summary = id + " summary";
  p.entry_point = entry_point;
  return p;
}

PJ::Extension registryEntry(const QString& id, const QString& category = u"toolbox"_s) {
  PJ::Extension ext;
  ext.id = id;
  ext.name = id + u" name"_s;
  ext.version = u"2.0.0"_s;
  ext.category = category;
  ext.description = id + u" registry summary"_s;
  return ext;
}

// The fakes every case edits before composing.
struct Fixture {
  std::vector<FeatureCatalogHost::Plugin> loaded;
  std::vector<FeatureCatalogHost::Installed> installed;
  std::set<QString> not_installable;
  QUrl registry_url = kRegistryUrl;
  QByteArray core_json = R"({"schema":1,"core":[],"limitations":[]})";

  [[nodiscard]] std::unique_ptr<FeatureCatalogHost> make() {
    FeatureCatalogHost::Sources sources;
    sources.loaded = [this]() { return loaded; };
    sources.installed = [this]() { return installed; };
    sources.installable = [this](const PJ::Extension& ext) { return !not_installable.contains(ext.id); };
    sources.registry_url = [this]() { return registry_url; };
    return std::make_unique<FeatureCatalogHost>(std::move(sources), core_json, "4.1.0", "linux-x86_64");
  }
};

Json parse(const FeatureCatalogHost& host) {
  Json doc = Json::parse(host.document(), nullptr, /*allow_exceptions=*/false);
  EXPECT_TRUE(doc.is_object());
  return doc;
}

std::vector<std::string> ids(const Json& list) {
  std::vector<std::string> out;
  for (const auto& entry : list) {
    out.push_back(entry.at("id").get<std::string>());
  }
  return out;
}

const Json* findById(const Json& list, const std::string& id) {
  for (const auto& entry : list) {
    if (entry.at("id") == id) {
      return &entry;
    }
  }
  return nullptr;
}

}  // namespace

TEST_F(FeatureCatalogHostTest, DocumentIsSchemaOneWithEveryList) {
  Fixture fixture;
  auto host = fixture.make();
  const Json doc = parse(*host);

  EXPECT_EQ(doc.at("schema"), 1);
  EXPECT_EQ(doc.at("host_version"), "4.1.0");
  EXPECT_EQ(doc.at("platform"), "linux-x86_64");
  for (const char* list : {"core", "limitations", "loaded", "installed_not_loaded", "available"}) {
    ASSERT_TRUE(doc.contains(list)) << list;
    EXPECT_TRUE(doc.at(list).is_array()) << list;
  }
  EXPECT_EQ(doc.at("registry"), (Json{{"state", "not_fetched"}, {"reason", ""}}));
}

TEST_F(FeatureCatalogHostTest, MaintainerFieldsAreNotForwardedAndCoreIsSortedById) {
  Fixture fixture;
  fixture.core_json = R"({"schema":1,
    "core":[
      {"id":"zeta","name":"Zeta","summary":"Z.","entry_point":"File → Z","evidence":"a.cpp:1","verified":"",
       "maintainer_note":"x"},
      {"id":"alpha","name":"Alpha","summary":"A.","entry_point":"File → A","evidence":"b.cpp:2","verified":"yes"}],
    "limitations":[{"id":"no_publish","summary":"Cannot publish.","evidence":"c.cpp:3"}]})";
  auto host = fixture.make();
  const Json doc = parse(*host);

  EXPECT_EQ(ids(doc.at("core")), (std::vector<std::string>{"alpha", "zeta"}));
  EXPECT_EQ(
      doc.at("core").at(1), (Json{{"id", "zeta"}, {"name", "Zeta"}, {"summary", "Z."}, {"entry_point", "File → Z"}}));
  EXPECT_EQ(doc.at("limitations").at(0), (Json{{"id", "no_publish"}, {"summary", "Cannot publish."}}));
}

TEST_F(FeatureCatalogHostTest, EachIdLandsInExactlyOneListLoadedFirst) {
  using Family = PJ::PluginFamily;
  Fixture fixture;
  fixture.loaded = {plugin(Family::kToolbox, "both-loaded-and-installed", "toolbox")};
  fixture.installed = {
      {plugin(Family::kToolbox, "both-loaded-and-installed", "toolbox"), FeatureCatalogHost::Reason::kDisabled},
      {plugin(Family::kToolbox, "installed-only", "toolbox"), FeatureCatalogHost::Reason::kDisabled},
  };
  auto host = fixture.make();
  host->registryFetchFinished(
      kRegistryUrl, true,
      {registryEntry(u"both-loaded-and-installed"_s), registryEntry(u"installed-only"_s),
       registryEntry(u"registry-only"_s)});
  const Json doc = parse(*host);

  EXPECT_EQ(ids(doc.at("loaded")), (std::vector<std::string>{"both-loaded-and-installed"}));
  EXPECT_EQ(ids(doc.at("installed_not_loaded")), (std::vector<std::string>{"installed-only"}));
  EXPECT_EQ(ids(doc.at("available")), (std::vector<std::string>{"registry-only"}));

  // A loaded plugin describes itself from the LOADED manifest, never the registry.
  const Json& loaded = doc.at("loaded").at(0);
  EXPECT_EQ(loaded.at("version"), "1.0.0");
  EXPECT_EQ(loaded.at("summary"), "both-loaded-and-installed summary");
  EXPECT_EQ(loaded.at("in_registry"), true);
  EXPECT_EQ(doc.at("available").at(0).at("summary"), "registry-only registry summary");
}

TEST_F(FeatureCatalogHostTest, InstalledReasonsAreReportedAndUnknownIsOmitted) {
  using Family = PJ::PluginFamily;
  using Reason = FeatureCatalogHost::Reason;
  Fixture fixture;
  fixture.installed = {
      {plugin(Family::kToolbox, "a-disabled", "toolbox"), Reason::kDisabled},
      {plugin(Family::kToolbox, "b-restart", "toolbox"), Reason::kNeedsRestart},
      {plugin(Family::kToolbox, "c-incompatible", "toolbox"), Reason::kIncompatible},
      {plugin(Family::kToolbox, "d-unknown", "toolbox"), Reason::kUnknown},
  };
  auto host = fixture.make();
  const Json list = parse(*host).at("installed_not_loaded");

  ASSERT_EQ(list.size(), 4U);
  EXPECT_EQ(list.at(0).at("reason"), "disabled");
  EXPECT_EQ(list.at(1).at("reason"), "needs_restart");
  EXPECT_EQ(list.at(2).at("reason"), "incompatible");
  EXPECT_FALSE(list.at(3).contains("reason"));  // missing = unknown, never a guess
}

// The real gate: a registry entry is installable only when the host can install
// it, platform artifact AND SDK floor. compatibleExtensions() would admit the
// SDK-floor case, since it checks the platform alone; hostCompatibility() would
// admit an entry with no platforms map, which the Marketplace list hides.
TEST_F(FeatureCatalogHostTest, RegistryCandidatesPassTheHostInstallGate) {
  QTemporaryDir root;
  ASSERT_TRUE(root.isValid());
  PJ::ExtensionManager manager(nullptr, root.filePath(u"extensions"_s), root.filePath(u"pending"_s));

  const QString platform = PJ::PlatformUtils::currentPlatform();
  PJ::Extension ok = registryEntry(u"ok"_s);
  ok.platforms.insert(platform, {});
  PJ::Extension other_platform = registryEntry(u"other-platform"_s);
  other_platform.platforms.insert(u"no-such-platform"_s, {});
  PJ::Extension sdk_too_new = registryEntry(u"sdk-too-new"_s);
  sdk_too_new.platforms.insert(platform, {});
  sdk_too_new.min_sdk_required = u"999.0.0"_s;
  const PJ::Extension no_platforms = registryEntry(u"no-platforms"_s);
  ASSERT_TRUE(manager.hostCompatibility(no_platforms).ok);  // the gap the adapter closes

  FeatureCatalogHost::Sources sources;
  sources.loaded = []() { return std::vector<FeatureCatalogHost::Plugin>{}; };
  sources.installed = []() { return std::vector<FeatureCatalogHost::Installed>{}; };
  sources.installable = [&manager](const PJ::Extension& ext) { return FeatureCatalogHost::installable(manager, ext); };
  sources.registry_url = []() { return kRegistryUrl; };
  FeatureCatalogHost host(std::move(sources), {}, "4.1.0", platform.toStdString());
  host.registryFetchFinished(kRegistryUrl, true, {ok, other_platform, sdk_too_new, no_platforms});

  EXPECT_EQ(ids(parse(host).at("available")), (std::vector<std::string>{"ok"}));
}

TEST_F(FeatureCatalogHostTest, InstalledRecordsCarryTheLoadGateReasons) {
  QTemporaryDir root;
  ASSERT_TRUE(root.isValid());
  PJ::ExtensionManager manager(nullptr, root.filePath(u"extensions"_s), root.filePath(u"pending"_s));
  const auto record = [](const QString& id) {
    PJ::InstalledExtension installed;
    installed.id = id;
    installed.name = id;
    installed.version = u"1.0.0"_s;
    installed.category = u"toolbox"_s;
    installed.path = id;
    return installed;
  };
  PJ::InstalledExtension incompatible = record(u"incompatible"_s);
  incompatible.min_sdk_required = u"999.0.0"_s;
  QMap<QString, PJ::InstalledExtension> records;
  records.insert(u"plain"_s, record(u"plain"_s));
  records.insert(u"disabled"_s, record(u"disabled"_s));
  records.insert(u"incompatible"_s, incompatible);
  manager.setInstalledExtensions(records);
  manager.setEnabled(u"disabled"_s, false);
  manager.setEnabled(u"incompatible"_s, false);  // incompatible outranks disabled

  std::map<std::string, FeatureCatalogHost::Reason> reasons;
  for (const auto& entry : FeatureCatalogHost::installedRecords(manager)) {
    reasons[entry.plugin.id] = entry.reason;
  }
  EXPECT_EQ(reasons.at("plain"), FeatureCatalogHost::Reason::kUnknown);
  EXPECT_EQ(reasons.at("disabled"), FeatureCatalogHost::Reason::kDisabled);
  EXPECT_EQ(reasons.at("incompatible"), FeatureCatalogHost::Reason::kIncompatible);
}

// An installed record does not know where its extension would appear, and a
// missing field means unknown. The registry's entry for the same id does not
// fill it in either; the other fields are unaffected.
TEST_F(FeatureCatalogHostTest, InstalledNotLoadedHasNoEntryPoint) {
  Fixture fixture;
  fixture.installed = {
      {plugin(PJ::PluginFamily::kToolbox, "disabled", "toolbox"), FeatureCatalogHost::Reason::kDisabled}};
  auto host = fixture.make();
  PJ::Extension in_registry = registryEntry(u"disabled"_s);
  in_registry.entry_point = u"Toolbox menu → Something"_s;
  host->registryFetchFinished(kRegistryUrl, true, {in_registry});

  const Json list = parse(*host).at("installed_not_loaded");
  ASSERT_EQ(list.size(), 1U);
  EXPECT_FALSE(list.at(0).contains("entry_point"));
  EXPECT_EQ(list.at(0).at("reason"), "disabled");
  EXPECT_EQ(list.at(0).at("summary"), "disabled summary");
}

TEST_F(FeatureCatalogHostTest, DeclaredEntryPointWinsOverTheDefault) {
  using Family = PJ::PluginFamily;
  Fixture fixture;
  fixture.loaded = {
      plugin(Family::kToolbox, "editor", "toolbox", "Custom Series panel → + (Add custom series)"),
      plugin(Family::kToolbox, "blank-override", "toolbox", "  "),
  };
  fixture.loaded[0].tags = {"hidden"};
  auto host = fixture.make();
  PJ::Extension available = registryEntry(u"available"_s);
  available.entry_point = u"Toolbox menu → Something else"_s;
  host->registryFetchFinished(kRegistryUrl, true, {available});
  const Json doc = parse(*host);

  EXPECT_EQ(findById(doc.at("loaded"), "editor")->at("entry_point"), "Custom Series panel → + (Add custom series)");
  EXPECT_EQ(findById(doc.at("loaded"), "blank-override")->at("entry_point"), "Toolbox menu → blank-override name");
  EXPECT_EQ(findById(doc.at("available"), "available")->at("entry_point"), "Toolbox menu → Something else");
}

TEST_F(FeatureCatalogHostTest, DefaultEntryPointsRouteByCapabilityThenCategory) {
  using Family = PJ::PluginFamily;
  const std::string file = "Sources panel → File (or File → Open Data File...)";
  const std::string stream = "Sources panel → Stream → src name → + (Start streaming)";

  auto source = plugin(Family::kDataSource, "src", "data_stream");
  source.source_capabilities = PJ_DATA_SOURCE_CAPABILITY_FINITE_IMPORT;
  EXPECT_EQ(FeatureCatalogHost::defaultEntryPoint(source), file);  // the bits win over the category
  source.source_capabilities = PJ_DATA_SOURCE_CAPABILITY_CONTINUOUS_STREAM;
  EXPECT_EQ(FeatureCatalogHost::defaultEntryPoint(source), stream);
  source.source_capabilities = PJ_DATA_SOURCE_CAPABILITY_FINITE_IMPORT | PJ_DATA_SOURCE_CAPABILITY_CONTINUOUS_STREAM;
  EXPECT_EQ(FeatureCatalogHost::defaultEntryPoint(source), file + "; " + stream);

  // Registry/installed entries have no runtime bits: the category decides.
  EXPECT_EQ(FeatureCatalogHost::defaultEntryPoint(plugin(Family::kDataSource, "src", "data_loader")), file);
  EXPECT_EQ(FeatureCatalogHost::defaultEntryPoint(plugin(Family::kDataSource, "src", "data_stream")), stream);

  auto toolbox = plugin(Family::kToolbox, "tb", "toolbox");
  EXPECT_EQ(FeatureCatalogHost::defaultEntryPoint(toolbox), "Toolbox menu → tb name");
  toolbox.tags = {"hidden"};
  EXPECT_EQ(FeatureCatalogHost::defaultEntryPoint(toolbox), "");
  toolbox.tags = {"hidden", "cloud"};
  EXPECT_EQ(FeatureCatalogHost::defaultEntryPoint(toolbox), "Sources panel → Cloud → tb name → + (Open cloud browser)");

  EXPECT_EQ(
      FeatureCatalogHost::defaultEntryPoint(plugin(Family::kMessageParser, "p", "message_parser")),
      "No panel of its own: chosen as the message format inside the data source that uses it (for example a "
      "stream's message serialization selector).");
  EXPECT_EQ(FeatureCatalogHost::defaultEntryPoint(plugin(Family::kUnknown, "x", "bundle")), "");
}

const Json kPending = {{"state", "pending"}, {"reason", ""}};
const Json kUnreachable = {{"state", "unreachable"}, {"reason", ""}};
const Json kFetched = {{"state", "fetched"}, {"reason", ""}};

TEST_F(FeatureCatalogHostTest, RegistryStateFollowsTheAppsOwnFetches) {
  const auto registry = [](FeatureCatalogHost& host) { return parse(host).at("registry"); };
  const auto notFetched = [](const char* reason) { return Json{{"state", "not_fetched"}, {"reason", reason}}; };

  {
    Fixture fixture;
    auto host = fixture.make();
    host->setStartupScanSkipped();
    EXPECT_EQ(registry(*host), notFetched("updates_disabled"));
    // A Marketplace fetch the user made still counts.
    host->registryFetchFinished(kRegistryUrl, true, {registryEntry(u"x"_s)});
    EXPECT_EQ(registry(*host), kFetched);
  }
  {
    Fixture fixture;
    auto host = fixture.make();
    host->registryFetchStarted(kRegistryUrl);
    EXPECT_EQ(registry(*host), kPending);
    host->registryFetchFinished(kRegistryUrl, false, {});
    EXPECT_EQ(registry(*host), kUnreachable);
    EXPECT_TRUE(parse(*host).at("available").empty());
  }
  {
    Fixture fixture;
    fixture.loaded = {plugin(PJ::PluginFamily::kToolbox, "x", "toolbox")};
    auto host = fixture.make();
    host->registryFetchStarted(kRegistryUrl);
    host->registryFetchFinished(kRegistryUrl, true, {registryEntry(u"x"_s), registryEntry(u"y"_s)});
    Json doc = parse(*host);
    EXPECT_EQ(doc.at("registry"), kFetched);
    EXPECT_EQ(ids(doc.at("available")), (std::vector<std::string>{"y"}));

    // Preferences pointed the app at another registry: the fetched list is
    // not that registry's, and the host does not fetch it on its own.
    fixture.registry_url = QUrl(u"https://other.example/registry.json"_s);
    doc = parse(*host);
    EXPECT_EQ(doc.at("registry"), notFetched("url_changed"));
    EXPECT_TRUE(doc.at("available").empty());
    EXPECT_EQ(doc.at("loaded").at(0).at("in_registry"), false);
  }
}

// Each URL keeps its own last list: the fetch of an old URL finishing late
// cannot replace what was fetched from the current one.
TEST_F(FeatureCatalogHostTest, LateSuccessForAnOldUrlKeepsTheCurrentUrlsList) {
  const QUrl url_b(u"https://other.example/registry.json"_s);
  Fixture fixture;
  auto host = fixture.make();
  host->registryFetchStarted(kRegistryUrl);
  fixture.registry_url = url_b;
  host->registryFetchFinished(url_b, true, {registryEntry(u"from-b"_s)});
  host->registryFetchFinished(kRegistryUrl, true, {registryEntry(u"from-a"_s)});

  Json doc = parse(*host);
  EXPECT_EQ(doc.at("registry"), kFetched);
  EXPECT_EQ(ids(doc.at("available")), (std::vector<std::string>{"from-b"}));

  // Back on A, its own outcome is reported.
  fixture.registry_url = kRegistryUrl;
  EXPECT_EQ(ids(parse(*host).at("available")), (std::vector<std::string>{"from-a"}));
}

TEST_F(FeatureCatalogHostTest, LateFailureForAnOldUrlKeepsTheCurrentUrlsList) {
  const QUrl url_b(u"https://other.example/registry.json"_s);
  Fixture fixture;
  auto host = fixture.make();
  host->registryFetchStarted(kRegistryUrl);
  fixture.registry_url = url_b;
  host->registryFetchFinished(url_b, true, {registryEntry(u"from-b"_s)});
  host->registryFetchFinished(kRegistryUrl, false, {});

  Json doc = parse(*host);
  EXPECT_EQ(doc.at("registry"), kFetched);
  EXPECT_EQ(ids(doc.at("available")), (std::vector<std::string>{"from-b"}));

  fixture.registry_url = kRegistryUrl;
  EXPECT_EQ(parse(*host).at("registry"), kUnreachable);
}

// Two fetches of one URL can overlap (the startup scan and the Marketplace have
// a manager each): whichever finishes last decides, in both directions.
TEST_F(FeatureCatalogHostTest, TheLastFinishedFetchOfAUrlWins) {
  Fixture fixture;
  auto host = fixture.make();
  host->registryFetchFinished(kRegistryUrl, true, {registryEntry(u"x"_s)});
  EXPECT_EQ(parse(*host).at("registry"), kFetched);
  host->registryFetchFinished(kRegistryUrl, false, {});
  EXPECT_EQ(parse(*host).at("registry"), kUnreachable);
  EXPECT_TRUE(parse(*host).at("available").empty());
  host->registryFetchFinished(kRegistryUrl, true, {registryEntry(u"y"_s)});
  EXPECT_EQ(parse(*host).at("registry"), kFetched);
  EXPECT_EQ(ids(parse(*host).at("available")), (std::vector<std::string>{"y"}));
}

TEST_F(FeatureCatalogHostTest, ARefetchInFlightDoesNotHideTheListInHand) {
  Fixture fixture;
  auto host = fixture.make();
  host->registryFetchFinished(kRegistryUrl, true, {registryEntry(u"x"_s)});
  host->registryFetchStarted(kRegistryUrl);
  EXPECT_EQ(parse(*host).at("registry"), kFetched);
  EXPECT_EQ(ids(parse(*host).at("available")), (std::vector<std::string>{"x"}));
}

// With no list in hand a refetch after a failure is pending again, and only a
// fetch of the pending URL ends that.
TEST_F(FeatureCatalogHostTest, OnlyTheSameUrlEndsThePendingState) {
  Fixture fixture;
  auto host = fixture.make();
  host->registryFetchFinished(kRegistryUrl, false, {});
  host->registryFetchStarted(kRegistryUrl);
  EXPECT_EQ(parse(*host).at("registry"), kPending);
  host->registryFetchFinished(QUrl(u"https://other.example/registry.json"_s), true, {registryEntry(u"x"_s)});
  EXPECT_EQ(parse(*host).at("registry"), kPending);
  host->registryFetchFinished(kRegistryUrl, false, {});
  EXPECT_EQ(parse(*host).at("registry"), kUnreachable);
}

TEST_F(FeatureCatalogHostTest, FailureOfAnotherUrlLeavesTheCurrentOneAlone) {
  Fixture fixture;
  auto host = fixture.make();
  host->registryFetchFinished(kRegistryUrl, true, {registryEntry(u"x"_s)});
  host->registryFetchFinished(QUrl(u"https://other.example/registry.json"_s), false, {});
  EXPECT_EQ(parse(*host).at("registry"), kFetched);
}

// An id past the bound is dropped, never cut: two ids sharing a 128-character
// prefix would otherwise land in the document as the same id.
TEST_F(FeatureCatalogHostTest, IdsPastTheBoundAreSkippedNotTruncated) {
  using Family = PJ::PluginFamily;
  const std::string prefix(128, 'a');
  Fixture fixture;
  fixture.loaded = {plugin(Family::kToolbox, prefix + "1", "toolbox"), plugin(Family::kToolbox, prefix, "toolbox")};
  fixture.installed = {{plugin(Family::kToolbox, prefix + "2", "toolbox"), FeatureCatalogHost::Reason::kDisabled}};
  auto host = fixture.make();
  host->registryFetchFinished(
      kRegistryUrl, true, {registryEntry(QString::fromStdString(prefix + "3")), registryEntry(u"fits"_s)});
  const Json doc = parse(*host);

  EXPECT_EQ(ids(doc.at("loaded")), (std::vector<std::string>{prefix}));  // exactly at the bound fits
  EXPECT_TRUE(doc.at("installed_not_loaded").empty());
  EXPECT_EQ(ids(doc.at("available")), (std::vector<std::string>{"fits"}));
}

TEST_F(FeatureCatalogHostTest, StringsAreBoundedAndTheDocumentIsDeterministic) {
  using Family = PJ::PluginFamily;
  Fixture fixture;
  auto verbose = plugin(Family::kToolbox, "b-verbose", "toolbox");
  verbose.summary.clear();
  for (int i = 0; i < 400; ++i) {
    verbose.summary += "é→";  // multi-byte code points, so a byte cut would split one
  }
  verbose.name = std::string(500, 'n');
  fixture.loaded = {verbose, plugin(Family::kToolbox, "a-first", "toolbox")};
  auto host = fixture.make();

  const std::string first = host->document();
  EXPECT_EQ(first, host->document());
  const Json doc = Json::parse(first);  // throws on invalid UTF-8
  EXPECT_EQ(ids(doc.at("loaded")), (std::vector<std::string>{"a-first", "b-verbose"}));

  const QString summary = QString::fromStdString(doc.at("loaded").at(1).at("summary").get<std::string>());
  EXPECT_EQ(summary.size(), 600);  // all BMP code points: one UTF-16 unit each
  EXPECT_TRUE(summary.endsWith(u'…'));
  EXPECT_EQ(doc.at("loaded").at(1).at("name").get<std::string>().size(), 127U + std::string("…").size());
}

// What loadedPlugins() reads from a real catalog: family from the collection,
// capability bits from the probe, the rest from each plugin's own manifest.
TEST_F(FeatureCatalogHostTest, LoadedPluginsCarryWhatTheCatalogAndManifestsSay) {
  static const PJ_data_source_vtable_t kSource = []() {
    auto vtable = pj_mock::makeMockDataSourceVtable(
        R"({"id":"src","name":"Source","version":"1.2.3","category":"data_stream","description":"Streams.",
            "entry_point":"Somewhere","tags":["a","cloud"]})");
    vtable.capabilities = [](void*) noexcept -> uint64_t { return PJ_DATA_SOURCE_CAPABILITY_CONTINUOUS_STREAM; };
    return vtable;
  }();
  static const PJ_toolbox_vtable_t kToolbox = pj_mock::makeMockToolboxVtable(
      R"({"id":"tb","name":"Toolbox","version":"2.0.0","category":"toolbox","tags":["hidden"]})");
  static const PJ_message_parser_vtable_t kParser = pj_mock::makeMockMessageParserVtable(
      R"({"id":"mp","name":"Parser","version":"3.0.0","category":"message_parser","encoding":["json"]})");
  PJ::StaticPluginSet static_plugins;
  static_plugins.data_sources.emplace_back(&kSource);
  static_plugins.toolboxes.emplace_back(&kToolbox);
  static_plugins.message_parsers.emplace_back(&kParser);
  PJ::test::HermeticCatalog catalog(std::move(static_plugins));

  std::map<std::string, FeatureCatalogHost::Plugin> loaded;
  for (auto& entry : FeatureCatalogHost::loadedPlugins(catalog.service)) {
    loaded.emplace(entry.id, std::move(entry));
  }
  ASSERT_EQ(loaded.size(), 3U);

  const FeatureCatalogHost::Plugin& source = loaded.at("src");
  EXPECT_EQ(source.family, PJ::PluginFamily::kDataSource);
  EXPECT_EQ(source.name, "Source");
  EXPECT_EQ(source.version, "1.2.3");
  EXPECT_EQ(source.category, "data_stream");
  EXPECT_EQ(source.summary, "Streams.");
  EXPECT_EQ(source.entry_point, "Somewhere");
  EXPECT_EQ(source.tags, (std::vector<std::string>{"a", "cloud"}));
  EXPECT_EQ(source.source_capabilities, uint64_t{PJ_DATA_SOURCE_CAPABILITY_CONTINUOUS_STREAM});

  EXPECT_EQ(loaded.at("tb").family, PJ::PluginFamily::kToolbox);
  EXPECT_EQ(loaded.at("tb").tags, (std::vector<std::string>{"hidden"}));
  EXPECT_FALSE(loaded.at("tb").source_capabilities.has_value());
  EXPECT_EQ(loaded.at("mp").family, PJ::PluginFamily::kMessageParser);
  EXPECT_EQ(loaded.at("mp").version, "3.0.0");
}

TEST_F(FeatureCatalogHostTest, ServiceSlotReturnsTheDocument) {
  Fixture fixture;
  auto host = fixture.make();
  const PJ_feature_catalog_t service = host->view();
  ASSERT_NE(service.vtable, nullptr);
  PJ_string_view_t json{};
  PJ_error_t error{};
  ASSERT_TRUE(service.vtable->get_feature_catalog(service.ctx, &json, &error));
  EXPECT_EQ(std::string(json.data, json.size), host->document());
}
