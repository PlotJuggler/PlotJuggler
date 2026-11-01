// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Widget-family admission in PluginRuntimeCatalog: loading, the "never unload" rule across reload(), the
// Qt build check, and static registration. The fixture libraries are Qt-free (see runtime_widget_plugin.cpp).

#include <gtest/gtest.h>

#include <QLibraryInfo>
#include <QVersionNumber>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "hermetic_catalog.h"
#include "mock_widget_vtable.h"
#include "pj_plugins/widget_types.hpp"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/PluginRuntimeCatalog.h"
#include "plugin_test_utils.h"

namespace PJ {
namespace {

using test::pluginFileName;

// Static vtables for registerStaticWidget: they claim this host's own Qt unless a test says otherwise.
const PJ_widget_build_info_t* hostBuildInfo() noexcept {
  static const PJ_widget_build_info_t info = [] {
    const QVersionNumber qt = QLibraryInfo::version();
    return pj_mock::makeBuildInfo(
        {static_cast<uint32_t>(qt.majorVersion()), static_cast<uint32_t>(qt.minorVersion()),
         PJ::widgetCompilerFamily()});
  }();
  return &info;
}

const PJ_widget_vtable_t* staticWidgetVtable() {
  static const PJ_widget_vtable_t vt = pj_mock::makeMockWidgetVtable(
      R"({"id":"static-widgets","name":"Static Widgets","version":"1.0.0","widgets":[{"type":"gauge","name":"Gauge"}]})",
      &hostBuildInfo);
  return &vt;
}

const PJ_widget_vtable_t* staticWidgetDuplicateVtable() {
  static const PJ_widget_vtable_t vt = pj_mock::makeMockWidgetVtable(
      R"({"id":"static-widgets","name":"Static Widgets Again","version":"2.0.0","widgets":[{"type":"table","name":"Table"}]})",
      &hostBuildInfo);
  return &vt;
}

const PJ_widget_vtable_t* staticWidgetSharingDsoIdVtable() {
  static const PJ_widget_vtable_t vt = pj_mock::makeMockWidgetVtable(
      R"({"id":"test-widgets","name":"Same Id As The DSO","version":"9.0.0","widgets":[{"type":"gauge","name":"Gauge"}]})",
      &hostBuildInfo);
  return &vt;
}

class WidgetCatalogTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("pj_widget_catalog_test_" +
            std::to_string(static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())));
    std::filesystem::create_directories(dir_);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::filesystem::path install(const char* fixture_path, const char* stem) const {
    const std::filesystem::path target = dir_ / pluginFileName(stem);
    std::filesystem::copy_file(fixture_path, target, std::filesystem::copy_options::overwrite_existing);
    return target;
  }

  [[nodiscard]] bool hasDiagnostic(DiagnosticLevel level, const std::string& needle) const {
    return std::ranges::any_of(diagnostics_, [&](const Diagnostic& d) {
      return d.level == level && d.message.find(needle) != std::string::npos;
    });
  }

  [[nodiscard]] int countDiagnostics(const std::string& needle) const {
    return static_cast<int>(std::ranges::count_if(
        diagnostics_, [&](const Diagnostic& d) { return d.message.find(needle) != std::string::npos; }));
  }

  std::filesystem::path dir_;
  std::vector<Diagnostic> diagnostics_;
};

TEST_F(WidgetCatalogTest, LoadExposesTypesSettingsAndModules) {
  install(PJ_RUNTIME_WIDGET_PLUGIN_PATH, "w");
  PluginRuntimeCatalog catalog(dir_, [this](const Diagnostic& d) { diagnostics_.push_back(d); });
  catalog.scanDirectory();

  ASSERT_EQ(catalog.widgets().size(), 1U);
  const RuntimeWidgetPlugin& plugin = catalog.widgets()[0];
  EXPECT_EQ(plugin.id, "test-widgets");
  EXPECT_EQ(plugin.name, "Test Widgets");
  EXPECT_EQ(plugin.version, "1.2.3");
  EXPECT_TRUE(plugin.qt_compatible);
  EXPECT_TRUE(plugin.qt_mismatch_reason.empty());
  ASSERT_EQ(plugin.widgets.size(), 2U);
  EXPECT_EQ(plugin.widgets[0].type, "gauge");
  EXPECT_EQ(plugin.widgets[0].name, "Gauge");
  EXPECT_EQ(plugin.widgets[0].icon, "<svg/>");
  EXPECT_NE(plugin.widgets[0].settings_json.find("\"min\""), std::string::npos);
  EXPECT_EQ(plugin.widgets[1].type, "table");
  EXPECT_EQ(plugin.widgets[1].settings_json, "[]");
  EXPECT_TRUE(plugin.library.valid());
  EXPECT_TRUE(plugin.library.createHandle("gauge"));
  // The manifest declares Qt3DCore, which PlotJuggler does not ship: a warning, never a refusal.
  EXPECT_TRUE(hasDiagnostic(DiagnosticLevel::kWarning, "Qt3DCore"));
  EXPECT_TRUE(hasDiagnostic(DiagnosticLevel::kInfo, "Loaded Widget Test Widgets"));
  EXPECT_FALSE(hasDiagnostic(DiagnosticLevel::kError, ""));
}

TEST_F(WidgetCatalogTest, ExtensionCatalogServiceListsAndFindsWidgetsById) {
  StaticPluginSet set;
  set.widgets.emplace_back(staticWidgetVtable());
  PJ::test::HermeticCatalog hermetic(std::move(set));
  ExtensionCatalogService& service = hermetic.service;

  ASSERT_EQ(service.widgets().size(), 1U);
  const LoadedWidget* found = service.findWidget(u"static-widgets");
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found, &service.widgets()[0]);
  EXPECT_EQ(found->widgets.at(0).type, "gauge");
  EXPECT_EQ(service.findWidget(u"nope"), nullptr);
  // A Widget id is not a Toolbox id.
  EXPECT_EQ(service.findToolbox(u"static-widgets"), nullptr);
}

TEST_F(WidgetCatalogTest, RescanKeepsTheLoadedLibrary) {
  install(PJ_RUNTIME_WIDGET_PLUGIN_PATH, "w");
  PluginRuntimeCatalog catalog(dir_, [this](const Diagnostic& d) { diagnostics_.push_back(d); });
  catalog.scanDirectory();
  const std::string path = catalog.widgets().at(0).path;
  catalog.scanDirectory();
  ASSERT_EQ(catalog.widgets().size(), 1U);
  EXPECT_EQ(catalog.widgets()[0].path, path);
  EXPECT_EQ(countDiagnostics("Loaded Widget"), 1);
}

TEST_F(WidgetCatalogTest, ReloadNeverDropsAWidgetWhoseFileDisappeared) {
  const auto file = install(PJ_RUNTIME_WIDGET_PLUGIN_PATH, "w");
  PluginRuntimeCatalog catalog(dir_, [this](const Diagnostic& d) { diagnostics_.push_back(d); });
  catalog.scanDirectory();
  ASSERT_EQ(catalog.widgets().size(), 1U);

  // Renamed, not removed: Windows refuses to delete a loaded DLL, and the scanner only looks at
  // the platform's library suffix, so the renamed file is as good as gone.
  std::filesystem::rename(file, file.string() + ".gone");
  EXPECT_FALSE(catalog.reload());
  ASSERT_EQ(catalog.widgets().size(), 1U);
  EXPECT_TRUE(catalog.widgets()[0].library.valid());
  EXPECT_EQ(countDiagnostics("Unloaded"), 0);
}

TEST_F(WidgetCatalogTest, ReloadNeverEvictsAWidgetWhoseFileChanged) {
  const auto file = install(PJ_RUNTIME_WIDGET_PLUGIN_PATH, "w");
  PluginRuntimeCatalog catalog(dir_, [this](const Diagnostic& d) { diagnostics_.push_back(d); });
  catalog.scanDirectory();
  ASSERT_EQ(catalog.widgets().size(), 1U);
  const auto loaded_mtime = catalog.widgets()[0].loaded_mtime;

  std::filesystem::last_write_time(file, loaded_mtime + std::chrono::seconds(30));
  EXPECT_FALSE(catalog.reload());
  ASSERT_EQ(catalog.widgets().size(), 1U);
  EXPECT_EQ(catalog.widgets()[0].loaded_mtime, loaded_mtime);
  EXPECT_TRUE(hasDiagnostic(DiagnosticLevel::kInfo, "test-widgets: updated on disk; restart required"));
  EXPECT_EQ(countDiagnostics("Loaded Widget"), 1);

  // The same change is announced once, however many times the catalog is reconciled.
  EXPECT_FALSE(catalog.reload());
  EXPECT_EQ(countDiagnostics("updated on disk; restart required"), 1);
}

TEST_F(WidgetCatalogTest, ReloadWithdrawsAWidgetDisabledAfterLoadButKeepsItsLibraryMapped) {
  install(PJ_RUNTIME_WIDGET_PLUGIN_PATH, "w");
  PluginRuntimeCatalog catalog(dir_, [this](const Diagnostic& d) { diagnostics_.push_back(d); });
  catalog.scanDirectory();
  ASSERT_EQ(catalog.widgets().size(), 1U);
  auto live = catalog.widgets()[0].library.createHandle("gauge");  // a panel already running it
  ASSERT_TRUE(live);

  catalog.setDisabledIds({"test-widgets"});
  EXPECT_TRUE(catalog.reload());
  EXPECT_TRUE(catalog.widgets().empty()) << "no new panel may use a disabled widget";
  EXPECT_TRUE(hasDiagnostic(DiagnosticLevel::kInfo, "test-widgets: disabled by user"));
  EXPECT_TRUE(live->valid());
  EXPECT_FALSE(live->manifest().empty()) << "the running panel still reaches the library";

  catalog.setDisabledIds({});
  EXPECT_TRUE(catalog.reload());
  ASSERT_EQ(catalog.widgets().size(), 1U) << "enabled again, the next reload brings it back";
}

TEST_F(WidgetCatalogTest, ReloadStillLoadsANewWidgetLibrary) {
  install(PJ_RUNTIME_WIDGET_PLUGIN_PATH, "w");
  PluginRuntimeCatalog catalog(dir_, [this](const Diagnostic& d) { diagnostics_.push_back(d); });
  catalog.scanDirectory();
  ASSERT_EQ(catalog.widgets().size(), 1U);

  install(PJ_RUNTIME_WIDGET_OTHER_COMPILER_PLUGIN_PATH, "w2");
  EXPECT_TRUE(catalog.reload());
  EXPECT_EQ(catalog.widgets().size(), 2U);
}

TEST_F(WidgetCatalogTest, ASecondCopyOfALoadedIdWaitsForARestart) {
  install(PJ_RUNTIME_WIDGET_PLUGIN_PATH, "w");
  PluginRuntimeCatalog catalog(dir_, [this](const Diagnostic& d) { diagnostics_.push_back(d); });
  catalog.scanDirectory();

  // Same manifest id in another folder entry that wins the scan once the first file is gone.
  const auto first = dir_ / pluginFileName("w");
  std::filesystem::rename(first, first.string() + ".gone");  // see ReloadNeverDropsAWidgetWhoseFileDisappeared
  const auto moved = dir_ / "sub";
  std::filesystem::create_directories(moved);
  std::filesystem::copy_file(PJ_RUNTIME_WIDGET_PLUGIN_PATH, moved / pluginFileName("w"));
  catalog.setPluginDirs({{dir_}, {moved}});
  EXPECT_FALSE(catalog.reload());
  ASSERT_EQ(catalog.widgets().size(), 1U);
  EXPECT_TRUE(hasDiagnostic(DiagnosticLevel::kInfo, "restart required"));
}

TEST_F(WidgetCatalogTest, AnotherQtMinorIsRegisteredAsIncompatibleWithTheReason) {
  install(PJ_RUNTIME_WIDGET_OTHER_QT_PLUGIN_PATH, "w");
  PluginRuntimeCatalog catalog(dir_, [this](const Diagnostic& d) { diagnostics_.push_back(d); });
  catalog.scanDirectory();

  ASSERT_EQ(catalog.widgets().size(), 1U);
  const RuntimeWidgetPlugin& plugin = catalog.widgets()[0];
  EXPECT_FALSE(plugin.qt_compatible);
  const QVersionNumber host = QLibraryInfo::version();
  const std::string needs = std::to_string(host.majorVersion()) + "." + std::to_string(host.minorVersion() + 1);
  const std::string uses = std::to_string(host.majorVersion()) + "." + std::to_string(host.minorVersion());
  EXPECT_EQ(plugin.qt_mismatch_reason, "this widget needs Qt " + needs + " (PlotJuggler uses " + uses + ")");
  // Still registered, with its types: the dock explains the mismatch in place of the widget.
  EXPECT_EQ(plugin.widgets.size(), 2U);
  EXPECT_TRUE(hasDiagnostic(DiagnosticLevel::kError, plugin.qt_mismatch_reason));
  // The module cross-check only runs for a compatible build.
  EXPECT_FALSE(hasDiagnostic(DiagnosticLevel::kWarning, "Qt3DCore"));
}

TEST_F(WidgetCatalogTest, AnotherCompilerFamilyIsRegisteredAsIncompatible) {
  install(PJ_RUNTIME_WIDGET_OTHER_COMPILER_PLUGIN_PATH, "w");
  PluginRuntimeCatalog catalog(dir_, [this](const Diagnostic& d) { diagnostics_.push_back(d); });
  catalog.scanDirectory();

  ASSERT_EQ(catalog.widgets().size(), 1U);
  EXPECT_FALSE(catalog.widgets()[0].qt_compatible);
  EXPECT_NE(catalog.widgets()[0].qt_mismatch_reason.find("this widget was built with"), std::string::npos);
  EXPECT_TRUE(hasDiagnostic(DiagnosticLevel::kError, "this widget was built with"));
}

TEST_F(WidgetCatalogTest, StaticRegistrationAcceptsOnceAndRejectsADuplicateId) {
  PluginRuntimeCatalog catalog(dir_, [this](const Diagnostic& d) { diagnostics_.push_back(d); });
  EXPECT_TRUE(catalog.registerStaticWidget(staticWidgetVtable()));
  ASSERT_EQ(catalog.widgets().size(), 1U);
  EXPECT_EQ(catalog.widgets()[0].id, "static-widgets");
  EXPECT_EQ(catalog.widgets()[0].path, "static://static-widgets");
  EXPECT_TRUE(catalog.widgets()[0].qt_compatible);
  ASSERT_EQ(catalog.widgets()[0].widgets.size(), 1U);

  EXPECT_FALSE(catalog.registerStaticWidget(staticWidgetDuplicateVtable()));
  EXPECT_EQ(catalog.widgets().size(), 1U);
  EXPECT_EQ(catalog.widgets()[0].name, "Static Widgets");
  EXPECT_TRUE(hasDiagnostic(DiagnosticLevel::kError, "already statically registered"));
  EXPECT_FALSE(catalog.registerStaticWidget(nullptr));
}

TEST_F(WidgetCatalogTest, StaticSetRegistersWidgetsAndSurvivesReload) {
  PluginRuntimeCatalog catalog(dir_, [this](const Diagnostic& d) { diagnostics_.push_back(d); });
  StaticPluginSet set;
  set.widgets.emplace_back(staticWidgetVtable());
  EXPECT_TRUE(catalog.registerStaticPlugins(set));
  EXPECT_FALSE(catalog.reload());
  catalog.scanDirectory();
  EXPECT_EQ(catalog.widgets().size(), 1U);
}

TEST_F(WidgetCatalogTest, StaticRegistrationCannotSupersedeALoadedLibrary) {
  install(PJ_RUNTIME_WIDGET_PLUGIN_PATH, "w");
  PluginRuntimeCatalog catalog(dir_, [this](const Diagnostic& d) { diagnostics_.push_back(d); });
  catalog.scanDirectory();
  ASSERT_EQ(catalog.widgets().size(), 1U);

  EXPECT_FALSE(catalog.registerStaticWidget(staticWidgetSharingDsoIdVtable()));
  ASSERT_EQ(catalog.widgets().size(), 1U);
  EXPECT_NE(catalog.widgets()[0].path.rfind("static://", 0), 0U);
  EXPECT_TRUE(catalog.widgets()[0].library.valid());
}

}  // namespace
}  // namespace PJ

TEST(WidgetLoadFailureWording, AQtVersionNodeFailureNamesTheQtTheWidgetNeeds) {
  EXPECT_EQ(
      PJ::describeQtVersionLoadFailure(
          "/opt/w/libw.so: version `Qt_6.13' not found (required by /opt/w/libw.so)", 6, 12),
      "needs Qt 6.13 (PlotJuggler uses 6.12)");
  EXPECT_EQ(
      PJ::describeQtVersionLoadFailure("version `Qt_6.14_PRIVATE_API' not found", 6, 12),
      "needs Qt 6.14 (PlotJuggler uses 6.12)");
  EXPECT_EQ(PJ::describeQtVersionLoadFailure("version `GLIBCXX_3.4.32' not found", 6, 12), "");
  EXPECT_EQ(PJ::describeQtVersionLoadFailure("LoadLibraryExW failed (error 126)", 6, 12), "");
}
