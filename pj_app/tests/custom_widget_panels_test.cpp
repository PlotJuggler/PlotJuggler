// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The Custom Widget side-panel pages: the picker of installed widget types, the per-panel config
// page (inputs, wires, settings form), and the shell flow from the fifth placeholder icon to a
// bound panel. Set PJ_UI_TEST_DUMP_DIR to also save dark-theme captures of both pages.

#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
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
#include "support/gui_test_env.h"
#include "support/layout_import_gui_support.h"
#include "support/loader_test_support.h"
#include "ui/CustomWidgetConfigPanel.h"
#include "ui/CustomWidgetPickerPanel.h"

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

// Nine types: past the picker's filter threshold.
constexpr const char* kManyManifest =
    R"({"id":"many_widgets","name":"Many","version":"1.0.0","widgets":[)"
    R"({"type":"w1","name":"W1"},{"type":"w2","name":"W2"},{"type":"w3","name":"W3"},)"
    R"({"type":"w4","name":"W4"},{"type":"w5","name":"W5"},{"type":"w6","name":"W6"},)"
    R"({"type":"w7","name":"W7"},{"type":"w8","name":"W8"},{"type":"w9","name":"Nine"}]})";

const PJ_widget_vtable_t* vtableWithManifest(const char* manifest) {
  // One static copy per manifest literal; the catalog keeps the pointer.
  static std::map<const char*, PJ_widget_vtable_t> copies;
  auto [it, inserted] = copies.try_emplace(manifest, *goodVtable());
  it->second.manifest_json = manifest;
  return &it->second;
}

QListWidgetItem* itemNamed(QListWidget* list, const QString& name) {
  for (int row = 0; row < list->count(); ++row) {
    if (list->item(row)->text().startsWith(name + u'\n')) {
      return list->item(row);
    }
  }
  return nullptr;
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
  PJ::CustomWidgetPickerPanel picker;
  picker.setCatalog(&app_->extensionCatalog());

  auto* list = picker.findChild<QListWidget*>(u"listCustomWidgets"_s);
  ASSERT_NE(list, nullptr);
  EXPECT_EQ(list->count(), 4);  // gauge, table, viewer + the other-Qt gauge
  EXPECT_FALSE(list->isHidden());
  EXPECT_TRUE(picker.findChild<QWidget*>(u"emptyState"_s)->isHidden());
  EXPECT_TRUE(picker.findChild<PJ::SectionHeaderBand*>(u"pickerBand"_s)->filterSearch()->isHidden())
      << "four rows need no filter";

  int disabled = 0;
  for (int row = 0; row < list->count(); ++row) {
    QListWidgetItem* item = list->item(row);
    EXPECT_FALSE(item->icon().isNull());
    if (!item->flags().testFlag(Qt::ItemIsEnabled)) {
      ++disabled;
      EXPECT_TRUE(item->text().contains(u"from Other Qt"_s));
      EXPECT_FALSE(item->toolTip().isEmpty()) << "the Qt mismatch reason";
    }
  }
  EXPECT_EQ(disabled, 1);

  QSignalSpy chosen(&picker, &PJ::CustomWidgetPickerPanel::widgetChosen);
  QListWidgetItem* table = itemNamed(list, u"Table"_s);
  ASSERT_NE(table, nullptr);
  EXPECT_TRUE(table->text().contains(u"from Test Widgets"_s));
  emit list->itemClicked(table);
  ASSERT_EQ(chosen.count(), 1);
  EXPECT_EQ(chosen.at(0).at(0).toString(), u"test_widgets"_s);
  EXPECT_EQ(chosen.at(0).at(1).toString(), u"table"_s);

  for (int row = 0; row < list->count(); ++row) {
    if (!list->item(row)->flags().testFlag(Qt::ItemIsEnabled)) {
      emit list->itemClicked(list->item(row));
    }
  }
  EXPECT_EQ(chosen.count(), 1) << "a disabled row is not a choice";
}

TEST_F(CustomWidgetPagesTest, PickerWithNothingInstalledOffersTheMarketplace) {
  PJ::CustomWidgetPickerPanel picker;
  picker.setCatalog(&app_->extensionCatalog());
  EXPECT_TRUE(picker.findChild<QListWidget*>(u"listCustomWidgets"_s)->isHidden());
  EXPECT_FALSE(picker.findChild<QWidget*>(u"emptyState"_s)->isHidden());
  auto* label = picker.findChild<QLabel*>(u"labelNoCustomWidgets"_s);
  ASSERT_NE(label, nullptr);
  EXPECT_EQ(label->text(), u"No custom widgets are installed."_s);
  auto* button = picker.findChild<QPushButton*>(u"buttonOpenMarketplace"_s);
  ASSERT_NE(button, nullptr);
  EXPECT_FALSE(button->isHidden());
  QSignalSpy marketplace(&picker, &PJ::CustomWidgetPickerPanel::marketplaceRequested);
  button->click();
  EXPECT_EQ(marketplace.count(), 1);
}

TEST_F(CustomWidgetPagesTest, PickerShowsTheFilterPastTheThresholdAndFilters) {
  ASSERT_TRUE(plugins().registerStaticWidget(vtableWithManifest(kManyManifest)));
  PJ::CustomWidgetPickerPanel picker;
  picker.setCatalog(&app_->extensionCatalog());
  auto* list = picker.findChild<QListWidget*>(u"listCustomWidgets"_s);
  ASSERT_EQ(list->count(), 9);
  PJ::Search* search = picker.findChild<PJ::SectionHeaderBand*>(u"pickerBand"_s)->filterSearch();
  ASSERT_NE(search, nullptr);
  EXPECT_FALSE(search->isHidden());
  search->setText(u"nine"_s);
  int shown = 0;
  for (int row = 0; row < list->count(); ++row) {
    shown += list->item(row)->isHidden() ? 0 : 1;
  }
  EXPECT_EQ(shown, 1);
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
    picker_ = window_->findChild<PJ::CustomWidgetPickerPanel*>();
    config_ = window_->findChild<PJ::CustomWidgetConfigPanel*>();
    ASSERT_NE(picker_, nullptr);
    ASSERT_NE(config_, nullptr);
    stack_ = qobject_cast<QStackedWidget*>(picker_->parentWidget());
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
  PJ::CustomWidgetPickerPanel* picker_ = nullptr;
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
// the tab widget (CustomWidgetSupport), and a catalog change reaches every dock of every tab. An earlier
// per-dock lambda connection was refused by Qt with a warning, and those docks kept the default.
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
  EXPECT_EQ(stack_->currentWidget(), picker_);
  EXPECT_FALSE(stack_->parentWidget()->isHidden()) << "the right panel opens";

  auto* list = picker_->findChild<QListWidget*>(u"listCustomWidgets"_s);
  QListWidgetItem* gauge = itemNamed(list, u"Gauge"_s);
  ASSERT_NE(gauge, nullptr);
  emit list->itemClicked(gauge);

  ASSERT_NE(dock_->objectWidget(), nullptr);
  auto* custom = qobject_cast<PJ::CustomWidgetDockWidget*>(dock_->objectWidget()->widget());
  ASSERT_NE(custom, nullptr);
  EXPECT_FALSE(custom->isMissing());
  EXPECT_EQ(custom->pluginId(), u"test_widgets"_s);
  EXPECT_EQ(custom->widgetType(), u"gauge"_s);
  EXPECT_EQ(g.widgets_created, 1);
  EXPECT_EQ(stack_->currentWidget(), config_);
  EXPECT_EQ(config_->dock(), custom);

  emit list->itemClicked(gauge);
  EXPECT_EQ(g.widgets_created, 1) << "the choice is spent";
}

TEST_F(CustomWidgetShellFlowTest, ThePanelsWidgetGetsThePlaybackServiceWithTheShellsTimeMappers) {
  PJ::AppSession& app = Peer::session(*window_);
  app.sessionManager().setUseTimeOffset(false);
  const PJ::DatasetId dataset = pj_test::createDataset(app, "mapped");
  ASSERT_NE(pj_test::addScalarTopic(app, dataset, "/signal", 0, 10'000'000'000), 0U);

  clickIcon("buttonVizCustomWidget");
  auto* list = picker_->findChild<QListWidget*>(u"listCustomWidgets"_s);
  emit list->itemClicked(itemNamed(list, u"Gauge"_s));
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

TEST_F(CustomWidgetShellFlowTest, ALogTopicDropWiresACustomWidgetAndAnEmptyDockSaysWhereItGoes) {
  PJ::AppSession& app = Peer::session(*window_);
  const PJ::DatasetId dataset = pj_test::createDataset(app, "robot.mcap");
  auto topic = app.sessionManager().objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = dataset, .topic_name = "/rosout", .metadata_json = R"({"builtin_object_type":"kLog"})"});
  ASSERT_TRUE(topic.has_value()) << topic.error();
  app.catalogModel().rebuildFromDatastore();
  QString key;
  for (const PJ::CatalogItem& item : app.catalogModel().items()) {
    if (item.topic_name == u"/rosout"_s) {
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
  EXPECT_EQ(dock_->objectWidget(), nullptr) << "no built-in view takes a log";
  EXPECT_TRUE(toasts().contains(u"This topic is shown by a Custom Widget. Create one in this panel first."_s))
      << toasts().join(u" | "_s).toStdString();

  clickIcon("buttonVizCustomWidget");
  auto* list = picker_->findChild<QListWidget*>(u"listCustomWidgets"_s);
  emit list->itemClicked(itemNamed(list, u"Viewer"_s));
  ASSERT_NE(dock_->objectWidget(), nullptr);
  auto* custom = qobject_cast<PJ::CustomWidgetDockWidget*>(dock_->objectWidget()->widget());
  ASSERT_NE(custom, nullptr);
  ASSERT_TRUE(custom->bindings().empty());

  ASSERT_TRUE(drop(dock_));
  ASSERT_EQ(custom->bindings().size(), 1U);
  EXPECT_EQ(custom->bindings()[0].label, "/rosout");
  EXPECT_EQ(dock_->objectWidget()->widget(), custom) << "the panel took the topic in place";
}

TEST_F(CustomWidgetShellFlowTest, ALogTopicDroppedWithScalarsOnAnEmptyDockStillPlotsTheScalars) {
  PJ::AppSession& app = Peer::session(*window_);
  const PJ::DatasetId dataset = pj_test::createDataset(app, "robot.mcap");
  auto topic = app.sessionManager().objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = dataset, .topic_name = "/rosout", .metadata_json = R"({"builtin_object_type":"kLog"})"});
  ASSERT_TRUE(topic.has_value()) << topic.error();
  ASSERT_NE(pj_test::addScalarTopic(app, dataset, "/signal", 0, 10'000'000'000), 0U);
  app.catalogModel().rebuildFromDatastore();
  QString log_key;
  QString scalar_key;
  for (const PJ::CatalogItem& item : app.catalogModel().items()) {
    if (item.topic_name == u"/rosout"_s) {
      log_key = item.key;
    } else if (app.catalogModel().curveDescriptor(item.key).has_value()) {
      scalar_key = item.key;
    }
  }
  ASSERT_FALSE(log_key.isEmpty());
  ASSERT_FALSE(scalar_key.isEmpty());

  // The order a curve-list drag delivers: the object row sorts ahead of the scalar field.
  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          dock_, "onCatalogItemsDropped", Qt::DirectConnection,
          Q_ARG(QStringList, (QStringList{log_key, scalar_key}))));
  ASSERT_NE(dock_->plotWidget(), nullptr) << "the scalar of a drop that also carried a log was not plotted";
  EXPECT_FALSE(dock_->plotWidget()->isEmpty());
}

TEST_F(CustomWidgetShellFlowTest, AnAdvertisedLogDroppedWithScalarsOnAnEmptyDockStillPlotsTheScalars) {
  PJ::AppSession& app = Peer::session(*window_);
  const PJ::DatasetId dataset = pj_test::createDataset(app, "live");
  ASSERT_NE(pj_test::addScalarTopic(app, dataset, "/signal", 0, 10'000'000'000), 0U);
  app.catalogModel().rebuildFromDatastore();
  // A streaming source's unsubscribed /rosout: a catalog row with no data yet.
  app.catalogModel().setAdvertisedTopics(
      dataset, {PJ::AdvertisedTopic{u"/rosout"_s, PJ::sdk::BuiltinObjectType::kLog}});
  QString log_key;
  QString scalar_key;
  for (const PJ::CatalogItem& item : app.catalogModel().items()) {
    if (item.topic_name == u"/rosout"_s) {
      log_key = item.key;
    } else if (app.catalogModel().curveDescriptor(item.key).has_value()) {
      scalar_key = item.key;
    }
  }
  ASSERT_FALSE(log_key.isEmpty());
  ASSERT_FALSE(scalar_key.isEmpty());

  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          dock_, "onCatalogItemsDropped", Qt::DirectConnection,
          Q_ARG(QStringList, (QStringList{log_key, scalar_key}))));
  ASSERT_NE(dock_->plotWidget(), nullptr) << "the scalar of a drop that also carried an advertised log was not plotted";
  EXPECT_FALSE(dock_->plotWidget()->isEmpty());
}

TEST_F(CustomWidgetShellFlowTest, AChoiceForAPanelFilledMeanwhileDoesNothing) {
  clickIcon("buttonVizCustomWidget");
  clickIcon("buttonVizPlot");
  ASSERT_NE(dock_->plotWidget(), nullptr);
  auto* list = picker_->findChild<QListWidget*>(u"listCustomWidgets"_s);
  emit list->itemClicked(itemNamed(list, u"Gauge"_s));
  EXPECT_EQ(dock_->objectWidget(), nullptr);
  EXPECT_EQ(g.widgets_created, 0);
}

// Choosing a type while the picker's list holds the keyboard focus: switching the side panel away from
// the picker hides the list, and the focus it gives up must not land on a sibling dock and pull that
// dock's page in mid-switch. One page shows, and the new panel is the focused dock.
TEST_F(CustomWidgetShellFlowTest, AChoiceLeavesOnlyTheNewPanelsPageShowingAndFocusesIt) {
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
  auto* list = picker_->findChild<QListWidget*>(u"listCustomWidgets"_s);
  list->setFocus();
  QApplication::processEvents();
  ASSERT_TRUE(list->hasFocus()) << "the case under test: the list keeps the focus the click gave it";
  emit list->itemClicked(itemNamed(list, u"Gauge"_s));
  QApplication::processEvents();

  ASSERT_NE(dock_->objectWidget(), nullptr);
  QStringList showing;  // the side panel's pages: the plot page, the picker, the config page, …
  for (int index = 0; index < stack_->count(); ++index) {
    if (stack_->widget(index)->isVisible()) {
      showing << QString::fromLatin1(stack_->widget(index)->metaObject()->className());
    }
  }
  EXPECT_EQ(showing.size(), 1) << showing.join(u", "_s).toStdString();
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
  ASSERT_EQ(stack_->currentWidget(), picker_);
  QCoreApplication::processEvents();
  dump(side_panel, u"s4-picker.png"_s);

  auto* list = picker_->findChild<QListWidget*>(u"listCustomWidgets"_s);
  QListWidgetItem* gauge = nullptr;
  for (int row = 0; row < list->count(); ++row) {
    if (list->item(row)->text() == u"Gauge\nfrom Basic Widgets"_s) {
      gauge = list->item(row);
    }
  }
  ASSERT_NE(gauge, nullptr);
  emit list->itemClicked(gauge);
  ASSERT_EQ(stack_->currentWidget(), config_);
  ASSERT_NE(config_->dock(), nullptr);
  ASSERT_TRUE(config_->dock()->setBinding(u"value"_s, 0, voltage));
  QCoreApplication::processEvents();
  dump(side_panel, u"s4-config-panel.png"_s);
  dump(window_.get(), u"s4-window.png"_s);
}

}  // namespace
