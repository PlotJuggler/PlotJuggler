// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// CustomWidgetDockWidget against the in-process Widget plugin of in_process_widget_plugin.h.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPointer>
#include <QTemporaryDir>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "dock_test_support.h"
#include "in_process_widget_plugin.h"
#include "pj_custom_widgets/CustomWidgetDockWidget.h"
#include "pj_custom_widgets/MissingWidgetPlaceholder.h"
#include "pj_runtime/ActiveWidgetsMarker.h"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_widgets/MessageBox.h"

using namespace Qt::StringLiterals;

namespace PJ {
namespace {

using pj_cw_test::faultyVtable;
using pj_cw_test::g;
using pj_cw_test::goodVtable;
using pj_cw_test::kSecond;
using pj_cw_test::otherQtVtable;
using pj_cw_test::PluginStats;

class CustomWidgetDockTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g = PluginStats{};
    app_ = std::make_unique<AppSession>();
    auto& plugins = app_->extensionCatalog().pluginCatalog();
    ASSERT_TRUE(plugins.registerStaticWidget(goodVtable()));
    ASSERT_TRUE(plugins.registerStaticWidget(otherQtVtable()));
    ASSERT_TRUE(plugins.registerStaticWidget(faultyVtable()));
    dataset_ = pj_cw_test::createDataset(*app_, "robot.mcap");
    number_ = pj_cw_test::addNumberTopic(*app_, dataset_, "/speed", {{1.0, 10.0}, {2.0, 20.0}, {3.0, 30.0}});
    text_ = pj_cw_test::addTextTopic(*app_, dataset_, "/mode", {{1.0, "IDLE"}, {2.5, "ARMED"}});
    ASSERT_FALSE(number_.isEmpty());
    ASSERT_FALSE(text_.isEmpty());
  }

  std::unique_ptr<CustomWidgetDockWidget> makeDock() {
    return std::make_unique<CustomWidgetDockWidget>(
        &app_->sessionManager(), &app_->catalogModel(), &app_->extensionCatalog(), &app_->playbackEngine());
  }

  std::unique_ptr<CustomWidgetDockWidget> makeBound(const QString& type = u"gauge"_s) {
    auto dock = makeDock();
    EXPECT_TRUE(dock->bindToWidget(u"test_widgets"_s, type));
    return dock;
  }

  /// Records every diagnostic the dock reports.
  void captureDiagnostics(CustomWidgetDockWidget& dock) {
    dock.setDiagnosticSink([this](const Diagnostic& d) { diagnostics_.push_back(d); });
  }

  [[nodiscard]] bool reported(DiagnosticLevel level, const std::string& needle) const {
    return std::ranges::any_of(diagnostics_, [&](const Diagnostic& d) {
      return d.level == level && d.message.find(needle) != std::string::npos;
    });
  }

  static QJsonObject parseJson(const std::string& json) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(json)).object();
  }

  static QDomElement parseElement(QDomDocument& doc, const QString& xml) {
    EXPECT_TRUE(doc.setContent(xml));
    return doc.documentElement();
  }

  std::unique_ptr<AppSession> app_;
  std::vector<Diagnostic> diagnostics_;
  DatasetId dataset_ = 0;
  QString number_;
  QString text_;
};

TEST_F(CustomWidgetDockTest, BindCreatesTheWidgetUnderTheDockAndReadsItsInputs) {
  auto dock = makeBound();
  ASSERT_FALSE(dock->isMissing());
  EXPECT_EQ(g.widgets_created, 1);
  QLabel* label = dock->findChild<QLabel*>();
  ASSERT_NE(label, nullptr);
  EXPECT_TRUE(dock->isAncestorOf(label));
  ASSERT_EQ(dock->inputs().size(), 2U);
  EXPECT_EQ(dock->inputs()[0].name, "value");
  EXPECT_EQ(dock->inputs()[1].kind, WidgetInputKind::kText);
  EXPECT_EQ(dock->widgetName(), u"Gauge"_s);
  EXPECT_TRUE(std::ranges::any_of(dock->settingsDeclarations(), [](const SettingDecl& setting) {
    return setting.key == u"unit"_s;
  }));
}

TEST_F(CustomWidgetDockTest, AFreshPanelReceivesItsFirstUpdateBeforeAnyWire) {
  auto dock = makeBound();
  EXPECT_EQ(g.updates, 1);
  EXPECT_TRUE(g.last_values.empty());
}

TEST_F(CustomWidgetDockTest, OnUpdateRunsOnlyWhenAWiredSampleChanges) {
  auto dock = makeBound();
  ASSERT_TRUE(dock->setBinding(u"value"_s, 0, number_));
  const int base = g.updates;

  dock->onTrackerTime(1.0);
  EXPECT_EQ(g.updates, base + 1);
  ASSERT_EQ(g.last_values.size(), 1U);
  EXPECT_EQ(g.last_values[0].kind, PJ_WIDGET_VALUE_NUMBER);
  EXPECT_DOUBLE_EQ(g.last_values[0].number, 10.0);
  EXPECT_DOUBLE_EQ(g.last_values[0].time, 1.0);
  EXPECT_EQ(g.last_values[0].label, "/speed/value");
  EXPECT_EQ(g.last_values[0].input, 0U);

  dock->onTrackerTime(1.0);
  dock->onTrackerTime(1.6);
  dock->onTrackerTime(1.99);
  EXPECT_EQ(g.updates, base + 1) << "the held sample did not change";

  dock->onTrackerTime(2.0);
  EXPECT_EQ(g.updates, base + 2);
  EXPECT_DOUBLE_EQ(g.last_values[0].number, 20.0);

  dock->onTrackerTime(0.5);  // before the first sample: the wire goes to NONE, which is a change
  EXPECT_EQ(g.updates, base + 3);
  EXPECT_EQ(g.last_values[0].kind, PJ_WIDGET_VALUE_NONE);
  EXPECT_EQ(g.last_values[0].label, "/speed/value") << "NONE still names the wire";
}

TEST_F(CustomWidgetDockTest, TextAndNumberWiresArriveInOneCall) {
  auto dock = makeBound();
  ASSERT_TRUE(dock->setBinding(u"value"_s, 0, number_));
  ASSERT_TRUE(dock->setBinding(u"status"_s, 0, text_));
  dock->onTrackerTime(2.7);
  ASSERT_EQ(g.last_values.size(), 2U);
  EXPECT_EQ(g.last_values[1].kind, PJ_WIDGET_VALUE_TEXT);
  EXPECT_EQ(g.last_values[1].text, "ARMED");
  EXPECT_EQ(g.last_values[1].input, 1U);
  EXPECT_DOUBLE_EQ(g.last_values[1].time, 2.5);
}

TEST_F(CustomWidgetDockTest, OnlyTheOffsetOfAWiredDatasetReachesTheWidget) {
  // A display offset lives on a time domain, and the default domain carries none.
  const auto datasetWithOwnDomain = [&](const char* name) -> DatasetId {
    DataEngine& engine = app_->sessionManager().dataEngine();
    auto domain = engine.createTimeDomain(name);
    EXPECT_TRUE(domain.has_value());
    auto dataset = engine.createDataset(DatasetDescriptor{.source_name = name, .time_domain_id = *domain});
    EXPECT_TRUE(dataset.has_value());
    return *dataset;
  };
  const DatasetId wired = datasetWithOwnDomain("wired.mcap");
  const DatasetId other = datasetWithOwnDomain("other.mcap");
  const QString key = pj_cw_test::addNumberTopic(*app_, wired, "/wired", {{1.0, 10.0}, {2.0, 20.0}});

  auto dock = makeBound();
  ASSERT_TRUE(dock->setBinding(u"value"_s, 0, key));
  dock->onTrackerTime(1.5);

  const int base = g.updates;
  app_->sessionManager().setDisplayOffset(other, DisplayOffset{Duration{kSecond}});
  EXPECT_EQ(g.updates, base) << "nothing this panel shows moved";
  app_->sessionManager().setDisplayOffset(wired, DisplayOffset{Duration{kSecond}});
  EXPECT_EQ(g.updates, base + 1);
}

TEST_F(CustomWidgetDockTest, ObjectWireDeliversBytesAndTypeName) {
  auto dock = makeBound(u"viewer"_s);
  ObjectStore& objects = app_->sessionManager().objectStore();
  auto topic =
      objects.registerTopic(ObjectTopicDescriptor{.dataset_id = dataset_, .topic_name = "/cam", .metadata_json = {}});
  ASSERT_TRUE(topic.has_value());
  ASSERT_TRUE(objects.pushOwned(*topic, kSecond, {9, 8, 7}).has_value());
  ASSERT_TRUE(objects.pushOwned(*topic, 2 * kSecond, {6}).has_value());

  EXPECT_TRUE(dock->tryAcceptObjectTopic(*topic, sdk::BuiltinObjectType::kImage, u"/cam"_s));
  const int base = g.updates;
  dock->onTrackerTime(1.2);
  EXPECT_EQ(g.updates, base + 1);
  ASSERT_EQ(g.last_values.size(), 1U);
  EXPECT_EQ(g.last_values[0].kind, PJ_WIDGET_VALUE_OBJECT);
  EXPECT_EQ(g.last_values[0].bytes, (std::vector<uint8_t>{9, 8, 7}));
  EXPECT_EQ(g.last_values[0].label, "/cam");
  dock->onTrackerTime(1.7);
  EXPECT_EQ(g.updates, base + 1);
  dock->onTrackerTime(2.1);
  EXPECT_EQ(g.last_values[0].bytes, (std::vector<uint8_t>{6}));
}

TEST_F(CustomWidgetDockTest, ReconciliationKeepsWiresByNameAndDropsTheOthers) {
  auto dock = makeBound();
  ASSERT_TRUE(dock->setBinding(u"value"_s, 0, number_));
  ASSERT_TRUE(dock->setBinding(u"status"_s, 0, text_));
  int inputs_changed = 0;
  int bindings_changed = 0;
  int workspace_changed = 0;
  QObject::connect(dock.get(), &CustomWidgetDockWidget::inputsChanged, [&] { ++inputs_changed; });
  QObject::connect(dock.get(), &CustomWidgetDockWidget::bindingsChanged, [&] { ++bindings_changed; });
  QObject::connect(dock.get(), &CustomWidgetDockWidget::workspaceChanged, [&] { ++workspace_changed; });

  EXPECT_TRUE(dock->applySettings(u"{\"dynamic\":true}"_s));
  EXPECT_EQ(parseJson(g.last_config), (QJsonObject{{u"dynamic"_s, true}, {u"unit"_s, u"m"_s}, {u"scale"_s, 2}}));
  ASSERT_EQ(dock->inputs().size(), 2U);
  EXPECT_EQ(dock->inputs()[1].name, "extra");
  ASSERT_EQ(dock->bindings().size(), 1U) << "status vanished with its input";
  EXPECT_EQ(dock->bindings()[0].input, u"value"_s);
  EXPECT_EQ(inputs_changed, 1);
  EXPECT_GE(bindings_changed, 1);
  EXPECT_GE(workspace_changed, 1);
  dock->onTrackerTime(2.0);
  ASSERT_EQ(g.last_values.size(), 1U);
  EXPECT_DOUBLE_EQ(g.last_values[0].number, 20.0);
}

TEST_F(CustomWidgetDockTest, InputsChangedFromInsideOnUpdateIsDeferred) {
  auto dock = makeBound();
  EXPECT_TRUE(dock->applySettings(u"{\"grow_on_update\":true}"_s));
  // applySettings pushed a value; the plugin announced a new input from inside that on_update.
  EXPECT_EQ(dock->inputs().size(), 2U) << "the list must not change under the running call";
  QCoreApplication::processEvents();
  ASSERT_EQ(dock->inputs().size(), 3U);
  EXPECT_EQ(dock->inputs()[2].name, "grown");
}

TEST_F(CustomWidgetDockTest, AFreshPanelReceivesEveryDeclaredSettingWithItsDefault) {
  auto dock = makeBound();
  EXPECT_EQ(g.load_configs, 1) << "the defaults are the widget's first configuration";
  EXPECT_EQ(parseJson(g.last_config), (QJsonObject{{u"unit"_s, u"m"_s}, {u"scale"_s, 2}}));
  EXPECT_EQ(parseJson(dock->settingsJson().toStdString()), (QJsonObject{{u"unit"_s, u"m"_s}, {u"scale"_s, 2}}));
}

TEST_F(CustomWidgetDockTest, AnEditedSettingLeavesTheOthersAtTheirDefaults) {
  auto dock = makeBound();
  ASSERT_TRUE(dock->applySettings(u"{\"unit\":\"km\"}"_s));
  EXPECT_EQ(parseJson(g.last_config), (QJsonObject{{u"unit"_s, u"km"_s}, {u"scale"_s, 2}}));

  // Restored from a layout that stored only the edited key: the defaults are filled again.
  QDomDocument doc;
  auto restored = makeDock();
  ASSERT_TRUE(restored->xmlLoadState(parseElement(
      doc,
      u"<CustomWidget plugin=\"test_widgets\" type=\"gauge\"><config><![CDATA[{\"scale\":5}]]></config>"
      u"</CustomWidget>"_s)));
  EXPECT_EQ(parseJson(g.last_config), (QJsonObject{{u"unit"_s, u"m"_s}, {u"scale"_s, 5}}));
}

TEST_F(CustomWidgetDockTest, ASaveConfigKeyCollidingWithASettingWins) {
  auto dock = makeBound();
  ASSERT_TRUE(dock->applySettings(u"{\"unit\":\"km\",\"own_state\":true}"_s));
  const QJsonObject settings = parseJson(dock->settingsJson().toStdString());
  EXPECT_EQ(settings.value(u"unit"_s).toString(), u"from_state"_s);
  EXPECT_EQ(settings.value(u"scale"_s).toInt(), 2) << "an untouched setting keeps its default";
  EXPECT_TRUE(settings.value(u"own_state"_s).toBool());
}

TEST_F(CustomWidgetDockTest, SettingsAreStoredWholeEvenWhenThePluginRejectsThem) {
  auto dock = makeBound();
  EXPECT_FALSE(dock->applySettings(u"{\"reject\":true}"_s));
  EXPECT_NE(dock->settingsJson().indexOf(u"reject"_s), -1);
  EXPECT_FALSE(dock->isMissing()) << "rejecting settings is part of the contract, not an error";
}

TEST_F(CustomWidgetDockTest, DroppedSeriesGoToTheFirstCompatibleInput) {
  auto dock = makeBound();
  const QString other = pj_cw_test::addNumberTopic(*app_, dataset_, "/other", {{1.0, 1.0}});
  EXPECT_FALSE(dock->tryAcceptSeriesKeys({}));
  EXPECT_FALSE(dock->tryAcceptSeriesKeys({u"nothing"_s}));
  EXPECT_TRUE(dock->tryAcceptSeriesKeys({number_, text_}));
  ASSERT_EQ(dock->bindings().size(), 2U);
  EXPECT_EQ(dock->bindings()[0].input, u"value"_s);
  EXPECT_EQ(dock->bindings()[1].input, u"status"_s);

  // Another number replaces the one on the only number input.
  EXPECT_TRUE(dock->tryAcceptSeriesKeys({other}));
  ASSERT_EQ(dock->bindings().size(), 2U);
  EXPECT_EQ(dock->bindings()[0].label, "/other/value");

  // Two numbers in one drop onto a single input: the first one wins.
  auto fresh = makeBound();
  EXPECT_TRUE(fresh->tryAcceptSeriesKeys({number_, other}));
  ASSERT_EQ(fresh->bindings().size(), 1U);
  EXPECT_EQ(fresh->bindings()[0].label, "/speed/value");
}

TEST_F(CustomWidgetDockTest, ADropThatWiresNothingTellsTheUserWhy) {
  const auto rejections = [](const CustomWidgetDockWidget& dock) {
    return std::ranges::count_if(dock.findChildren<MessageBox*>(), [](const MessageBox* box) {
      return box->objectName() == u"customWidgetDropRejected"_s;
    });
  };

  // "table" has a single number input: a text series has nowhere to go.
  auto dock = makeBound(u"table"_s);
  EXPECT_FALSE(dock->tryAcceptSeriesKeys({u"nothing"_s}));
  EXPECT_EQ(rejections(*dock), 0) << "a key with no kind is not a series";
  EXPECT_FALSE(dock->tryAcceptSeriesKeys({text_}));
  EXPECT_EQ(rejections(*dock), 1);

  // A drop where one key binds is accepted: no message.
  EXPECT_TRUE(dock->tryAcceptSeriesKeys({text_, number_}));
  EXPECT_EQ(rejections(*dock), 1);

  // An object topic dropped on a widget without an object input.
  ObjectStore& objects = app_->sessionManager().objectStore();
  auto topic =
      objects.registerTopic(ObjectTopicDescriptor{.dataset_id = dataset_, .topic_name = "/cam", .metadata_json = {}});
  ASSERT_TRUE(topic.has_value());
  auto gauge = makeBound();
  EXPECT_TRUE(gauge->tryAcceptSeriesKeys({number_}));
  EXPECT_EQ(rejections(*gauge), 0);
  EXPECT_FALSE(gauge->tryAcceptObjectTopic(*topic, sdk::BuiltinObjectType::kImage, u"/cam"_s));
  EXPECT_EQ(rejections(*gauge), 1);

  // A panel with no live plugin returns before any message.
  auto missing = makeDock();
  EXPECT_FALSE(missing->tryAcceptSeriesKeys({text_}));
  EXPECT_EQ(rejections(*missing), 0);
}

TEST_F(CustomWidgetDockTest, AMultipleInputTakesOneSlotPerDroppedSeries) {
  auto dock = makeBound(u"table"_s);
  const QString other = pj_cw_test::addNumberTopic(*app_, dataset_, "/other", {{1.0, 1.0}});
  EXPECT_TRUE(dock->tryAcceptSeriesKeys({number_, other}));
  ASSERT_EQ(dock->bindings().size(), 2U);
  EXPECT_EQ(dock->bindings()[0].slot, 0U);
  EXPECT_EQ(dock->bindings()[1].slot, 1U);
  dock->onTrackerTime(1.0);
  ASSERT_EQ(g.last_values.size(), 2U);
  EXPECT_EQ(g.last_values[1].slot, 1U);
  dock->removeBinding(u"rows"_s, 0);
  ASSERT_EQ(dock->bindings().size(), 1U);
  EXPECT_EQ(dock->bindings()[0].slot, 1U) << "slots are stable per wire";
}

TEST_F(CustomWidgetDockTest, SavedStateRoundTripsThroughXml) {
  auto dock = makeBound();
  app_->sessionManager().setDatasetSourcePath(dataset_, u"/data/robot.mcap"_s);
  ASSERT_TRUE(dock->setBinding(u"value"_s, 0, number_));
  ASSERT_TRUE(dock->setBinding(u"status"_s, 0, text_));
  ASSERT_TRUE(dock->applySettings(u"{\"unit\":\"km\"}"_s));

  QDomDocument doc;
  const QDomElement saved = dock->xmlSaveState(doc);
  doc.appendChild(saved);
  EXPECT_EQ(saved.tagName(), u"CustomWidget"_s);
  EXPECT_EQ(saved.attribute(u"plugin"_s), u"test_widgets"_s);
  EXPECT_EQ(saved.attribute(u"type"_s), u"gauge"_s);
  EXPECT_EQ(saved.attribute(u"plugin_version"_s), u"1.2.3"_s);
  const QDomElement first = saved.firstChildElement(u"input"_s);
  EXPECT_EQ(first.attribute(u"name"_s), u"value"_s);
  // The saved path is the session's NORMALIZED registration (drive-qualified on Windows), so
  // compare against the same source xmlSaveState reads.
  const QString normalized_path = app_->catalogModel().datasetSourcePath(dataset_);
  EXPECT_FALSE(normalized_path.isEmpty());
  EXPECT_EQ(first.attribute(u"dataset_path"_s), normalized_path);
  EXPECT_EQ(first.attribute(u"dataset_source"_s), u"robot.mcap"_s);
  EXPECT_EQ(first.attribute(u"topic"_s), u"/speed"_s);
  const QDomElement config = saved.firstChildElement(u"config"_s);
  ASSERT_FALSE(config.isNull());
  EXPECT_NE(config.text().indexOf(u"km"_s), -1);

  const QString xml = doc.toString();
  QDomDocument parsed;
  ASSERT_TRUE(parsed.setContent(xml));
  auto restored = makeDock();
  ASSERT_TRUE(restored->xmlLoadState(parsed.documentElement()));
  EXPECT_FALSE(restored->isMissing());
  ASSERT_EQ(restored->bindings().size(), 2U);
  EXPECT_TRUE(restored->bindings()[0].resolved);
  EXPECT_EQ(restored->bindings()[0].key, number_);
  EXPECT_EQ(restored->bindings()[1].key, text_);
  EXPECT_NE(restored->settingsJson().indexOf(u"km"_s), -1);
  EXPECT_NE(g.last_config.find("km"), std::string::npos) << "restore hands the whole JSON to load_config";

  QDomDocument again;
  again.appendChild(restored->xmlSaveState(again));
  EXPECT_EQ(again.toString(), xml);
}

TEST_F(CustomWidgetDockTest, ARestoredWidgetReadsItsHistoryWhileCreatingItsView) {
  auto dock = makeBound();
  ASSERT_TRUE(dock->setBinding(u"value"_s, 0, number_));
  ASSERT_TRUE(dock->applySettings(u"{\"read_past_at_create\":true}"_s));
  QDomDocument doc;
  const QDomElement saved = dock->xmlSaveState(doc);
  doc.appendChild(saved);

  g.history_samples = -1;
  auto restored = makeDock();
  ASSERT_TRUE(restored->xmlLoadState(saved));
  EXPECT_TRUE(g.history_ok);
  EXPECT_EQ(g.history_samples, 3) << "the saved wires exist before create_qwidget";
  EXPECT_EQ(restored->bindings().size(), 1U);
}

TEST_F(CustomWidgetDockTest, ARestoreThatFailsAtCreationKeepsNoLiveWires) {
  auto dock = makeBound();
  ASSERT_TRUE(dock->setBinding(u"value"_s, 0, number_));
  QDomDocument doc;
  const QDomElement saved = dock->xmlSaveState(doc);
  doc.appendChild(saved);

  g.null_widget = true;
  auto restored = makeDock();
  ASSERT_TRUE(restored->xmlLoadState(saved));
  g.null_widget = false;
  EXPECT_TRUE(restored->isMissing());
  EXPECT_TRUE(restored->bindings().empty());
  EXPECT_TRUE(restored->inputs().empty());
  QDomDocument round_trip;
  const QDomElement again = restored->xmlSaveState(round_trip);
  EXPECT_EQ(again.firstChildElement(u"input"_s).attribute(u"topic"_s), u"/speed"_s);
}

TEST_F(CustomWidgetDockTest, ACdataTerminatorInASettingSurvivesTheXmlRoundTrip) {
  auto dock = makeBound();
  ASSERT_TRUE(dock->applySettings(u"{\"unit\":\"[m]]>x\"}"_s));

  QDomDocument doc;
  doc.appendChild(dock->xmlSaveState(doc));
  QDomDocument parsed;
  ASSERT_TRUE(parsed.setContent(doc.toString())) << doc.toString().toStdString();
  const QDomElement config = parsed.documentElement().firstChildElement(u"config"_s);
  EXPECT_NE(config.text().indexOf(u"[m]]>x"_s), -1) << config.text().toStdString();
}

TEST_F(CustomWidgetDockTest, AbsentPluginKeepsItsElementVerbatim) {
  const QString xml =
      u"<CustomWidget plugin=\"absent_plugin\" type=\"gauge\" plugin_version=\"9.9\">"
      u"<input name=\"value\" slot=\"0\" kind=\"number\" dataset_id=\"3\" dataset_source=\"x.csv\" topic=\"/t\" "
      u"field=\"f\"/>"
      u"<config><![CDATA[{\"min\":1}]]></config></CustomWidget>"_s;
  QDomDocument in;
  auto dock = makeDock();
  ASSERT_TRUE(dock->xmlLoadState(parseElement(in, xml)));
  EXPECT_TRUE(dock->isMissing());
  EXPECT_NE(dock->missingReason().indexOf(u"absent_plugin/gauge"_s), -1);
  EXPECT_NE(dock->findChild<MissingWidgetPlaceholder*>(), nullptr);
  EXPECT_EQ(g.widgets_created, 0);
  EXPECT_FALSE(dock->tryAcceptSeriesKeys({number_}));

  QDomDocument out;
  out.appendChild(dock->xmlSaveState(out));
  EXPECT_EQ(out.toString(), in.toString());
}

TEST_F(CustomWidgetDockTest, UnknownWidgetTypeOfAnInstalledPluginIsMissingToo) {
  auto dock = makeDock();
  EXPECT_FALSE(dock->bindToWidget(u"test_widgets"_s, u"nope"_s));
  EXPECT_TRUE(dock->isMissing());
  EXPECT_EQ(g.instances, 0);
}

TEST_F(CustomWidgetDockTest, QtMismatchShowsAPlaceholderAndNeverCreatesTheWidget) {
  auto dock = makeDock();
  EXPECT_FALSE(dock->bindToWidget(u"test_widgets_other_qt"_s, u"gauge"_s));
  EXPECT_TRUE(dock->isMissing());
  EXPECT_EQ(g.instances, 0) << "not even the plugin instance is created";
  EXPECT_EQ(g.widgets_created, 0);
  auto* placeholder = dock->findChild<MissingWidgetPlaceholder*>();
  ASSERT_NE(placeholder, nullptr);
  ASSERT_NE(placeholder->findChild<QLabel*>(), nullptr);
  EXPECT_NE(placeholder->findChild<QLabel*>()->text().indexOf(u"Qt"_s), -1);

  QDomDocument doc;
  const QString xml = u"<CustomWidget plugin=\"test_widgets_other_qt\" type=\"gauge\"/>"_s;
  auto other = makeDock();
  ASSERT_TRUE(other->xmlLoadState(parseElement(doc, xml)));
  QDomDocument out;
  out.appendChild(other->xmlSaveState(out));
  EXPECT_EQ(out.toString(), doc.toString());
}

TEST_F(CustomWidgetDockTest, PluginInstanceDiesBeforeTheDockAndTheWidgetIsDetachedFirst) {
  auto dock = makeBound();
  QPointer<QLabel> label = dock->findChild<QLabel*>();
  ASSERT_FALSE(label.isNull());
  dock.reset();
  EXPECT_EQ(g.destroyed, 1);
  EXPECT_EQ(g.widgets_deleted_by_plugin, 1);
  EXPECT_EQ(g.destroyed_while_parented, 0) << "the plugin would otherwise delete a widget its parent still owns";
  EXPECT_TRUE(label.isNull());
}

TEST_F(CustomWidgetDockTest, AboutToBeDiscardedDestroysThePluginObjectsSynchronously) {
  auto dock = makeBound();
  QPointer<QLabel> label = dock->findChild<QLabel*>();
  dock->aboutToBeDiscarded();
  EXPECT_EQ(g.destroyed, 1);
  EXPECT_TRUE(label.isNull());
  EXPECT_EQ(g.destroyed_while_parented, 0);
  dock->onTrackerTime(1.0);  // a late tick on a discarded dock is harmless
  dock.reset();
  EXPECT_EQ(g.destroyed, 1) << "no second destroy";
}

TEST_F(CustomWidgetDockTest, RebindingReplacesTheInstanceThroughTheSamePath) {
  auto dock = makeBound();
  ASSERT_TRUE(dock->setBinding(u"value"_s, 0, number_));
  EXPECT_TRUE(dock->bindToWidget(u"test_widgets"_s, u"table"_s));
  EXPECT_EQ(g.destroyed, 1);
  EXPECT_EQ(g.instances, 2);
  EXPECT_TRUE(dock->bindings().empty());
  ASSERT_EQ(dock->inputs().size(), 1U);
  EXPECT_EQ(dock->inputs()[0].name, "rows");
}

TEST_F(CustomWidgetDockTest, UnresolvedWiresAreRetriedWhenTheCatalogGrows) {
  const QString xml =
      u"<CustomWidget plugin=\"test_widgets\" type=\"gauge\">"
      u"<input name=\"value\" slot=\"0\" kind=\"number\" dataset_source=\"robot.mcap\" topic=\"/late\" "
      u"field=\"value\"/>"
      u"</CustomWidget>"_s;
  QDomDocument doc;
  auto dock = makeDock();
  ASSERT_TRUE(dock->xmlLoadState(parseElement(doc, xml)));
  ASSERT_EQ(dock->bindings().size(), 1U);
  EXPECT_FALSE(dock->bindings()[0].resolved);
  EXPECT_TRUE(dock->displayedTopics().empty());
  ASSERT_EQ(g.last_values.size(), 1U);
  EXPECT_EQ(g.last_values[0].kind, PJ_WIDGET_VALUE_NONE);
  EXPECT_EQ(g.last_values[0].label, "/late/value") << "a pending wire is still named";

  int bindings_changed = 0;
  QObject::connect(dock.get(), &CustomWidgetDockWidget::bindingsChanged, [&] { ++bindings_changed; });
  pj_cw_test::addNumberTopic(*app_, dataset_, "/late", {{1.0, 5.0}});
  EXPECT_TRUE(dock->bindings()[0].resolved);
  EXPECT_GE(bindings_changed, 1);
  EXPECT_EQ(dock->displayedTopics().size(), 1U);
  dock->onTrackerTime(1.0);
  EXPECT_EQ(g.last_values[0].kind, PJ_WIDGET_VALUE_NUMBER);
  EXPECT_DOUBLE_EQ(g.last_values[0].number, 5.0);
}

TEST_F(CustomWidgetDockTest, APendingWireReportsTheTopicItWaitsFor) {
  const QString xml =
      u"<CustomWidget plugin=\"test_widgets\" type=\"gauge\">"
      u"<input name=\"value\" slot=\"0\" kind=\"number\" dataset_source=\"robot.mcap\" topic=\"/late\" "
      u"field=\"value\"/>"
      u"<input name=\"status\" slot=\"0\" kind=\"text\" dataset_source=\"robot.mcap\" topic=\"/mode\" "
      u"field=\"state\"/>"
      u"</CustomWidget>"_s;
  QDomDocument doc;
  auto dock = makeDock();
  ASSERT_TRUE(dock->xmlLoadState(parseElement(doc, xml)));
  ASSERT_EQ(dock->bindings().size(), 2U);
  ASSERT_TRUE(dock->bindings()[1].resolved) << "/mode is loaded";
  EXPECT_EQ(dock->pendingDemands(), (std::vector<PendingWireDemand>{{u"/late"_s, dataset_}}))
      << "only the unresolved wire, with the dataset its saved identity names";

  pj_cw_test::addNumberTopic(*app_, dataset_, "/late", {{1.0, 5.0}});
  EXPECT_TRUE(dock->pendingDemands().empty());
}

TEST_F(CustomWidgetDockTest, HistoryServiceReadsTheWireOfTheInputAndSlot) {
  auto dock = makeBound(u"table"_s);
  const QString other = pj_cw_test::addNumberTopic(*app_, dataset_, "/other", {{1.0, 1.0}});
  ASSERT_TRUE(dock->applySettings(u"{\"history\":true}"_s));
  ASSERT_TRUE(dock->setBinding(u"rows"_s, WidgetBindings::kNewSlot, other));
  dock->onTrackerTime(2.0);
  EXPECT_TRUE(g.history_ok);
  EXPECT_EQ(g.history_samples, 1) << "slot 0 is the /other wire, which has one sample";
  EXPECT_EQ(g.history_total, 1U);
  ASSERT_TRUE(dock->setBinding(u"rows"_s, 0, number_));
  dock->onTrackerTime(2.0);
  EXPECT_EQ(g.history_samples, 3);
  EXPECT_EQ(g.history_total, 3U) << "nothing was capped";
}

// ---------------------------------------------------------------- errors and the active-widgets record

TEST_F(CustomWidgetDockTest, AnErrorAtTheBoundaryDisablesThePanelAndStopsCallingThePlugin) {
  auto dock = makeDock();
  captureDiagnostics(*dock);
  ASSERT_TRUE(dock->bindToWidget(u"faulty_widgets"_s, u"gauge"_s));
  ASSERT_TRUE(dock->setBinding(u"value"_s, 0, number_));
  ASSERT_TRUE(dock->applySettings(u"{\"unit\":\"km\"}"_s));
  dock->onTrackerTime(1.0);
  const int updates = g.updates;
  QPointer<QLabel> label = dock->findChild<QLabel*>();
  ASSERT_FALSE(label.isNull());
  int inputs_changed = 0;
  QObject::connect(dock.get(), &CustomWidgetDockWidget::inputsChanged, [&] { ++inputs_changed; });

  // The next settings change reconciles the inputs; the host refuses the impossible name.
  g.corrupt_inputs = true;
  dock->applySettings(u"{\"unit\":\"mi\"}"_s);

  EXPECT_TRUE(dock->isMissing());
  EXPECT_TRUE(dock->missingReason().startsWith(u"This widget was disabled after an error: "_s))
      << dock->missingReason().toStdString();
  auto* placeholder = dock->findChild<MissingWidgetPlaceholder*>();
  ASSERT_NE(placeholder, nullptr);
  ASSERT_NE(placeholder->findChild<QLabel*>(), nullptr);
  EXPECT_EQ(placeholder->findChild<QLabel*>()->text(), dock->missingReason());
  EXPECT_EQ(g.destroyed, 1) << "the plugin instance is destroyed at once";
  EXPECT_EQ(g.destroyed_while_parented, 0);
  EXPECT_TRUE(label.isNull());
  EXPECT_TRUE(dock->inputs().empty());
  EXPECT_TRUE(dock->bindings().empty());
  EXPECT_TRUE(dock->displayedTopics().empty()) << "a disabled panel holds no demand";
  EXPECT_GE(inputs_changed, 1) << "the side panel learns the panel is gone";
  EXPECT_TRUE(reported(DiagnosticLevel::kError, "faulty_widgets/gauge was disabled after an error"));

  dock->onTrackerTime(2.0);
  dock->onTrackerTime(3.0);
  EXPECT_EQ(g.updates, updates) << "a disabled panel never calls its plugin again";
  EXPECT_FALSE(dock->setBinding(u"value"_s, 0, number_));
  EXPECT_FALSE(dock->tryAcceptSeriesKeys({number_}));

  // The layout keeps the panel as it was before the error.
  QDomDocument doc;
  const QDomElement saved = dock->xmlSaveState(doc);
  EXPECT_EQ(saved.attribute(u"plugin"_s), u"faulty_widgets"_s);
  EXPECT_EQ(saved.attribute(u"type"_s), u"gauge"_s);
  EXPECT_EQ(saved.firstChildElement(u"input"_s).attribute(u"topic"_s), u"/speed"_s);
  EXPECT_NE(saved.firstChildElement(u"config"_s).text().indexOf(u"mi"_s), -1);

  g.corrupt_inputs = false;
  EXPECT_TRUE(dock->bindToWidget(u"faulty_widgets"_s, u"gauge"_s)) << "choosing the widget again re-enables it";
  EXPECT_FALSE(dock->isMissing());
}

TEST_F(CustomWidgetDockTest, InputsFailingAfterLoadConfigLeaveTheWiresAndDisableThePanel) {
  auto dock = makeBound();
  captureDiagnostics(*dock);
  ASSERT_TRUE(dock->setBinding(u"value"_s, 0, number_));
  ASSERT_TRUE(dock->setBinding(u"status"_s, 0, text_));
  int bindings_changed = 0;
  QObject::connect(dock.get(), &CustomWidgetDockWidget::bindingsChanged, [&] { ++bindings_changed; });

  // load_config accepts the settings; the inputs() that follows fails.
  dock->applySettings(u"{\"fail_inputs\":true}"_s);

  EXPECT_TRUE(dock->isMissing());
  EXPECT_NE(dock->missingReason().indexOf(u"inputs could not be read"_s), -1) << dock->missingReason().toStdString();
  EXPECT_NE(dock->missingReason().indexOf(u"inputs unavailable"_s), -1) << "the plugin's own message";
  EXPECT_TRUE(reported(DiagnosticLevel::kError, "disabled after an error"));
  EXPECT_EQ(g.destroyed, 1);
  EXPECT_GE(bindings_changed, 1);

  // Not reconciled against a list the plugin could not produce: both wires are in the layout.
  QDomDocument doc;
  const QDomElement saved = dock->xmlSaveState(doc);
  std::vector<QString> wired;
  for (QDomElement input = saved.firstChildElement(u"input"_s); !input.isNull();
       input = input.nextSiblingElement(u"input"_s)) {
    wired.push_back(input.attribute(u"name"_s) + u'=' + input.attribute(u"topic"_s));
  }
  EXPECT_EQ(wired, (std::vector<QString>{u"value=/speed"_s, u"status=/mode"_s}));
}

TEST_F(CustomWidgetDockTest, AnErrorDuringRestoreKeepsTheLoadedElementVerbatim) {
  const QString xml =
      u"<CustomWidget plugin=\"faulty_widgets\" type=\"gauge\" plugin_version=\"0.1.0\">"
      u"<input name=\"value\" slot=\"0\" kind=\"number\" dataset_id=\"7\" dataset_source=\"robot.mcap\" "
      u"topic=\"/speed\" field=\"value\"/>"
      u"<config><![CDATA[{\"unit\":\"km\"}]]></config></CustomWidget>"_s;
  g.corrupt_inputs = true;
  QDomDocument in;
  auto dock = makeDock();
  captureDiagnostics(*dock);
  ASSERT_TRUE(dock->xmlLoadState(parseElement(in, xml)));
  EXPECT_TRUE(dock->isMissing());
  EXPECT_NE(dock->missingReason().indexOf(u"disabled after an error"_s), -1);
  EXPECT_EQ(g.instances, 1);
  EXPECT_EQ(g.destroyed, 1);
  EXPECT_EQ(g.widgets_created, 0) << "the error came before create_qwidget";
  EXPECT_TRUE(reported(DiagnosticLevel::kError, "disabled after an error"));

  QDomDocument out;
  out.appendChild(dock->xmlSaveState(out));
  EXPECT_EQ(out.toString(), in.toString());
}

TEST_F(CustomWidgetDockTest, AnErrorWhileSavingDisablesThePanelFromTheEventLoop) {
  auto dock = makeDock();
  ASSERT_TRUE(dock->bindToWidget(u"faulty_widgets"_s, u"gauge"_s));
  ASSERT_TRUE(dock->applySettings(u"{\"unit\":\"km\"}"_s));
  g.corrupt_saved_config = true;

  QDomDocument doc;
  const QDomElement saved = dock->xmlSaveState(doc);
  EXPECT_NE(saved.firstChildElement(u"config"_s).text().indexOf(u"km"_s), -1) << "the stored settings are saved";
  EXPECT_FALSE(dock->isMissing()) << "a save never rebuilds the panel under the caller";

  QCoreApplication::processEvents();
  EXPECT_TRUE(dock->isMissing());
  EXPECT_EQ(g.destroyed, 1);
  EXPECT_EQ(g.destroyed_while_parented, 0);
}

TEST_F(CustomWidgetDockTest, ACreateQWidgetThatReturnsNullLeavesThePlaceholder) {
  g.null_widget = true;
  auto dock = makeDock();
  EXPECT_FALSE(dock->bindToWidget(u"test_widgets"_s, u"gauge"_s));
  EXPECT_TRUE(dock->isMissing());
  EXPECT_NE(dock->missingReason().indexOf(u"createWidget returned null"_s), -1) << dock->missingReason().toStdString();
  EXPECT_EQ(g.instances, 1);
  EXPECT_EQ(g.destroyed, 1);
  dock->onTrackerTime(1.0);
  EXPECT_EQ(g.updates, 0);

  QDomDocument doc;
  const QDomElement saved = dock->xmlSaveState(doc);
  EXPECT_EQ(saved.attribute(u"plugin"_s), u"test_widgets"_s);
  EXPECT_EQ(saved.attribute(u"type"_s), u"gauge"_s);
}

TEST_F(CustomWidgetDockTest, AnExceptionInOnUpdateIsContainedByTheSdkTrampoline) {
  // The SDK base catches it inside the plugin, so it never reaches the host: the panel stays live.
  auto dock = makeBound();
  ASSERT_TRUE(dock->applySettings(u"{\"throw_on_update\":true}"_s));
  ASSERT_TRUE(dock->setBinding(u"value"_s, 0, number_));
  const int updates = g.updates;
  dock->onTrackerTime(1.0);
  dock->onTrackerTime(2.0);
  EXPECT_EQ(g.updates, updates + 2);
  EXPECT_FALSE(dock->isMissing());
}

TEST_F(CustomWidgetDockTest, TheActiveWidgetsRecordCoversEveryPluginCall) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  auto marker = std::make_shared<ActiveWidgetsMarker>(dir.path());
  const QString path = marker->path();
  using Entry = ActiveWidgetsMarker::Entry;
  const std::vector<Entry> gauge{{u"test_widgets"_s, u"gauge"_s}};

  // Already on disk when the plugin creates its widget.
  std::vector<Entry> seen_at_create;
  g.on_create_widget = [&]() { seen_at_create = ActiveWidgetsMarker::read(path).entries; };
  auto dock = makeDock();
  dock->setActiveWidgetsMarker(marker);
  ASSERT_TRUE(dock->bindToWidget(u"test_widgets"_s, u"gauge"_s));
  EXPECT_EQ(seen_at_create, gauge);
  EXPECT_EQ(ActiveWidgetsMarker::read(path).entries, gauge);

  dock->aboutToBeDiscarded();
  EXPECT_FALSE(QFile::exists(path)) << "released: the plugin instance is gone";

  // A panel that fails to create, or is disabled, does not stay on the record either.
  g.null_widget = true;
  EXPECT_FALSE(dock->bindToWidget(u"test_widgets"_s, u"gauge"_s));
  EXPECT_FALSE(QFile::exists(path));
  g.null_widget = false;
  g.corrupt_inputs = true;
  EXPECT_FALSE(dock->bindToWidget(u"faulty_widgets"_s, u"gauge"_s));
  EXPECT_FALSE(QFile::exists(path));
  g.corrupt_inputs = false;
  g.on_create_widget = nullptr;

  ASSERT_TRUE(dock->bindToWidget(u"test_widgets"_s, u"table"_s));
  EXPECT_EQ(ActiveWidgetsMarker::read(path).entries, (std::vector<Entry>{{u"test_widgets"_s, u"table"_s}}));
  dock.reset();
  EXPECT_FALSE(QFile::exists(path));
}

}  // namespace
}  // namespace PJ
