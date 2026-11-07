// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A Custom Widget panel through the real shell: the layout element restores into a
// CustomWidgetDockWidget, survives a save, and is kept verbatim when its plugin is not installed.
// Also the startup review of the active-widgets record a run that died leaves behind.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <memory>
#include <vector>

#include "MainWindow.h"
#include "dataset_test_helpers.h"
#include "in_process_widget_plugin.h"
#include "pj_custom_widgets/CustomWidgetDockWidget.h"
#include "pj_marketplace/extension_manager.hpp"
#include "pj_marketplace/qt_diagnostic_bridge.hpp"
#include "pj_runtime/ActiveWidgetsMarker.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "support/gui_test_env.h"
#include "support/layout_import_gui_support.h"
#include "support/loader_test_support.h"

using namespace Qt::StringLiterals;

namespace {

using Peer = PJ::MainWindowLayoutImportTestPeer;
using pj_app_test::pumpUntil;

QString layoutXml(const QString& plugin) {
  return u"<?xml version='1.0' encoding='UTF-8'?>\n"
         u"<root format=\"PlotJuggler\" pj4_version=\"4\">\n"
         u"  <tabbed_widget parent=\"main_window\"><Tab id=\"t1\" containers=\"1\"><Container>\n"
         u"    <DockArea id=\"a1\" name=\"Widget\"><CustomWidget plugin=\"%1\" type=\"gauge\" "
         u"plugin_version=\"1.2.3\">\n"
         u"      <input name=\"value\" slot=\"0\" kind=\"number\" dataset_id=\"1\" dataset_source=\"absent.csv\""
         u" dataset_path=\"/data/absent.csv\" topic=\"/battery\" field=\"voltage\"/>\n"
         u"      <config><![CDATA[{\"unit\":\"km\"}]]></config>\n"
         u"    </CustomWidget></DockArea>\n"
         u"  </Container></Tab></tabbed_widget>\n"
         u"</root>\n"_s.arg(plugin);
}

// The in-process plugin under an id of its own, for the case that disables it.
constexpr const char* kCrashedManifest =
    R"({"id":"crashed_widgets","name":"Crashed Widgets","version":"1.0.0","widgets":[)"
    R"({"type":"gauge","name":"Gauge"}]})";

const PJ_widget_vtable_t* crashedVtable() {
  static const PJ_widget_vtable_t vtable = [] {
    PJ_widget_vtable_t copy = *pj_cw_test::goodVtable();
    copy.manifest_json = kCrashedManifest;
    return copy;
  }();
  return &vtable;
}

class MainWindowCustomWidgetTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(extensions_dir_.isValid());
    ASSERT_TRUE(project_dir_.isValid());
    window_ = std::make_unique<PJ::MainWindow>(extensions_dir_.path());
    // Each case owns its record directory: the default one is shared by every test process.
    marker_dir_ = project_dir_.filePath(u"records"_s);
    auto marker = std::make_shared<PJ::ActiveWidgetsMarker>(marker_dir_);
    marker_path_ = marker->path();
    Peer::activeWidgetsMarker(*window_) = std::move(marker);
    QObject::connect(
        Peer::diagnosticBridge(*window_), &PJ::QtDiagnosticBridge::diagnosticReported, window_.get(),
        [this](int level, const QString& /*source*/, const QString& /*id*/, const QString& message) {
          diagnostics_.emplace_back(level, message);
        });
    // A layout only applies onto loaded data; this dataset holds nothing the layout names.
    const PJ::DatasetId dataset = pj_test::createDataset(session(), "other.csv", /*own_time_domain=*/true);
    ASSERT_NE(dataset, 0U);
    ASSERT_NE(pj_test::addScalarTopic(session(), dataset, "/other", 1'000, 2'000), 0U);
    session().catalogModel().rebuildFromDatastore();
  }

  void TearDown() override {
    window_.reset();
  }

  PJ::AppSession& session() {
    return Peer::session(*window_);
  }

  /// Loads the layout and returns its (only) Custom Widget dock once the restore settled.
  PJ::CustomWidgetDockWidget* loadWith(const QString& plugin) {
    const QString path = project_dir_.filePath(u"widget.pj4.xml"_s);
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(layoutXml(plugin).toUtf8());
    file.close();
    Peer::loadLayout(*window_, path, /*interactive=*/false);
    PJ::CustomWidgetDockWidget* dock = nullptr;
    EXPECT_TRUE(pumpUntil([&]() {
      const auto docks = window_->findChildren<PJ::CustomWidgetDockWidget*>();
      dock = docks.isEmpty() ? nullptr : docks.front();
      return dock != nullptr;
    }));
    return dock;
  }

  QDomElement savedElement(QDomDocument& doc) {
    const QString out = project_dir_.filePath(u"saved.pj4.xml"_s);
    // With the data source: a generic layout sheds every wire's dataset qualifiers (LayoutXml),
    // and these cases check what the panel itself writes back.
    Peer::saveLayoutToPath(*window_, out, /*include_data_source=*/true);
    QFile file(out);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_TRUE(doc.setContent(&file));
    return doc.documentElement().elementsByTagName(u"CustomWidget"_s).at(0).toElement();
  }

  /// A pid no process can have (above every platform's pid range): the run that died.
  static constexpr qint64 kDeadPid = 0x7FFFFFF0;

  /// What a run of `pid` with `plugin_id`/gauge live leaves on disk; its path.
  QString plantRecord(const QString& plugin_id, qint64 pid = kDeadPid) const {
    PJ::ActiveWidgetsMarker run(marker_dir_, pid);
    const int panel = 0;
    run.add(&panel, plugin_id, u"gauge"_s);
    return run.path();
  }

  [[nodiscard]] bool reported(PJ::DiagnosticLevel level, const QString& needle) const {
    return std::ranges::any_of(
        diagnostics_, [&](const auto& d) { return d.first == static_cast<int>(level) && d.second.contains(needle); });
  }

  QTemporaryDir extensions_dir_;
  QTemporaryDir project_dir_;
  QString marker_dir_;
  QString marker_path_;  ///< this process's record
  std::vector<std::pair<int, QString>> diagnostics_;
  std::unique_ptr<PJ::MainWindow> window_;
};

TEST_F(MainWindowCustomWidgetTest, MissingPluginKeepsTheElementThroughLoadAndSave) {
  PJ::CustomWidgetDockWidget* dock = loadWith(u"not_installed"_s);
  ASSERT_NE(dock, nullptr);
  EXPECT_TRUE(dock->isMissing());

  QDomDocument doc;
  const QDomElement element = savedElement(doc);
  ASSERT_FALSE(element.isNull());
  EXPECT_EQ(element.attribute(u"plugin"_s), u"not_installed"_s);
  EXPECT_EQ(element.attribute(u"type"_s), u"gauge"_s);
  EXPECT_EQ(element.attribute(u"plugin_version"_s), u"1.2.3"_s);
  const QDomElement input = element.firstChildElement(u"input"_s);
  EXPECT_EQ(input.attribute(u"topic"_s), u"/battery"_s);
  // Unresolved wires are written back as loaded, except that the path goes through the same
  // normalization as a registered source (drive-qualified on Windows): compare path components.
  EXPECT_TRUE(input.attribute(u"dataset_path"_s).endsWith(u"/data/absent.csv"_s))
      << qPrintable(input.attribute(u"dataset_path"_s));
  EXPECT_NE(element.firstChildElement(u"config"_s).text().indexOf(u"km"_s), -1);
}

TEST_F(MainWindowCustomWidgetTest, InstalledPluginRestoresIntoALiveWidgetAndSavesItsPendingWire) {
  pj_cw_test::g = pj_cw_test::PluginStats{};
  ASSERT_TRUE(session().extensionCatalog().pluginCatalog().registerStaticWidget(pj_cw_test::goodVtable()));
  PJ::CustomWidgetDockWidget* dock = loadWith(u"test_widgets"_s);
  ASSERT_NE(dock, nullptr);
  EXPECT_FALSE(dock->isMissing());
  EXPECT_EQ(pj_cw_test::g.widgets_created, 1);
  ASSERT_EQ(dock->bindings().size(), 1U);
  EXPECT_FALSE(dock->bindings()[0].resolved) << "its dataset is not loaded";
  EXPECT_NE(pj_cw_test::g.last_config.find("km"), std::string::npos);

  QDomDocument doc;
  const QDomElement element = savedElement(doc);
  ASSERT_FALSE(element.isNull());
  EXPECT_EQ(element.attribute(u"plugin"_s), u"test_widgets"_s);
  const QDomElement input = element.firstChildElement(u"input"_s);
  EXPECT_EQ(input.attribute(u"dataset_source"_s), u"absent.csv"_s);
  EXPECT_EQ(input.attribute(u"field"_s), u"voltage"_s);

  // Closing the window destroys the docks before the session: the plugin instance dies with them.
  window_.reset();
  EXPECT_EQ(pj_cw_test::g.destroyed, 1);
  EXPECT_EQ(pj_cw_test::g.destroyed_while_parented, 0);
}

TEST_F(MainWindowCustomWidgetTest, AWireThatStartsWaitingLaterStillDemandsItsTopic) {
  ASSERT_TRUE(session().extensionCatalog().pluginCatalog().registerStaticWidget(pj_cw_test::goodVtable()));
  PJ::CustomWidgetDockWidget* dock = loadWith(u"test_widgets"_s);
  ASSERT_NE(dock, nullptr);
  QDomDocument doc;
  const QDomElement saved = savedElement(doc);
  dock->removeBinding(u"value"_s, 0);
  ASSERT_TRUE(pumpUntil([&]() { return Peer::binderEmpty(*window_); }));

  ASSERT_TRUE(dock->xmlLoadState(saved));
  ASSERT_TRUE(dock->hasPendingWires());
  EXPECT_TRUE(pumpUntil([&]() { return !Peer::binderEmpty(*window_); }));
}

TEST_F(MainWindowCustomWidgetTest, TheRecordFollowsLivePanelsAndAWindowCloseClearsIt) {
  ASSERT_TRUE(session().extensionCatalog().pluginCatalog().registerStaticWidget(pj_cw_test::goodVtable()));
  PJ::CustomWidgetDockWidget* dock = loadWith(u"test_widgets"_s);
  ASSERT_NE(dock, nullptr);
  ASSERT_FALSE(dock->isMissing());
  EXPECT_EQ(
      PJ::ActiveWidgetsMarker::read(marker_path_).entries,
      (std::vector<PJ::ActiveWidgetsMarker::Entry>{{u"test_widgets"_s, u"gauge"_s}}));
  window_.reset();
  EXPECT_FALSE(QFile::exists(marker_path_)) << "a clean exit leaves no record";
}

// The startup review writes the persisted disabled list through the ExtensionManager, which needs
// the extension store's single-writer lease. Concurrent cases of this runner would compete for the
// lease and the settings: each case gets an application name of its own, so both are private.
class UncleanShutdownTest : public MainWindowCustomWidgetTest {
 protected:
  void SetUp() override {
    QCoreApplication::setApplicationName(
        u"%1-unclean-%2"_s.arg(QCoreApplication::applicationName()).arg(QCoreApplication::applicationPid()));
    MainWindowCustomWidgetTest::SetUp();
  }

  void TearDown() override {
    MainWindowCustomWidgetTest::TearDown();
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).removeRecursively();
    QSettings settings;
    settings.clear();
    settings.sync();
    QFile::remove(settings.fileName());
  }
};

TEST_F(UncleanShutdownTest, AnUnattendedStartWarnsAboutTheCrashedRunAndStillRestoresTheWidget) {
  ASSERT_TRUE(session().extensionCatalog().pluginCatalog().registerStaticWidget(pj_cw_test::goodVtable()));
  const QString crashed = plantRecord(u"test_widgets"_s);

  window_->reviewUncleanShutdown(/*interactive=*/false);  // returns without a dialog: it would block here
  EXPECT_FALSE(QFile::exists(crashed)) << "the previous run's record is consumed";
  EXPECT_TRUE(pumpUntil([&]() {
    return reported(PJ::DiagnosticLevel::kWarning, u"did not shut down cleanly"_s) &&
           reported(PJ::DiagnosticLevel::kWarning, u"test_widgets/gauge"_s);
  }));

  PJ::CustomWidgetDockWidget* dock = loadWith(u"test_widgets"_s);
  ASSERT_NE(dock, nullptr);
  EXPECT_FALSE(dock->isMissing()) << "an unattended run keeps the widgets";
  EXPECT_TRUE(QFile::exists(marker_path_)) << "this run records its own live panels";
}

TEST_F(UncleanShutdownTest, ALiveInstancesRecordIsLeftAloneAndADeadRunsIsReviewed) {
  // The record of a process that is still running (this one stands in for a second instance)
  // names a widget that did not crash: it is neither reported nor erased.
  const QString live = plantRecord(u"running_widgets"_s, PJ::ActiveWidgetsMarker::currentPid());
  const QString crashed = plantRecord(u"test_widgets"_s);
  window_->reviewUncleanShutdown(/*interactive=*/false);
  EXPECT_TRUE(pumpUntil([&]() { return reported(PJ::DiagnosticLevel::kWarning, u"test_widgets/gauge"_s); }));
  EXPECT_FALSE(reported(PJ::DiagnosticLevel::kWarning, u"running_widgets"_s));
  EXPECT_TRUE(QFile::exists(live));
  EXPECT_FALSE(QFile::exists(crashed));
}

TEST_F(UncleanShutdownTest, AStartWithoutARecordAsksNothing) {
  window_->reviewUncleanShutdown(/*interactive=*/true);  // a dialog would block here
  QCoreApplication::processEvents();
  EXPECT_FALSE(reported(PJ::DiagnosticLevel::kWarning, u"did not shut down cleanly"_s));
}

TEST_F(UncleanShutdownTest, AnUnreadableRecordIsAWarningAndNoQuestion) {
  ASSERT_TRUE(QDir().mkpath(marker_dir_));
  const QString path = QDir(marker_dir_).filePath(PJ::ActiveWidgetsMarker::fileNameFor(kDeadPid));
  QFile file(path);
  ASSERT_TRUE(file.open(QIODevice::WriteOnly));
  file.write("{ not json");
  file.close();
  window_->reviewUncleanShutdown(/*interactive=*/true);
  EXPECT_TRUE(pumpUntil([&]() { return reported(PJ::DiagnosticLevel::kWarning, u"could not be read"_s); }));
  EXPECT_FALSE(QFile::exists(path));
}

TEST_F(UncleanShutdownTest, AnsweringDisableTurnsThePluginOffAndItsPanelsRestoreAsPlaceholders) {
  pj_cw_test::g = pj_cw_test::PluginStats{};
  const QString id = u"crashed_widgets"_s;
  auto& extensions = session().extensionCatalog();
  ASSERT_TRUE(extensions.pluginCatalog().registerStaticWidget(crashedVtable()));
  ASSERT_NE(extensions.findWidget(id), nullptr);
  plantRecord(id);

  // Answer the question the way a user would: click its "Disable" button once it is up.
  bool clicked = false;
  QTimer answer;
  QObject::connect(&answer, &QTimer::timeout, [&]() {
    QWidget* modal = QApplication::activeModalWidget();
    if (modal == nullptr || clicked) {
      return;
    }
    for (QPushButton* button : modal->findChildren<QPushButton*>()) {
      if (button->text() == u"Disable"_s) {
        clicked = true;
        button->click();
        return;
      }
    }
  });
  answer.start(20);
  window_->reviewUncleanShutdown(/*interactive=*/true);
  answer.stop();
  ASSERT_TRUE(clicked);

  EXPECT_TRUE(PJ::ExtensionManager::disabledExtensionIds().contains(id))
      << "the write needs this case's own store lease (see UncleanShutdownTest)";
  EXPECT_EQ(extensions.findWidget(id), nullptr) << "withdrawn from the catalog for this session";

  PJ::CustomWidgetDockWidget* dock = loadWith(id);
  ASSERT_NE(dock, nullptr);
  EXPECT_TRUE(dock->isMissing());
  EXPECT_EQ(pj_cw_test::g.widgets_created, 0);
}

}  // namespace
