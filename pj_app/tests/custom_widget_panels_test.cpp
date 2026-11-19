// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The Custom Widget side-panel pages (picker of installed widget types, per-panel config page with inputs, wires
// and settings form) and the shell flow from the placeholder's Custom Widget button to a bound panel. Set
// PJ_UI_TEST_DUMP_DIR to also save dark-theme captures of both pages.

#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTreeWidgetItemIterator>
#include <memory>

#include "MainWindow.h"
#include "Theme.h"
#include "dataset_test_helpers.h"
#include "dock_test_support.h"
#include "in_process_widget_plugin.h"
#include "pj_custom_widgets/CustomWidgetDockWidget.h"
#include "pj_plotting/DockWidget.h"
#include "pj_plotting/PlotDocker.h"
#include "pj_plotting/TabbedPlotWidget.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_widgets/ColorPickerWidget.h"
#include "pj_widgets/CurveTreeView.h"
#include "pj_widgets/DoubleScrubber.h"
#include "pj_widgets/Search.h"
#include "pj_widgets/SectionHeaderBand.h"
#include "pj_widgets/Style.h"
#include "pj_widgets/SvgUtil.h"
#include "pj_widgets/ToastNotification.h"
#include "pj_widgets/ToggleSwitch.h"
#include "pj_widgets/VisualizationPlaceholderWidget.h"
#include "scene_object_classification.h"
#include "support/gui_test_env.h"
#include "support/layout_import_gui_support.h"
#include "support/loader_test_support.h"
#include "ui/CustomWidgetConfigPanel.h"
#include "ui/CustomWidgetPickerMenu.h"

using namespace Qt::StringLiterals;

namespace {

using pj_cw_test::g;
using pj_cw_test::goodVtable;
using pj_cw_test::otherQtVtable;

// A gauge-like library: every settings type, inline icons, one `multiple` input (table).
constexpr const char* kGaugeManifest =
    R"({"id":"ui_widgets","name":"Basic Widgets","version":"1.0.0","widgets":[)"
    R"({"type":"gauge","name":"Gauge",)"
    R"("icon":"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'><path fill='#3D3D3D' )"
    R"(d='M12 4a9 9 0 0 0-9 9h2a7 7 0 0 1 14 0h2a9 9 0 0 0-9-9zm-1 9.5 5-5 1 1-5 5z'/></svg>",)"
    R"("settings":[)"
    R"({"key":"min","type":"number","default":0,"label":"Minimum"},)"
    R"({"key":"max","type":"number","min":0,"max":1000,"default":100,"label":"Maximum"},)"
    R"({"key":"show_value","type":"boolean","default":true,"label":"Show value"},)"
    R"({"key":"unit","type":"text","default":"V","label":"Unit"},)"
    R"({"key":"needle","type":"color","default":"#e05a47","label":"Needle colour"},)"
    R"({"key":"mode","type":"enum","options":["arc","bar"],"default":"arc","label":"Mode"}]},)"
    R"({"type":"table","name":"Value Table",)"
    R"("icon":"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'><path fill='#3D3D3D' )"
    R"(d='M3 5h18v2H3zm0 6h18v2H3zm0 6h18v2H3z'/></svg>"},)"
    R"({"type":"viewer","name":"Object Viewer"}]})";

// An `integer` setting: whole numbers in the form and in the JSON.
constexpr const char* kIntegerManifest =
    R"({"id":"int_widgets","name":"Integer Widgets","version":"1.0.0","widgets":[)"
    R"({"type":"gauge","name":"Gauge","settings":[)"
    R"({"key":"decimals","type":"integer","min":0,"max":6,"default":2,"label":"Decimals"}]}]})";

const PJ_widget_vtable_t* vtableWithManifest(const char* manifest) {
  // One static copy per manifest literal; the catalog keeps the pointer.
  static std::map<const char*, PJ_widget_vtable_t> copies;
  auto [it, inserted] = copies.try_emplace(manifest, *goodVtable());
  it->second.manifest_json = manifest;
  return &it->second;
}

// The menu entry of widget `name` (its text is "<name> (from <library>)").
QAction* entryNamed(QMenu* menu, const QString& name) {
  for (QAction* entry : menu->actions()) {
    if (entry->text().startsWith(name + u" (from "_s)) {
      return entry;
    }
  }
  return nullptr;
}

// A pick as the user makes it: QMenu hides itself (handing the focus back) before it emits triggered.
void pick(QMenu* menu, const QString& name) {
  QAction* entry = entryNamed(menu, name);
  ASSERT_NE(entry, nullptr) << name.toStdString();
  menu->hide();
  entry->trigger();
}

QTreeWidgetItem* treeItemWithKey(QTreeWidget* tree, const QString& key) {
  for (QTreeWidgetItemIterator it(tree); *it != nullptr; ++it) {
    if (PJ::CurveTreeView::catalogKeyOf(*it) == key) {
      return *it;
    }
  }
  return nullptr;
}

template <typename Widget>
Widget* withProperty(QWidget* root, const char* object_name, const char* property, const QVariant& value) {
  for (Widget* widget : root->findChildren<Widget*>(QString::fromLatin1(object_name))) {
    if (widget->property(property) == value) {
      return widget;
    }
  }
  return nullptr;
}

/// Dark theme for the captures, as the app applies it.
void applyDarkTheme() {
  QApplication::setStyle(new PJ::Style(u"Fusion"_s));
  QSettings().setValue(PJ::kThemeSettingsKey, u"dark"_s);
  PJ::Theme theme;
  theme.setTheme(u"dark"_s);
  qApp->setStyleSheet(theme.expandedQss());
}

/// Saves `widget` as its window paints it (a child grabbed alone has no backdrop behind it).
void dump(QWidget* widget, const QString& file_name) {
  const QString dir = qEnvironmentVariable("PJ_UI_TEST_DUMP_DIR");
  QWidget* window = widget->window();
  const QImage image = window->grab(QRect(widget->mapTo(window, QPoint(0, 0)), widget->size())).toImage();
  ASSERT_FALSE(image.isNull());
  if (!dir.isEmpty()) {
    QDir().mkpath(dir);
    EXPECT_TRUE(image.save(QDir(dir).filePath(file_name)));
  }
}

// ---------------------------------------------------------------- pages over a bare session

class CustomWidgetPagesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g = pj_cw_test::PluginStats{};
    app_ = std::make_unique<PJ::AppSession>();
    dataset_ = pj_cw_test::createDataset(*app_, "robot.mcap");
    number_ = pj_cw_test::addNumberTopic(*app_, dataset_, "/battery", {{1.0, 12.1}, {2.0, 12.6}});
    text_ = pj_cw_test::addTextTopic(*app_, dataset_, "/mode", {{1.0, "IDLE"}});
    ASSERT_FALSE(number_.isEmpty());
    ASSERT_FALSE(text_.isEmpty());
  }

  PJ::PluginRuntimeCatalog& plugins() {
    return app_->extensionCatalog().pluginCatalog();
  }

  std::unique_ptr<PJ::CustomWidgetDockWidget> makeDock(const QString& plugin, const QString& type) {
    auto dock = std::make_unique<PJ::CustomWidgetDockWidget>(
        &app_->sessionManager(), &app_->catalogModel(), &app_->extensionCatalog(), &app_->playbackEngine());
    dock->bindToWidget(plugin, type);
    return dock;
  }

  std::unique_ptr<PJ::CustomWidgetConfigPanel> makePanel(PJ::CustomWidgetDockWidget* dock) {
    auto panel = std::make_unique<PJ::CustomWidgetConfigPanel>();
    panel->setServices(&app_->catalogModel(), &app_->extensionCatalog());
    panel->bindDock(dock);
    return panel;
  }

  std::unique_ptr<PJ::AppSession> app_;
  PJ::DatasetId dataset_ = 0;
  QString number_;
  QString text_;
};

TEST_F(CustomWidgetPagesTest, PickerListsEveryTypeAndDisablesTheOtherQtLibrary) {
  ASSERT_TRUE(plugins().registerStaticWidget(goodVtable()));
  ASSERT_TRUE(plugins().registerStaticWidget(otherQtVtable()));
  PJ::CustomWidgetPickerMenu menu;
  menu.rebuild(app_->extensionCatalog(), 1.0);
  EXPECT_TRUE(menu.toolTipsVisible()) << "a menu hides its entries' tooltips unless told otherwise";
  EXPECT_EQ(menu.objectName(), u"PJMenu"_s) << "the app stylesheet styles menus by this name";

  QString reason;
  for (const PJ::LoadedWidget& library : app_->extensionCatalog().widgets()) {
    if (!library.qt_compatible) {
      reason = QString::fromStdString(library.qt_mismatch_reason);
    }
  }
  ASSERT_FALSE(reason.isEmpty());

  const QList<QAction*> entries = menu.actions();
  EXPECT_EQ(entries.size(), 4);  // gauge, table, viewer + the other-Qt gauge
  int disabled = 0;
  for (QAction* entry : entries) {
    EXPECT_FALSE(entry->icon().isNull());
    if (!entry->isEnabled()) {
      ++disabled;
      EXPECT_TRUE(entry->text().contains(u"from Other Qt"_s));
      EXPECT_TRUE(entry->toolTip().contains(reason.toHtmlEscaped())) << entry->toolTip().toStdString();
    }
  }
  EXPECT_EQ(disabled, 1);

  QSignalSpy chosen(&menu, &PJ::CustomWidgetPickerMenu::widgetChosen);
  QAction* table = entryNamed(&menu, u"Table"_s);
  ASSERT_NE(table, nullptr);
  EXPECT_TRUE(table->text().contains(u"from Test Widgets"_s));
  table->trigger();
  ASSERT_EQ(chosen.count(), 1);
  EXPECT_EQ(chosen.at(0).at(0).toString(), u"test_widgets"_s);
  EXPECT_EQ(chosen.at(0).at(1).toString(), u"table"_s);

  for (QAction* entry : entries) {
    if (!entry->isEnabled()) {
      entry->trigger();
    }
  }
  EXPECT_EQ(chosen.count(), 1) << "a disabled entry is not a choice";
}

TEST_F(CustomWidgetPagesTest, MenuWithNothingInstalledSaysSo) {
  PJ::CustomWidgetPickerMenu menu;
  menu.rebuild(app_->extensionCatalog(), 1.0);
  ASSERT_EQ(menu.actions().size(), 1);
  EXPECT_FALSE(menu.actions().first()->isEnabled());
  EXPECT_EQ(menu.actions().first()->text(), u"No custom widgets installed."_s);
}

TEST_F(CustomWidgetPagesTest, AnAmpersandInANameShowsLiterally) {
  static constexpr const char* kAmpersandManifest =
      R"({"id":"amp_widgets","name":"Speed & Co","version":"1.0.0","widgets":[)"
      R"({"type":"st","name":"Speed & Torque"}]})";
  ASSERT_TRUE(plugins().registerStaticWidget(vtableWithManifest(kAmpersandManifest)));
  PJ::CustomWidgetPickerMenu menu;
  menu.rebuild(app_->extensionCatalog(), 1.0);
  ASSERT_EQ(menu.actions().size(), 1);
  // A single '&' in a menu entry marks the next letter as its shortcut and is not drawn.
  EXPECT_EQ(menu.actions().first()->text(), u"Speed && Torque (from Speed && Co)"_s);
}

TEST(SceneObjectClassificationTest, OnlyTopicsACustomWidgetAloneShowsGetABadge) {
  for (int value = 0; value <= static_cast<int>(PJ::sdk::BuiltinObjectType::kPlotMarkers); ++value) {
    const auto type = static_cast<PJ::sdk::BuiltinObjectType>(value);
    const QString icon = PJ::customWidgetObjectIcon(type);
    EXPECT_EQ(icon.isEmpty(), !PJ::isCustomWidgetObjectType(type)) << value;
    if (!icon.isEmpty()) {
      EXPECT_TRUE(QFile::exists(icon)) << icon.toStdString();
    }
  }
  EXPECT_EQ(PJ::customWidgetObjectIcon(PJ::sdk::BuiltinObjectType::kLog), u":/resources/svg/view_list.svg"_s);
  EXPECT_TRUE(PJ::customWidgetObjectIcon(PJ::sdk::BuiltinObjectType::kPointCloud).isEmpty()) << "a 3D topic";
}

TEST_F(CustomWidgetPagesTest, ConfigPagePaintsOneControlPerSettingType) {
  ASSERT_TRUE(plugins().registerStaticWidget(vtableWithManifest(kGaugeManifest)));
  auto dock = makeDock(u"ui_widgets"_s, u"gauge"_s);
  ASSERT_FALSE(dock->isMissing());
  auto panel = makePanel(dock.get());

  EXPECT_EQ(panel->findChild<QLabel*>(u"labelWidgetName"_s)->text(), u"Gauge"_s);
  EXPECT_EQ(panel->findChild<QLabel*>(u"labelPluginName"_s)->text(), u"from Basic Widgets"_s);
  auto* min = panel->findChild<PJ::DoubleScrubber*>(u"setting_min"_s);
  auto* max = panel->findChild<PJ::DoubleScrubber*>(u"setting_max"_s);
  auto* show = panel->findChild<PJ::ToggleSwitch*>(u"setting_show_value"_s);
  auto* unit = panel->findChild<QLineEdit*>(u"setting_unit"_s);
  auto* needle = panel->findChild<PJ::ColorPickerWidget*>(u"setting_needle"_s);
  auto* mode = panel->findChild<QComboBox*>(u"setting_mode"_s);
  ASSERT_NE(min, nullptr);
  ASSERT_NE(max, nullptr);
  ASSERT_NE(show, nullptr);
  ASSERT_NE(unit, nullptr);
  ASSERT_NE(needle, nullptr);
  ASSERT_NE(mode, nullptr);
  // Defaults fill the controls while the config has no value for the key.
  EXPECT_DOUBLE_EQ(max->value(), 100.0);
  EXPECT_DOUBLE_EQ(max->minimum(), 0.0);
  EXPECT_DOUBLE_EQ(max->maximum(), 1000.0);
  EXPECT_TRUE(show->isChecked());
  EXPECT_EQ(unit->text(), u"V"_s);
  EXPECT_EQ(needle->color(), QColor(u"#e05a47"_s));
  EXPECT_EQ(mode->count(), 2);
  EXPECT_EQ(mode->currentText(), u"arc"_s);
  EXPECT_TRUE(panel->findChild<QLabel*>(u"labelNoSettings"_s)->isHidden());
}

TEST_F(CustomWidgetPagesTest, NumberEditsCoalesceIntoOneApplyWithTheFullJson) {
  ASSERT_TRUE(plugins().registerStaticWidget(vtableWithManifest(kGaugeManifest)));
  auto dock = makeDock(u"ui_widgets"_s, u"gauge"_s);
  dock->applySettings(uR"({"unit":"km","mode":"bar"})"_s);
  auto panel = makePanel(dock.get());
  auto* max = panel->findChild<PJ::DoubleScrubber*>(u"setting_max"_s);
  ASSERT_NE(max, nullptr);
  EXPECT_EQ(panel->findChild<QComboBox*>(u"setting_mode"_s)->currentText(), u"bar"_s) << "stored value wins";

  QSignalSpy workspace(dock.get(), &PJ::CustomWidgetDockWidget::workspaceChanged);
  const int loads_before = g.load_configs;
  max->setValue(250.0);
  max->setValue(260.0);
  max->setValue(270.0);
  EXPECT_EQ(workspace.count(), 0) << "debounced";
  QTest::qWait(PJ::CustomWidgetConfigPanel::kSettingsDebounceMs * 3);
  EXPECT_EQ(workspace.count(), 1);
  EXPECT_EQ(g.load_configs, loads_before + 1);

  const QJsonObject config = QJsonDocument::fromJson(dock->settingsJson().toUtf8()).object();
  EXPECT_DOUBLE_EQ(config.value(u"max"_s).toDouble(), 270.0);
  EXPECT_EQ(config.value(u"unit"_s).toString(), u"km"_s);
  EXPECT_EQ(config.value(u"mode"_s).toString(), u"bar"_s);
  EXPECT_NE(g.last_config.find("270"), std::string::npos) << "the widget received the whole JSON";

  // A non-number edit is not debounced: the switch reports its new state when its slide ends.
  panel->findChild<PJ::ToggleSwitch*>(u"setting_show_value"_s)->toggle();
  EXPECT_TRUE(QTest::qWaitFor([&]() { return workspace.count() == 2; }, 2000));
  EXPECT_FALSE(QJsonDocument::fromJson(dock->settingsJson().toUtf8()).object().value(u"show_value"_s).toBool(true));
}

TEST_F(CustomWidgetPagesTest, IntegerSettingsShowAndStoreWholeNumbers) {
  ASSERT_TRUE(plugins().registerStaticWidget(vtableWithManifest(kIntegerManifest)));
  auto dock = makeDock(u"int_widgets"_s, u"gauge"_s);
  ASSERT_FALSE(dock->isMissing());
  auto panel = makePanel(dock.get());
  auto* decimals = panel->findChild<PJ::DoubleScrubber*>(u"setting_decimals"_s);
  ASSERT_NE(decimals, nullptr);
  EXPECT_EQ(decimals->decimals(), 0);
  EXPECT_DOUBLE_EQ(decimals->singleStep(), 1.0);
  EXPECT_DOUBLE_EQ(decimals->minimum(), 0.0);
  EXPECT_DOUBLE_EQ(decimals->maximum(), 6.0);
  EXPECT_DOUBLE_EQ(decimals->value(), 2.0);

  const int loads_before = g.load_configs;
  decimals->setValue(2.6);  // a drag lands between whole numbers
  QTest::qWait(PJ::CustomWidgetConfigPanel::kSettingsDebounceMs * 3);
  EXPECT_EQ(g.load_configs, loads_before + 1);
  const QString json = dock->settingsJson();
  EXPECT_NE(json.indexOf(u"\"decimals\":3"_s), -1) << json.toStdString();
  EXPECT_NE(g.last_config.find("\"decimals\":3"), std::string::npos) << g.last_config;
}

TEST_F(CustomWidgetPagesTest, InputsOfferChooseOrAddAndTheChooserWiresACompatibleSeries) {
  ASSERT_TRUE(plugins().registerStaticWidget(goodVtable()));
  auto gauge = makeDock(u"test_widgets"_s, u"gauge"_s);
  auto panel = makePanel(gauge.get());
  auto* choose_value =
      withProperty<QPushButton>(panel.get(), "buttonChooseSeries", "pjInputName", QVariant(u"value"_s));
  ASSERT_NE(choose_value, nullptr);
  EXPECT_EQ(choose_value->text(), u"Choose series"_s);
  EXPECT_NE(
      withProperty<QPushButton>(panel.get(), "buttonChooseSeries", "pjInputName", QVariant(u"status"_s)), nullptr);
  EXPECT_EQ(panel->findChild<QPushButton*>(u"buttonAddSeries"_s), nullptr);

  choose_value->click();
  auto* stack = panel->findChild<QStackedWidget*>(u"bodyStack"_s);
  EXPECT_EQ(stack->currentWidget()->objectName(), u"chooserPage"_s);
  auto* tree = panel->findChild<PJ::CurveTreeView*>(u"chooserTree"_s);
  EXPECT_EQ(treeItemWithKey(tree, text_), nullptr) << "a text series does not fit a number input";
  QTreeWidgetItem* leaf = treeItemWithKey(tree, number_);
  ASSERT_NE(leaf, nullptr);
  emit tree->itemActivated(leaf, 0);
  EXPECT_EQ(stack->currentWidget()->objectName(), u"formPage"_s);
  ASSERT_EQ(gauge->bindings().size(), 1U);
  EXPECT_EQ(gauge->bindings()[0].key, number_);

  auto table = makeDock(u"test_widgets"_s, u"table"_s);
  panel->bindDock(table.get());
  auto* add = withProperty<QPushButton>(panel.get(), "buttonAddSeries", "pjInputName", QVariant(u"rows"_s));
  ASSERT_NE(add, nullptr);
  EXPECT_EQ(add->text(), u"Add series"_s);
  EXPECT_EQ(panel->findChild<QPushButton*>(u"buttonChooseSeries"_s), nullptr);
  for (int round = 0; round < 2; ++round) {
    withProperty<QPushButton>(panel.get(), "buttonAddSeries", "pjInputName", QVariant(u"rows"_s))->click();
    emit tree->itemActivated(treeItemWithKey(tree, number_), 0);
  }
  EXPECT_EQ(table->bindings().size(), 2U) << "each add is a new slot";
}

TEST_F(CustomWidgetPagesTest, RemovingAWireCallsRemoveBinding) {
  ASSERT_TRUE(plugins().registerStaticWidget(goodVtable()));
  auto dock = makeDock(u"test_widgets"_s, u"gauge"_s);
  ASSERT_TRUE(dock->setBinding(u"value"_s, 0, number_));
  ASSERT_TRUE(dock->setBinding(u"status"_s, 0, text_));
  auto panel = makePanel(dock.get());
  QList<QToolButton*> trash = panel->findChildren<QToolButton*>(u"buttonRemoveWire"_s);
  ASSERT_EQ(trash.size(), 2);
  auto* remove_status =
      withProperty<QToolButton>(panel.get(), "buttonRemoveWire", "pjInputName", QVariant(u"status"_s));
  ASSERT_NE(remove_status, nullptr);
  remove_status->click();
  ASSERT_EQ(dock->bindings().size(), 1U);
  EXPECT_EQ(dock->bindings()[0].input, u"value"_s);
  QCoreApplication::processEvents();  // the page rebuilds once, from the event loop
  EXPECT_EQ(panel->findChildren<QToolButton*>(u"buttonRemoveWire"_s).size(), 1) << "the page followed the dock";
}

TEST_F(CustomWidgetPagesTest, ADockThatTurnsMissingTakesItsSettingsFormAway) {
  ASSERT_TRUE(plugins().registerStaticWidget(vtableWithManifest(kGaugeManifest)));
  auto dock = makeDock(u"ui_widgets"_s, u"gauge"_s);
  auto panel = makePanel(dock.get());
  ASSERT_NE(panel->findChild<QLineEdit*>(u"setting_unit"_s), nullptr);

  QDomDocument doc;
  ASSERT_TRUE(doc.setContent(uR"(<CustomWidget plugin="absent" type="gauge"/>)"_s));
  ASSERT_TRUE(dock->xmlLoadState(doc.documentElement()));
  QCoreApplication::processEvents();
  EXPECT_TRUE(panel->findChild<QWidget*>(u"settingsHost"_s)->isHidden());
  EXPECT_EQ(panel->findChild<QLineEdit*>(u"setting_unit"_s), nullptr);
}

TEST_F(CustomWidgetPagesTest, InputsChangingLeaveTheSettingBeingEditedInPlace) {
  ASSERT_TRUE(plugins().registerStaticWidget(vtableWithManifest(kGaugeManifest)));
  auto dock = makeDock(u"ui_widgets"_s, u"gauge"_s);
  auto panel = makePanel(dock.get());
  QPointer<QLineEdit> unit = panel->findChild<QLineEdit*>(u"setting_unit"_s);
  ASSERT_NE(unit, nullptr);

  emit dock->inputsChanged();
  QCoreApplication::processEvents();
  EXPECT_NE(unit, nullptr) << "rebuilding the form would take the focus from its editor";
}

TEST_F(CustomWidgetPagesTest, MissingWidgetShowsTheReasonAndItsStoredWiresReadOnly) {
  auto dock = std::make_unique<PJ::CustomWidgetDockWidget>(
      &app_->sessionManager(), &app_->catalogModel(), &app_->extensionCatalog(), &app_->playbackEngine());
  QDomDocument doc;
  ASSERT_TRUE(doc.setContent(
      uR"(<CustomWidget plugin="absent" type="gauge"><input name="value" slot="0" kind="number" dataset_id="9")"
      uR"( dataset_source="gone.csv" topic="/battery" field="voltage"/></CustomWidget>)"_s));
  ASSERT_TRUE(dock->xmlLoadState(doc.documentElement()));
  ASSERT_TRUE(dock->isMissing());
  auto panel = makePanel(dock.get());
  auto* reason = panel->findChild<QLabel*>(u"labelMissingReason"_s);
  EXPECT_FALSE(reason->isHidden());
  EXPECT_EQ(reason->text(), dock->missingReason());
  EXPECT_TRUE(panel->findChild<QWidget*>(u"settingsHost"_s)->isHidden());
  EXPECT_NE(panel->findChild<QWidget*>(u"storedWireRow"_s), nullptr);
  EXPECT_EQ(panel->findChild<QPushButton*>(u"buttonChooseSeries"_s), nullptr);
  EXPECT_EQ(panel->findChild<QToolButton*>(u"buttonRemoveWire"_s), nullptr);
}

// ---------------------------------------------------------------- through the shell

using Peer = PJ::MainWindowLayoutImportTestPeer;

/// Collects the Qt warnings emitted while it lives (one instance at a time).
class WarningCapture {
 public:
  WarningCapture() {
    instance_ = this;
    previous_ = qInstallMessageHandler(&WarningCapture::handle);
  }
  ~WarningCapture() {
    qInstallMessageHandler(previous_);
    instance_ = nullptr;
  }
  WarningCapture(const WarningCapture&) = delete;
  WarningCapture& operator=(const WarningCapture&) = delete;

  [[nodiscard]] QStringList messagesContaining(const QString& needle) const {
    QMutexLocker lock(&mutex_);
    QStringList found;
    for (const QString& message : messages_) {
      if (message.contains(needle)) {
        found << message;
      }
    }
    return found;
  }

 private:
  static void handle(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    if (instance_ != nullptr && (type == QtWarningMsg || type == QtCriticalMsg)) {
      QMutexLocker lock(&instance_->mutex_);
      instance_->messages_ << message;
    }
    if (instance_ != nullptr && instance_->previous_ != nullptr) {
      instance_->previous_(type, context, message);
    }
  }

  inline static WarningCapture* instance_ = nullptr;
  QtMessageHandler previous_ = nullptr;
  mutable QMutex mutex_;
  QStringList messages_;
};

class CustomWidgetShellFlowTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g = pj_cw_test::PluginStats{};
    ASSERT_TRUE(extensions_dir_.isValid());
    window_ = std::make_unique<PJ::MainWindow>(extensions_dir_.path());
    ASSERT_TRUE(Peer::session(*window_).extensionCatalog().pluginCatalog().registerStaticWidget(goodVtable()));
    // A raw registration does not notify; announce it the way a reload does.
    emit Peer::session(*window_).extensionCatalog().catalogChanged();
    auto* tabbed = window_->findChild<PJ::TabbedPlotWidget*>(u"tabbedPlotWidget"_s);
    ASSERT_NE(tabbed, nullptr);
    dock_ = tabbed->currentTab()->plotAt(0);
    ASSERT_NE(dock_, nullptr);
    dock_->setPlaceholderWidget();
    picker_ = window_->findChild<PJ::CustomWidgetPickerMenu*>();
    config_ = window_->findChild<PJ::CustomWidgetConfigPanel*>();
    ASSERT_NE(picker_, nullptr);
    ASSERT_NE(config_, nullptr);
    stack_ = qobject_cast<QStackedWidget*>(config_->parentWidget());
    ASSERT_NE(stack_, nullptr);
  }

  void TearDown() override {
    window_.reset();
  }

  void clickIcon(const char* name) {
    auto* placeholder = dock_->findChild<PJ::VisualizationPlaceholderWidget*>();
    ASSERT_NE(placeholder, nullptr);
    auto* button = placeholder->findChild<QToolButton*>(QString::fromLatin1(name));
    ASSERT_NE(button, nullptr);
    button->click();
  }

  QTemporaryDir extensions_dir_;
  std::unique_ptr<PJ::MainWindow> window_;
  PJ::DockWidget* dock_ = nullptr;
  PJ::CustomWidgetPickerMenu* picker_ = nullptr;
  PJ::CustomWidgetConfigPanel* config_ = nullptr;
  QStackedWidget* stack_ = nullptr;
};

TEST(CustomWidgetShellAvailabilityTest, FifthIconFollowsTheInstalledWidgetCatalog) {
  QTemporaryDir extensions_dir;
  ASSERT_TRUE(extensions_dir.isValid());
  pj_cw_test::g = pj_cw_test::PluginStats{};
  PJ::MainWindow window(extensions_dir.path());
  auto* tabbed = window.findChild<PJ::TabbedPlotWidget*>(u"tabbedPlotWidget"_s);
  ASSERT_NE(tabbed, nullptr);
  PJ::DockWidget* dock = tabbed->currentTab()->plotAt(0);
  ASSERT_NE(dock, nullptr);
  dock->setPlaceholderWidget();
  auto* placeholder = dock->findChild<PJ::VisualizationPlaceholderWidget*>();
  ASSERT_NE(placeholder, nullptr);
  QToolButton* button = placeholder->customWidgetButton();
  ASSERT_NE(button, nullptr);
  EXPECT_FALSE(button->isEnabled());

  PJ::ExtensionCatalogService& extensions = Peer::session(window).extensionCatalog();
  ASSERT_TRUE(extensions.pluginCatalog().registerStaticWidget(goodVtable()));
  emit extensions.catalogChanged();
  EXPECT_TRUE(button->isEnabled());
  EXPECT_EQ(button->toolTip(), u"Custom Widget"_s);

  PJ::DockWidget* split = dock->splitHorizontal();
  ASSERT_NE(split, nullptr);
  auto* split_placeholder = split->findChild<PJ::VisualizationPlaceholderWidget*>();
  ASSERT_NE(split_placeholder, nullptr);
  EXPECT_TRUE(split_placeholder->customWidgetButton()->isEnabled()) << "a new dock starts with the current state";
}

// Docks born after the shell wired their tab (a new tab, a split) take the Custom Widget availability from
// the tab widget (CustomWidgetSupport), and a catalog change reaches every dock of every tab, without Qt
// connection warnings.
TEST(CustomWidgetShellAvailabilityTest, DocksAddedAfterTheTabWasWiredFollowTheCatalogWithoutConnectionWarnings) {
  WarningCapture warnings;
  QTemporaryDir extensions_dir;
  ASSERT_TRUE(extensions_dir.isValid());
  pj_cw_test::g = pj_cw_test::PluginStats{};
  PJ::MainWindow window(extensions_dir.path());
  auto* tabbed = window.findChild<PJ::TabbedPlotWidget*>(u"tabbedPlotWidget"_s);
  ASSERT_NE(tabbed, nullptr);
  PJ::DockWidget* original = tabbed->currentTab()->plotAt(0);
  ASSERT_NE(original, nullptr);
  PJ::PlotDocker* tab = tabbed->addTab(u"Second"_s);
  ASSERT_NE(tab, nullptr);
  PJ::DockWidget* first = tab->plotAt(0);
  ASSERT_NE(first, nullptr);
  first->setPlaceholderWidget();
  PJ::DockWidget* split = first->splitHorizontal();
  ASSERT_NE(split, nullptr);

  const auto customButton = [](PJ::DockWidget* dock) {
    auto* placeholder = dock->findChild<PJ::VisualizationPlaceholderWidget*>();
    return placeholder != nullptr ? placeholder->customWidgetButton() : nullptr;
  };
  ASSERT_NE(customButton(first), nullptr);
  ASSERT_NE(customButton(split), nullptr);
  EXPECT_FALSE(customButton(first)->isEnabled()) << "no widget installed";
  EXPECT_FALSE(customButton(split)->isEnabled()) << "no widget installed";

  PJ::ExtensionCatalogService& extensions = Peer::session(window).extensionCatalog();
  ASSERT_TRUE(extensions.pluginCatalog().registerStaticWidget(goodVtable()));
  emit extensions.catalogChanged();
  EXPECT_TRUE(customButton(first)->isEnabled());
  EXPECT_TRUE(customButton(split)->isEnabled());

  // Docks added once the catalog already lists a widget start enabled, in the new tab and in the first one.
  PJ::DockWidget* later = split->splitVertical();
  ASSERT_NE(later, nullptr);
  ASSERT_NE(customButton(later), nullptr);
  EXPECT_TRUE(customButton(later)->isEnabled()) << "a split of the new tab";
  PJ::DockWidget* original_split = original->splitHorizontal();
  ASSERT_NE(original_split, nullptr);
  original_split->setPlaceholderWidget();
  ASSERT_NE(customButton(original_split), nullptr);
  EXPECT_TRUE(customButton(original_split)->isEnabled()) << "a split of the first tab";

  EXPECT_TRUE(warnings.messagesContaining(u"unique connections require a pointer to member function"_s).isEmpty())
      << warnings.messagesContaining(u"unique connections"_s).join(u" | "_s).toStdString();
}

TEST_F(CustomWidgetShellFlowTest, FifthIconOpensThePickerAndAChoiceBuildsABoundPanel) {
  clickIcon("buttonVizCustomWidget");
  EXPECT_EQ(dock_->objectWidget(), nullptr) << "nothing is built before the choice";
  EXPECT_TRUE(picker_->isVisible()) << "the menu opens under the button";

  QAction* gauge = entryNamed(picker_, u"Gauge"_s);
  ASSERT_NE(gauge, nullptr);
  pick(picker_, u"Gauge"_s);

  ASSERT_NE(dock_->objectWidget(), nullptr);
  auto* custom = qobject_cast<PJ::CustomWidgetDockWidget*>(dock_->objectWidget()->widget());
  ASSERT_NE(custom, nullptr);
  EXPECT_FALSE(custom->isMissing());
  EXPECT_EQ(custom->pluginId(), u"test_widgets"_s);
  EXPECT_EQ(custom->widgetType(), u"gauge"_s);
  EXPECT_EQ(g.widgets_created, 1);
  EXPECT_EQ(stack_->currentWidget(), config_);
  EXPECT_EQ(config_->dock(), custom);

  gauge->trigger();
  EXPECT_EQ(g.widgets_created, 1) << "the choice is spent";
}

TEST_F(CustomWidgetShellFlowTest, ThePanelsWidgetGetsThePlaybackServiceWithTheShellsTimeMappers) {
  PJ::AppSession& app = Peer::session(*window_);
  app.sessionManager().setUseTimeOffset(false);
  const PJ::DatasetId dataset = pj_test::createDataset(app, "mapped");
  ASSERT_NE(pj_test::addScalarTopic(app, dataset, "/signal", 0, 10'000'000'000), 0U);

  clickIcon("buttonVizCustomWidget");
  pick(picker_, u"Gauge"_s);
  ASSERT_NE(dock_->objectWidget(), nullptr);
  ASSERT_TRUE(g.offered_playback);
  ASSERT_TRUE(g.playback.valid());

  const auto display = g.playback.toDisplayTime("/signal", 3'000'000'000);
  ASSERT_TRUE(display.has_value()) << display.error();
  EXPECT_DOUBLE_EQ(*display, 3.0);
  EXPECT_FALSE(g.playback.toDisplayTime("/nowhere", 0).has_value());
  const auto by_source =
      g.playback.toDisplayTimeForSource(PJ::sdk::DataSourceHandle{static_cast<uint32_t>(dataset)}, 4'000'000'000);
  ASSERT_TRUE(by_source.has_value()) << by_source.error();
  EXPECT_DOUBLE_EQ(*by_source, 4.0);
}

TEST_F(CustomWidgetShellFlowTest, AMeshTopicDropWiresACustomWidgetAndAnEmptyDockSaysWhereItGoes) {
  PJ::AppSession& app = Peer::session(*window_);
  const PJ::DatasetId dataset = pj_test::createDataset(app, "robot.mcap");
  auto topic = app.sessionManager().objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = dataset, .topic_name = "/model", .metadata_json = R"({"builtin_object_type":"kMesh3D"})"});
  ASSERT_TRUE(topic.has_value()) << topic.error();
  app.catalogModel().rebuildFromDatastore();
  QString key;
  for (const PJ::CatalogItem& item : app.catalogModel().items()) {
    if (item.topic_name == u"/model"_s) {
      key = item.key;
    }
  }
  ASSERT_FALSE(key.isEmpty());
  const auto drop = [&key](PJ::DockWidget* target) {
    return QMetaObject::invokeMethod(
        target, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{key}));
  };
  const auto toasts = [this]() {
    QStringList messages;
    for (const auto* toast : window_->findChildren<PJ::ToastNotification*>()) {
      messages << toast->message();
    }
    return messages;
  };

  ASSERT_TRUE(drop(dock_));
  EXPECT_EQ(dock_->objectWidget(), nullptr) << "no built-in view takes a mesh asset";
  EXPECT_TRUE(toasts().contains(u"This topic is shown by a Custom Widget. Create one in this panel first."_s))
      << toasts().join(u" | "_s).toStdString();

  clickIcon("buttonVizCustomWidget");
  pick(picker_, u"Viewer"_s);
  ASSERT_NE(dock_->objectWidget(), nullptr);
  auto* custom = qobject_cast<PJ::CustomWidgetDockWidget*>(dock_->objectWidget()->widget());
  ASSERT_NE(custom, nullptr);
  ASSERT_TRUE(custom->bindings().empty());

  ASSERT_TRUE(drop(dock_));
  ASSERT_EQ(custom->bindings().size(), 1U);
  EXPECT_EQ(custom->bindings()[0].label, "/model");
  EXPECT_EQ(dock_->objectWidget()->widget(), custom) << "the panel took the topic in place";
}

TEST_F(CustomWidgetShellFlowTest, AMeshDroppedWithAScalarOnACustomWidgetWiresBoth) {
  PJ::AppSession& app = Peer::session(*window_);
  const PJ::DatasetId dataset = pj_test::createDataset(app, "robot.mcap");
  auto topic = app.sessionManager().objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = dataset, .topic_name = "/model", .metadata_json = R"({"builtin_object_type":"kMesh3D"})"});
  ASSERT_TRUE(topic.has_value()) << topic.error();
  ASSERT_NE(pj_test::addScalarTopic(app, dataset, "/signal", 0, 10'000'000'000), 0U);
  app.catalogModel().rebuildFromDatastore();
  QString mesh_key;
  QString scalar_key;
  for (const PJ::CatalogItem& item : app.catalogModel().items()) {
    if (item.topic_name == u"/model"_s) {
      mesh_key = item.key;
    } else if (app.catalogModel().curveDescriptor(item.key).has_value()) {
      scalar_key = item.key;
    }
  }
  ASSERT_FALSE(mesh_key.isEmpty());
  ASSERT_FALSE(scalar_key.isEmpty());

  clickIcon("buttonVizCustomWidget");
  pick(picker_, u"Viewer"_s);
  ASSERT_NE(dock_->objectWidget(), nullptr);
  auto* custom = qobject_cast<PJ::CustomWidgetDockWidget*>(dock_->objectWidget()->widget());
  ASSERT_NE(custom, nullptr);
  ASSERT_TRUE(custom->applySettings(u"{\"with_value\":true}"_s));

  // The order a curve-list drag delivers: the object row sorts ahead of the scalar field.
  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          dock_, "onCatalogItemsDropped", Qt::DirectConnection,
          Q_ARG(QStringList, (QStringList{mesh_key, scalar_key}))));
  ASSERT_EQ(custom->bindings().size(), 2U) << "the scalar of a drop led by a mesh topic was not offered";
  EXPECT_EQ(custom->bindings()[0].label, "/model");
  EXPECT_EQ(custom->bindings()[1].input, u"value"_s);
}

TEST_F(CustomWidgetShellFlowTest, AMeshTopicDroppedWithScalarsOnAnEmptyDockStillPlotsTheScalars) {
  PJ::AppSession& app = Peer::session(*window_);
  const PJ::DatasetId dataset = pj_test::createDataset(app, "robot.mcap");
  auto topic = app.sessionManager().objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = dataset, .topic_name = "/model", .metadata_json = R"({"builtin_object_type":"kMesh3D"})"});
  ASSERT_TRUE(topic.has_value()) << topic.error();
  ASSERT_NE(pj_test::addScalarTopic(app, dataset, "/signal", 0, 10'000'000'000), 0U);
  app.catalogModel().rebuildFromDatastore();
  QString mesh_key;
  QString scalar_key;
  for (const PJ::CatalogItem& item : app.catalogModel().items()) {
    if (item.topic_name == u"/model"_s) {
      mesh_key = item.key;
    } else if (app.catalogModel().curveDescriptor(item.key).has_value()) {
      scalar_key = item.key;
    }
  }
  ASSERT_FALSE(mesh_key.isEmpty());
  ASSERT_FALSE(scalar_key.isEmpty());

  // The order a curve-list drag delivers: the object row sorts ahead of the scalar field.
  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          dock_, "onCatalogItemsDropped", Qt::DirectConnection,
          Q_ARG(QStringList, (QStringList{mesh_key, scalar_key}))));
  ASSERT_NE(dock_->plotWidget(), nullptr) << "the scalar of a drop that also carried a mesh was not plotted";
  EXPECT_FALSE(dock_->plotWidget()->isEmpty());
}

TEST_F(CustomWidgetShellFlowTest, AnAdvertisedMeshDroppedWithScalarsOnAnEmptyDockStillPlotsTheScalars) {
  PJ::AppSession& app = Peer::session(*window_);
  const PJ::DatasetId dataset = pj_test::createDataset(app, "live");
  ASSERT_NE(pj_test::addScalarTopic(app, dataset, "/signal", 0, 10'000'000'000), 0U);
  app.catalogModel().rebuildFromDatastore();
  // A streaming source's unsubscribed /model: a catalog row with no data yet.
  app.catalogModel().setAdvertisedTopics(
      dataset, {PJ::AdvertisedTopic{u"/model"_s, PJ::sdk::BuiltinObjectType::kMesh3D}});
  QString mesh_key;
  QString scalar_key;
  for (const PJ::CatalogItem& item : app.catalogModel().items()) {
    if (item.topic_name == u"/model"_s) {
      mesh_key = item.key;
    } else if (app.catalogModel().curveDescriptor(item.key).has_value()) {
      scalar_key = item.key;
    }
  }
  ASSERT_FALSE(mesh_key.isEmpty());
  ASSERT_FALSE(scalar_key.isEmpty());

  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          dock_, "onCatalogItemsDropped", Qt::DirectConnection,
          Q_ARG(QStringList, (QStringList{mesh_key, scalar_key}))));
  ASSERT_NE(dock_->plotWidget(), nullptr)
      << "the scalar of a drop that also carried an advertised mesh was not plotted";
  EXPECT_FALSE(dock_->plotWidget()->isEmpty());
}

TEST_F(CustomWidgetShellFlowTest, AChoiceForAPanelFilledMeanwhileDoesNothing) {
  clickIcon("buttonVizCustomWidget");
  clickIcon("buttonVizPlot");
  ASSERT_NE(dock_->plotWidget(), nullptr);
  pick(picker_, u"Gauge"_s);
  EXPECT_EQ(dock_->objectWidget(), nullptr);
  EXPECT_EQ(g.widgets_created, 0);
}

// A choice makes the new panel's config page the side panel's page and focuses that panel.
TEST_F(CustomWidgetShellFlowTest, AChoiceShowsTheNewPanelsPageAndFocusesIt) {
  clickIcon("buttonVizPlot");
  ASSERT_NE(dock_->plotWidget(), nullptr);
  PJ::DockWidget* plot_dock = dock_;
  dock_ = plot_dock->splitHorizontal();
  ASSERT_NE(dock_, nullptr);
  auto* docker = window_->findChild<PJ::TabbedPlotWidget*>(u"tabbedPlotWidget"_s)->currentTab();
  window_->show();
  window_->activateWindow();
  ASSERT_TRUE(QTest::qWaitForWindowActive(window_.get()));
  docker->focusDock(plot_dock);

  clickIcon("buttonVizCustomWidget");
  pick(picker_, u"Gauge"_s);
  QApplication::processEvents();

  ASSERT_NE(dock_->objectWidget(), nullptr);
  EXPECT_EQ(stack_->currentWidget(), config_);
  EXPECT_EQ(docker->focusedDock(), dock_);
}

// Dark-theme captures of both pages inside the real window (saved when PJ_UI_TEST_DUMP_DIR is set).
class CustomWidgetCaptureTest : public CustomWidgetShellFlowTest {
 protected:
  void SetUp() override {
    applyDarkTheme();  // before the window reads the theme
    CustomWidgetShellFlowTest::SetUp();
    ASSERT_TRUE(
        Peer::session(*window_).extensionCatalog().pluginCatalog().registerStaticWidget(
            vtableWithManifest(kGaugeManifest)));
    ASSERT_TRUE(Peer::session(*window_).extensionCatalog().pluginCatalog().registerStaticWidget(otherQtVtable()));
  }
};

TEST_F(CustomWidgetCaptureTest, PickerAndConfigPage) {
  PJ::AppSession& app = Peer::session(*window_);
  const PJ::DatasetId dataset = pj_cw_test::createDataset(app, "robot.mcap");
  const QString voltage = pj_cw_test::addNumberTopic(app, dataset, "/battery/voltage", {{1.0, 12.1}, {2.0, 12.6}});
  ASSERT_FALSE(voltage.isEmpty());
  window_->resize(1400, 820);
  window_->show();
  QCoreApplication::processEvents();

  dump(dock_, u"s4-placeholder.png"_s);
  clickIcon("buttonVizCustomWidget");
  QWidget* side_panel = stack_->parentWidget();
  ASSERT_TRUE(picker_->isVisible());

  pick(picker_, u"Gauge"_s);
  ASSERT_EQ(stack_->currentWidget(), config_);
  if (side_panel->isHidden()) {  // the menu leaves the side panel as it was
    window_->findChild<PJ::TabbedPlotWidget*>(u"tabbedPlotWidget"_s)->rightPanelButton()->click();
  }
  ASSERT_NE(config_->dock(), nullptr);
  ASSERT_TRUE(config_->dock()->setBinding(u"value"_s, 0, voltage));
  QCoreApplication::processEvents();
  dump(side_panel, u"s4-config-panel.png"_s);
  dump(window_.get(), u"s4-window.png"_s);
}

}  // namespace
