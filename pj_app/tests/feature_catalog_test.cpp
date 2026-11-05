// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// resources/feature_catalog.json tells the assistant agent where things are in
// the UI. These tests hold it to the UI: each entry point must contain the
// complete live label of every menu, action and button it names, read from the
// real widgets, so renaming one fails here until the catalog follows. The host's
// own default entry points are held to the same labels.

#include <gtest/gtest.h>

#include <QAbstractButton>
#include <QAction>
#include <QCoreApplication>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <algorithm>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "MainWindow.h"
#include "PreferencesDialog.h"
#include "PreferencesNavRow.h"
#include "Theme.h"
#include "TitleBar.h"
#include "pj_base/data_source_protocol.h"
#include "pj_plotting/TabbedPlotWidget.h"
#include "pj_runtime/FeatureCatalogHost.h"
#include "pj_widgets/Search.h"
#include "pj_widgets/Timeline.h"
#include "pj_widgets/VisualizationPlaceholderWidget.h"
#include "support/gui_test_env.h"
#include "ui/CurveListPanel.h"

using namespace Qt::StringLiterals;
using Json = nlohmann::json;

namespace PJ {

class FeatureCatalogTestPeer {
 public:
  static QAction* openData(MainWindow& w) {
    return w.action_open_data_;
  }
  static QAction* loadLayout(MainWindow& w) {
    return w.action_load_layout_;
  }
  static QAction* saveLayout(MainWindow& w) {
    return w.action_save_layout_;
  }
  static QAction* preferences(MainWindow& w) {
    return w.action_preferences_;
  }
  static QMenu* installedExtensions(MainWindow& w) {
    return w.installed_extensions_menu_;
  }
  static Theme& theme(MainWindow& w) {
    return *w.theme_;
  }
  static Timeline* sourceTimeline(MainWindow& w) {
    return w.source_timeline_;
  }
};

}  // namespace PJ

namespace {

class SettingsEnvironment : public ::testing::Environment {
 public:
  void SetUp() override {
    if (!pj_app_test::isTestSuiteSelected("FeatureCatalogResourceTest")) {
      return;
    }
    QCoreApplication::setOrganizationName(u"PlotJugglerTest"_s);
    QCoreApplication::setApplicationName(u"feature_catalog_test"_s);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir_.path());
  }

 private:
  QTemporaryDir settings_dir_;
};

[[maybe_unused]] auto* const kEnv = ::testing::AddGlobalTestEnvironment(new SettingsEnvironment);

Json resource() {
  QFile file(u":/resources/feature_catalog.json"_s);
  if (!file.open(QIODevice::ReadOnly)) {
    return {};
  }
  return Json::parse(file.readAll().toStdString(), nullptr, /*allow_exceptions=*/false);
}

std::string menuTitle(const QMenu* menu) {
  return menu == nullptr ? std::string() : menu->title().remove(u'&').toStdString();
}

template <typename Widget>
Widget* child(QObject& parent, const char* name) {
  return parent.findChild<Widget*>(QString::fromLatin1(name));
}

std::string toolTipOf(QObject& parent, const char* name) {
  auto* widget = child<QWidget>(parent, name);
  return widget == nullptr ? std::string() : widget->toolTip().toStdString();
}

std::string textOf(QObject& parent, const char* name) {
  if (auto* label = child<QLabel>(parent, name)) {
    return label->text().toStdString();
  }
  if (auto* button = child<QAbstractButton>(parent, name)) {
    return button->text().toStdString();
  }
  return {};
}

// Every label an entry point may name, read from the live widgets by identity
// (object name or member), never by the text being checked.
struct LiveLabels {
  std::map<std::string, std::string> by_key;
  std::vector<std::string> preference_pages;
};

LiveLabels readLiveLabels(PJ::MainWindow& window) {
  using Peer = PJ::FeatureCatalogTestPeer;
  LiveLabels live;
  auto& labels = live.by_key;
  PJ::TitleBar* bar = window.titleBar();

  labels["menu.file"] = menuTitle(bar->fileMenu());
  labels["menu.toolbox"] = menuTitle(bar->toolboxMenu());
  labels["menu.help"] = menuTitle(bar->helpMenu());
  labels["menu.installed_extensions"] = menuTitle(Peer::installedExtensions(window));
  labels["action.open_data"] = Peer::openData(window)->text().toStdString();
  labels["action.load_layout"] = Peer::loadLayout(window)->text().toStdString();
  labels["action.save_layout"] = Peer::saveLayout(window)->text().toStdString();
  labels["action.preferences"] = Peer::preferences(window)->text().toStdString();
  if (auto* marketplace = child<QAction>(window, "actionMarketplace")) {
    labels["action.marketplace"] = marketplace->text().toStdString();
  }
  if (auto* updates = child<QAction>(window, "actionCheckForUpdates")) {
    labels["action.check_updates"] = updates->text().toStdString();
  }
  labels["titlebar.diagnostics"] = toolTipOf(*bar, "buttonNotifications");
  labels["titlebar.update"] = textOf(*bar, "buttonExtensionUpdate");

  // Sources panel: the tabs and most buttons are icon-only, so their tooltip
  // is the label the user sees.
  labels["sources.title"] = textOf(window, "labelInput");
  labels["sources.tab_file"] = toolTipOf(window, "tabFile");
  labels["sources.tab_stream"] = toolTipOf(window, "tabStream");
  labels["sources.tab_cloud"] = toolTipOf(window, "tabCloud");
  labels["sources.load_file"] = toolTipOf(window, "buttonLoadDatafile");
  labels["sources.recent"] = toolTipOf(window, "buttonRecentFiles");
  labels["sources.start_streaming"] = toolTipOf(window, "buttonStreamingOptions");
  labels["sources.record"] = toolTipOf(window, "buttonStreamingRecord");
  labels["sources.cloud_open"] = toolTipOf(window, "buttonCloudOpen");

  // Datasets and Custom Topics.
  labels["datasets.title"] = textOf(window, "labelTimeseries");
  if (auto* panel = window.findChild<PJ::CurveListPanel*>()) {
    if (auto* search = panel->findChild<PJ::Search*>()) {
      if (auto* edit = search->findChild<QLineEdit*>()) {
        labels["datasets.filter"] = edit->placeholderText().toStdString();
      }
    }
  }
  labels["custom.title"] = textOf(window, "labelCustom");
  labels["custom.add"] = toolTipOf(window, "buttonAddCustom");

  // Bottom bar and source timeline.
  labels["playback.play"] = toolTipOf(window, "buttonPlay");
  labels["playback.loop"] = toolTipOf(window, "playbackLoop");
  labels["playback.rate"] = toolTipOf(window, "playbackRate");
  labels["playback.step"] = toolTipOf(window, "labelStep");
  if (auto* tabs = window.findChild<PJ::TabbedPlotWidget*>()) {
    labels["tabs.toggle_bottom"] = tabs->bottomPanelButton()->toolTip().toStdString();
  }
  if (PJ::Timeline* timeline = Peer::sourceTimeline(window)) {
    labels["timeline.align"] = textOf(*timeline, "timelineAlignButton");
    labels["timeline.merge"] = toolTipOf(*timeline, "timelineMergeButton");
  }

  // Plot area: the toolbar of the first plot, and the empty-area chooser.
  labels["plot.split_h"] = toolTipOf(window, "buttonSplitHorizontal");
  labels["plot.split_v"] = toolTipOf(window, "buttonSplitVertical");
  labels["plot.fullscreen"] = toolTipOf(window, "buttonFullscreen");
  PJ::VisualizationPlaceholderWidget placeholder;
  labels["viz.2d"] = toolTipOf(placeholder, "buttonVizScene2D");
  labels["viz.3d"] = toolTipOf(placeholder, "buttonVizScene3D");
  labels["viz.state"] = toolTipOf(placeholder, "buttonVizStateTransitions");
  labels["viz.custom"] = toolTipOf(placeholder, "buttonVizCustomWidget");

  // Preferences pages (one nav row each) and the startup update check.
  PJ::PreferencesDialog prefs(Peer::theme(window), &window);
  for (auto* row : prefs.findChildren<PJ::PreferencesNavRow*>()) {
    if (auto* label = row->findChild<QLabel*>()) {
      live.preference_pages.push_back(label->text().toStdString());
    }
  }
  labels["prefs.check_updates"] = textOf(prefs, "labelCheckUpdates");
  return live;
}

// The live labels each core entry's entry point names. An entry missing here
// fails the test: a new catalog entry gets its labels checked from day one.
const std::map<std::string, std::vector<std::string>>& labelsNamedBy() {
  static const std::map<std::string, std::vector<std::string>> table = {
      {"open_data_file", {"menu.file", "action.open_data", "sources.title", "sources.tab_file", "sources.load_file"}},
      // This entry uses command-line flags or held keys, without widget labels.
      {"command_line_data", {}},
      {"live_streaming", {"sources.title", "sources.tab_stream", "sources.start_streaming"}},
      {"record_streams", {"sources.title", "sources.tab_stream", "sources.record"}},
      {"cloud_sources", {"sources.title", "sources.tab_cloud", "sources.cloud_open"}},
      {"layouts", {"menu.file", "action.save_layout", "action.load_layout", "sources.title", "sources.recent"}},
      {"extensions", {"menu.file", "action.marketplace", "menu.help", "menu.installed_extensions"}},
      {"datasets_tree", {"datasets.title", "datasets.filter"}},
      {"custom_series", {"custom.title", "custom.add"}},
      {"viewer_2d", {"viz.2d", "datasets.title"}},
      {"scene_3d", {"viz.3d", "datasets.title"}},
      {"playback", {"playback.play", "playback.loop", "playback.rate", "playback.step"}},
      {"source_timeline", {"tabs.toggle_bottom", "timeline.align", "timeline.merge", "datasets.title"}},
      {"state_transitions", {"viz.state", "datasets.title"}},
      {"custom_widget", {"viz.custom", "datasets.title"}},
      {"plot_tabs", {"plot.split_h", "plot.split_v", "plot.fullscreen", "datasets.title"}},
      // This entry uses command-line flags or held keys, without widget labels.
      {"axis_wheel_zoom", {}},
      {"preferences", {"menu.file", "action.preferences"}},
      {"diagnostics", {"titlebar.diagnostics"}},
      {"check_updates",
       {"menu.help", "action.check_updates", "menu.file", "action.preferences", "prefs.check_updates"}},
  };
  return table;
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

// One window, built on first use, serves every case that reads the live labels.
class FeatureCatalogResourceTest : public ::testing::Test {
 protected:
  static void TearDownTestSuite() {
    live_.reset();
    window_.reset();
    extensions_dir_.reset();
  }

  static const LiveLabels& live() {
    if (!live_) {
      extensions_dir_ = std::make_unique<QTemporaryDir>();
      window_ = std::make_unique<PJ::MainWindow>(extensions_dir_->path());
      live_ = std::make_unique<LiveLabels>(readLiveLabels(*window_));
    }
    return *live_;
  }

 private:
  static inline std::unique_ptr<QTemporaryDir> extensions_dir_;
  static inline std::unique_ptr<PJ::MainWindow> window_;
  static inline std::unique_ptr<LiveLabels> live_;
};

}  // namespace

TEST_F(FeatureCatalogResourceTest, ResourceIsValidAndEveryEntryHasEvidence) {
  const Json doc = resource();
  ASSERT_TRUE(doc.is_object()) << "the embedded resource is not valid JSON";
  EXPECT_EQ(doc.value("schema", 0), 1);
  ASSERT_TRUE(doc.contains("core") && doc.at("core").is_array());
  ASSERT_TRUE(doc.contains("limitations") && doc.at("limitations").is_array());
  EXPECT_FALSE(doc.at("core").empty());
  EXPECT_FALSE(doc.at("limitations").empty());

  std::set<std::string> ids;
  const auto check = [&ids](const Json& entry, std::initializer_list<const char*> fields) {
    ASSERT_TRUE(entry.is_object());
    const std::string id = entry.value("id", "");
    EXPECT_TRUE(ids.insert(id).second) << "duplicate id " << id;
    for (const char* field : fields) {
      ASSERT_TRUE(entry.contains(field) && entry.at(field).is_string()) << id << "." << field;
      EXPECT_FALSE(entry.at(field).get<std::string>().empty()) << id << "." << field;
    }
    EXPECT_LE(entry.value("summary", "").size(), 600U) << id;  // the host's bound, in bytes here
  };
  for (const auto& entry : doc.at("core")) {
    check(entry, {"id", "name", "summary", "entry_point", "evidence"});
    // In-app verification is recorded by a person; the field must exist.
    EXPECT_TRUE(entry.contains("verified") && entry.at("verified").is_string()) << entry.value("id", "");
  }
  for (const auto& entry : doc.at("limitations")) {
    check(entry, {"id", "summary", "evidence"});
  }
}

// "path#token; path#token": each cited file exists and its text contains the
// token, so a citation survives moved lines and fails only when the code it
// cites is gone.
TEST_F(FeatureCatalogResourceTest, EvidenceTokensAppearInTheCitedFiles) {
  const Json doc = resource();
  ASSERT_TRUE(doc.is_object());
  for (const char* list : {"core", "limitations"}) {
    for (const auto& entry : doc.at(list)) {
      const QString evidence = QString::fromStdString(entry.value("evidence", ""));
      for (const QString& citation : evidence.split(u"; "_s)) {
        const qsizetype hash = citation.indexOf(u'#');
        ASSERT_GT(hash, 0) << citation.toStdString();
        const QString token = citation.mid(hash + 1);
        ASSERT_FALSE(token.isEmpty()) << citation.toStdString();
        QFile file(QStringLiteral(PJ_FEATURE_CATALOG_SOURCE_DIR "/") + citation.left(hash));
        ASSERT_TRUE(file.open(QIODevice::ReadOnly)) << citation.toStdString();
        EXPECT_TRUE(QString::fromUtf8(file.readAll()).contains(token)) << citation.toStdString();
      }
    }
  }
}

TEST_F(FeatureCatalogResourceTest, EntryPointsNameTheLiveLabels) {
  const LiveLabels& live = FeatureCatalogResourceTest::live();

  const Json doc = resource();
  ASSERT_TRUE(doc.is_object());
  std::set<std::string> resource_ids;
  for (const auto& entry : doc.at("core")) {
    const std::string id = entry.value("id", "");
    const std::string entry_point = entry.value("entry_point", "");
    resource_ids.insert(id);
    const auto row = labelsNamedBy().find(id);
    ASSERT_NE(row, labelsNamedBy().end()) << "core entry '" << id << "' has no live-label row in this test";
    for (const std::string& key : row->second) {
      const auto label = live.by_key.find(key);
      ASSERT_NE(label, live.by_key.end()) << key;
      ASSERT_FALSE(label->second.empty()) << "no live label read for " << key;
      EXPECT_TRUE(contains(entry_point, label->second))
          << id << ": entry point \"" << entry_point << "\" lacks the live label \"" << label->second << "\" (" << key
          << ")";
    }
  }
  for (const auto& [id, keys] : labelsNamedBy()) {
    EXPECT_TRUE(resource_ids.contains(id)) << "stale live-label row: " << id;
  }

  // The update-check entry names the Appearance page as it is labelled.
  EXPECT_TRUE(
      std::find(live.preference_pages.begin(), live.preference_pages.end(), "Appearance") !=
      live.preference_pages.end())
      << "no Appearance page in Preferences";

  // The Preferences entry lists every page there is.
  ASSERT_FALSE(live.preference_pages.empty());
  for (const auto& entry : doc.at("core")) {
    if (entry.value("id", "") == "preferences") {
      for (const std::string& page : live.preference_pages) {
        EXPECT_TRUE(contains(entry.value("entry_point", ""), page)) << "Preferences page missing: " << page;
      }
    }
  }
}

TEST_F(FeatureCatalogResourceTest, HostDefaultEntryPointsNameTheLiveLabels) {
  const auto& labels = live().by_key;
  using Family = PJ::PluginFamily;
  const auto entry = [](Family family, const char* category, std::vector<std::string> tags = {},
                        std::optional<uint64_t> caps = std::nullopt) {
    PJ::FeatureCatalogHost::Plugin plugin;
    plugin.family = family;
    plugin.id = "id";
    plugin.name = "Name";
    plugin.category = category;
    plugin.tags = std::move(tags);
    plugin.source_capabilities = caps;
    return PJ::FeatureCatalogHost::defaultEntryPoint(plugin);
  };

  const auto expectNames = [&labels](const std::string& text, std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
      EXPECT_TRUE(contains(text, labels.at(key))) << text << " lacks " << labels.at(key);
    }
  };
  expectNames(
      entry(Family::kDataSource, "data_loader"),
      {"sources.title", "sources.tab_file", "menu.file", "action.open_data"});
  expectNames(
      entry(Family::kDataSource, "", {}, PJ_DATA_SOURCE_CAPABILITY_CONTINUOUS_STREAM),
      {"sources.title", "sources.tab_stream", "sources.start_streaming"});
  expectNames(
      entry(Family::kToolbox, "toolbox", {"cloud"}), {"sources.title", "sources.tab_cloud", "sources.cloud_open"});
  expectNames(entry(Family::kToolbox, "toolbox"), {"menu.toolbox"});
}
