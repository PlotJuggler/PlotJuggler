// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>
#include <qwt_plot_curve.h>
#include <qwt_text.h>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QDomDocument>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QMimeData>
#include <QMouseEvent>
#include <QRectF>
#include <QSplitter>
#include <QStringList>
#include <QToolButton>
#include <QtGlobal>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "pj_base/type_tree.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_plotting/DockToolbar.h"
#include "pj_plotting/DockWidget.h"
#include "pj_plotting/PlotDocker.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_plotting/StateTransitionsDockWidget.h"
#include "pj_plotting/TabbedPlotWidget.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/IDataWidget.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/SessionManager.h"
#include "pj_widgets/CurveTreeView.h"
#include "pj_widgets/VisualizationKind.h"
#include "pj_widgets/VisualizationPlaceholderWidget.h"
using namespace Qt::StringLiterals;

namespace {

PJ::TopicId addScalarTopic(PJ::SessionManager& session, PJ::DatasetId dataset_id, std::string_view topic_name) {
  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto handle_or = writer.registerScalarSeries(dataset_id, topic_name, PJ::NumericType::kFloat64);
  EXPECT_TRUE(handle_or.has_value()) << handle_or.error();
  if (!handle_or.has_value()) {
    return 0;
  }
  writer.appendScalar(*handle_or, 100, 1.0);
  const auto committed_topics = session.commitChunks(writer.flushAll());
  EXPECT_FALSE(committed_topics.empty());
  return handle_or->topic_id;
}

QString keyForTopic(PJ::CatalogModel& catalog, PJ::TopicId topic_id) {
  for (const PJ::CurveDescriptor& descriptor : catalog.curves()) {
    if (descriptor.topic_id == topic_id) {
      return descriptor.name;
    }
  }
  return {};
}

class TestPlaceholderWidget : public PJ::VisualizationPlaceholderWidget {
 public:
  using PJ::VisualizationPlaceholderWidget::VisualizationPlaceholderWidget;

  void sendDragEnter(QDragEnterEvent* event) {
    dragEnterEvent(event);
  }

  void sendDragMove(QDragMoveEvent* event) {
    dragMoveEvent(event);
  }

  void sendDrop(QDropEvent* event) {
    dropEvent(event);
  }

  bool sendFilteredEvent(QObject* watched, QEvent* event) {
    return eventFilter(watched, event);
  }
};

class FakeObjectWidget : public QWidget, public PJ::IDataWidget {
 public:
  using QWidget::QWidget;

  QWidget* widget() override {
    return this;
  }

  void onTrackerTime(double /*time*/) override {}
};

// Persists a non-null element tagged like a real scene dock (<scene2d>/<scene3d>),
// so an *empty* instance still round-trips through layout save/restore instead of
// being skipped. Mirrors SceneDockWidget::xmlSaveState's "always emit a root".
class FakeStatefulObjectWidget : public QWidget, public PJ::IDataWidget {
 public:
  explicit FakeStatefulObjectWidget(QString tag, QWidget* parent = nullptr) : QWidget(parent), tag_(std::move(tag)) {}

  QWidget* widget() override {
    return this;
  }
  void onTrackerTime(double /*time*/) override {}
  QDomElement xmlSaveState(QDomDocument& doc) const override {
    QDomElement element = doc.createElement(tag_);
    element.setAttribute(u"version"_s, u"1"_s);
    return element;
  }
  bool xmlLoadState(const QDomElement& /*element*/) override {
    return true;
  }

 private:
  QString tag_;
};

class FakeRejectingObjectWidget : public QWidget, public PJ::IDataWidget {
 public:
  using QWidget::QWidget;

  QWidget* widget() override {
    return this;
  }
  void onTrackerTime(double /*time*/) override {}
  bool xmlLoadState(const QDomElement& /*element*/) override {
    return false;
  }
};

class FakeClipboardObjectWidget : public QWidget, public PJ::IDataWidget {
 public:
  FakeClipboardObjectWidget(QString tag, QString value, QWidget* parent = nullptr)
      : QWidget(parent), tag_(std::move(tag)), value_(std::move(value)) {}

  QWidget* widget() override {
    return this;
  }
  void onTrackerTime(double /*time*/) override {}
  QDomElement xmlSaveState(QDomDocument& doc) const override {
    QDomElement element = doc.createElement(tag_);
    element.setAttribute(u"value"_s, value_);
    return element;
  }
  bool xmlLoadState(const QDomElement& element) override {
    if (element.tagName() != tag_) {
      return false;
    }
    ++load_count_;
    loaded_value_ = element.attribute(u"value"_s);
    return true;
  }

  int loadCount() const {
    return load_count_;
  }
  QString loadedValue() const {
    return loaded_value_;
  }

 private:
  QString tag_;
  QString value_;
  int load_count_ = 0;
  QString loaded_value_;
};

// Accepts every offered object topic in place (like the multi-topic 3D dock), so
// a drop onto an already-mounted empty widget exercises DockWidget's "offer"
// path rather than the factory-replace path.
class FakeAcceptingObjectWidget : public QWidget, public PJ::IDataWidget {
 public:
  using QWidget::QWidget;

  QWidget* widget() override {
    return this;
  }
  void onTrackerTime(double /*time*/) override {}
  bool tryAcceptObjectTopic(
      PJ::ObjectTopicId /*topic_id*/, PJ::sdk::BuiltinObjectType /*object_type*/, const QString& /*title*/) override {
    ++accepted_count_;
    return true;
  }

  int acceptedCount() const {
    return accepted_count_;
  }

 private:
  int accepted_count_ = 0;
};

void registerImageObjectTopic(PJ::SessionManager& session, PJ::DatasetId dataset_id, std::string_view topic_name) {
  auto topic_or = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = dataset_id,
          .topic_name = std::string(topic_name),
          .metadata_json = R"({"builtin_object_type":"kImage"})",
      });
  EXPECT_TRUE(topic_or.has_value()) << topic_or.error();
}

QToolButton* iconButton(QWidget* placeholder, const char* object_name) {
  return placeholder->findChild<QToolButton*>(QString::fromLatin1(object_name));
}

QAction* findActionByText(QObject* parent, const QString& text) {
  for (auto* action : parent->findChildren<QAction*>()) {
    QString action_text = action->text();
    action_text.remove(QLatin1Char('&'));
    if (action_text == text) {
      return action;
    }
  }
  return nullptr;
}

// Splits the placeholder inside `dock` and returns the newly created sibling.
PJ::DockWidget* splitFrom(PJ::PlotDocker& docker, PJ::DockWidget* dock, int expected_count) {
  auto* placeholder = dock->findChild<PJ::VisualizationPlaceholderWidget*>();
  EXPECT_NE(placeholder, nullptr);
  if (placeholder == nullptr) {
    return nullptr;
  }
  auto* split = findActionByText(placeholder, u"Split Horizontally"_s);
  EXPECT_NE(split, nullptr);
  if (split == nullptr) {
    return nullptr;
  }
  split->trigger();
  EXPECT_EQ(docker.plotCount(), expected_count);
  return docker.plotAt(expected_count - 1);
}

}  // namespace

// Focus behavior needs ADS's FocusController, which only exists when the
// FocusHighlighting config flag is set at CDockManager construction. The flag
// is process-global; set it per-test and restore it so test order can't leak
// focus behavior into tests that don't expect it.
class DockFocusTest : public ::testing::Test {
 protected:
  void SetUp() override {
    saved_flags_ = ads::CDockManager::configFlags();
    ads::CDockManager::setConfigFlag(ads::CDockManager::FocusHighlighting, true);
  }
  void TearDown() override {
    ads::CDockManager::setConfigFlags(saved_flags_);
  }

 private:
  ads::CDockManager::ConfigFlags saved_flags_;
};

TEST(DockWidgetPlaceholderTest, EmptyDockerStartsWithPlaceholderDock) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  PJ::PlotDocker docker(u"test"_s, &session, &catalog);

  ASSERT_EQ(docker.plotCount(), 1);
  auto* dock = docker.plotAt(0);
  ASSERT_NE(dock, nullptr);
  EXPECT_EQ(dock->plotWidget(), nullptr);
  EXPECT_EQ(dock->objectWidget(), nullptr);
}

TEST(DockWidgetPlaceholderTest, BarePlaceholderSurvivesLayoutSaveRestore) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  PJ::PlotDocker source(u"source"_s, &session, &catalog);
  auto* source_dock = source.plotAt(0);
  ASSERT_NE(source_dock, nullptr);
  ASSERT_EQ(source_dock->plotWidget(), nullptr);
  ASSERT_EQ(source_dock->objectWidget(), nullptr);

  QDomDocument doc;
  const QDomElement saved = source.xmlSaveState(doc);
  EXPECT_EQ(saved.elementsByTagName(u"placeholder"_s).size(), 1);

  PJ::PlotDocker restored(u"restored"_s, &session, &catalog);
  ASSERT_TRUE(restored.xmlLoadState(saved));
  auto* restored_dock = restored.plotAt(0);
  ASSERT_NE(restored_dock, nullptr);
  EXPECT_EQ(restored_dock->plotWidget(), nullptr);
  EXPECT_EQ(restored_dock->objectWidget(), nullptr);
  EXPECT_NE(restored_dock->findChild<PJ::VisualizationPlaceholderWidget*>(), nullptr);
}

TEST(DockWidgetPlaceholderTest, PlaceholderSplitActionsEmitRequests) {
  TestPlaceholderWidget placeholder;
  int horizontal_count = 0;
  int vertical_count = 0;
  QObject::connect(&placeholder, &PJ::VisualizationPlaceholderWidget::splitHorizontalRequested, &placeholder, [&]() {
    ++horizontal_count;
  });
  QObject::connect(&placeholder, &PJ::VisualizationPlaceholderWidget::splitVerticalRequested, &placeholder, [&]() {
    ++vertical_count;
  });

  auto* horizontal_action = findActionByText(&placeholder, u"Split Horizontally"_s);
  auto* vertical_action = findActionByText(&placeholder, u"Split Vertically"_s);
  ASSERT_NE(horizontal_action, nullptr);
  ASSERT_NE(vertical_action, nullptr);
  EXPECT_EQ(findActionByText(&placeholder, u"Copy"_s), nullptr);
  auto* paste_action = findActionByText(&placeholder, u"Paste"_s);
  ASSERT_NE(paste_action, nullptr);
  EXPECT_FALSE(paste_action->isEnabled());

  horizontal_action->trigger();
  vertical_action->trigger();

  EXPECT_EQ(horizontal_count, 1);
  EXPECT_EQ(vertical_count, 1);
}

TEST(DockWidgetPlaceholderTest, PlotWidgetCopyPasteUsesClipboardXmlAndKeepsTargetIdentity) {
  QGuiApplication::clipboard()->clear();

  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const PJ::TopicId topic_id = addScalarTopic(session, *dataset, "/imu/accel");
  ASSERT_NE(topic_id, 0U);
  const QString key = keyForTopic(catalog, topic_id);
  ASSERT_FALSE(key.isEmpty());

  PJ::PlotWidget source(&session, &catalog);
  source.setStateId(u"source-plot"_s);
  ASSERT_NE(source.addCurve(key), nullptr);

  PJ::PlotWidget target(&session, &catalog);
  target.setStateId(u"target-plot"_s);
  int undo_count = 0;
  QObject::connect(&target, &PJ::PlotWidget::undoableChange, &target, [&]() { ++undo_count; });

  ASSERT_TRUE(QMetaObject::invokeMethod(&source, "copyWidgetToClipboard", Qt::DirectConnection));
  EXPECT_TRUE(QGuiApplication::clipboard()->text().contains("<plot"_L1));

  ASSERT_TRUE(QMetaObject::invokeMethod(&target, "pasteWidgetFromClipboard", Qt::DirectConnection));

  ASSERT_EQ(target.curveList().size(), 1U);
  EXPECT_EQ(target.curveList().front().source_name, key);
  EXPECT_EQ(target.stateId(), u"target-plot"_s);
  EXPECT_EQ(undo_count, 1);
}

// Two datasets share "/imu/accel". The curve is copied from dataset 2, then
// pasted into a session where dataset 1 (same topic, loaded first) is also
// present. xmlSaveState stamps dataset_id + dataset_source on the clipboard
// XML, so rebindClipboardCurveKeys must resolve the pasted curve back to
// dataset 2's key via CatalogModel::resolveDatasetIdentity — never dataset 1's,
// which a naive first-match topic+field scan would pick.
TEST(DockWidgetPlaceholderTest, PlotWidgetPasteResolvesDatasetQualifiedCurveToItsSourceDataset) {
  QGuiApplication::clipboard()->clear();

  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset1 = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "run1.mcap"});
  auto dataset2 = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "run2.mcap"});
  ASSERT_TRUE(dataset1.has_value()) << dataset1.error();
  ASSERT_TRUE(dataset2.has_value()) << dataset2.error();
  ASSERT_NE(addScalarTopic(session, *dataset1, "/imu/accel"), 0U);
  const PJ::TopicId topic2_id = addScalarTopic(session, *dataset2, "/imu/accel");
  ASSERT_NE(topic2_id, 0U);
  const QString key2 = keyForTopic(catalog, topic2_id);
  ASSERT_FALSE(key2.isEmpty());

  PJ::PlotWidget source(&session, &catalog);
  ASSERT_NE(source.addCurve(key2), nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(&source, "copyWidgetToClipboard", Qt::DirectConnection));
  EXPECT_TRUE(QGuiApplication::clipboard()->text().contains(u"dataset_source=\"run2.mcap\""_s));

  PJ::PlotWidget target(&session, &catalog);
  ASSERT_TRUE(QMetaObject::invokeMethod(&target, "pasteWidgetFromClipboard", Qt::DirectConnection));

  ASSERT_EQ(target.curveList().size(), 1U);
  EXPECT_EQ(target.curveList().front().source_name, key2)
      << "the dataset qualifier must bind the paste to dataset 2, not dataset 1's same-topic key";
}

// Same two-dataset setup, but the copied curve's dataset qualifiers are
// stripped before paste (an old/generic copy). Two datasets provide
// "/imu/accel", so the paste must NOT rebind to either — the copied concrete
// key is left as-is, which xmlLoadState then fails to resolve into a curve.
TEST(DockWidgetPlaceholderTest, PlotWidgetPasteOfUnqualifiedAmbiguousCurveDoesNotRebindToFirstDataset) {
  QGuiApplication::clipboard()->clear();

  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset1 = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "run1.mcap"});
  auto dataset2 = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "run2.mcap"});
  ASSERT_TRUE(dataset1.has_value()) << dataset1.error();
  ASSERT_TRUE(dataset2.has_value()) << dataset2.error();
  const PJ::TopicId topic1_id = addScalarTopic(session, *dataset1, "/imu/accel");
  ASSERT_NE(topic1_id, 0U);
  const PJ::TopicId topic2_id = addScalarTopic(session, *dataset2, "/imu/accel");
  ASSERT_NE(topic2_id, 0U);
  const QString key1 = keyForTopic(catalog, topic1_id);
  ASSERT_FALSE(key1.isEmpty());

  PJ::PlotWidget source(&session, &catalog);
  ASSERT_NE(source.addCurve(key1), nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(&source, "copyWidgetToClipboard", Qt::DirectConnection));

  // Strip the dataset qualifiers the source just stamped, simulating a copy
  // made before this identity contract existed (or a hand-edited clipboard).
  QDomDocument clipboard_doc;
  ASSERT_TRUE(clipboard_doc.setContent(QGuiApplication::clipboard()->text()));
  QDomElement curve = clipboard_doc.documentElement().firstChildElement(u"curve"_s);
  ASSERT_FALSE(curve.isNull());
  curve.removeAttribute(u"dataset_id"_s);
  curve.removeAttribute(u"dataset_source"_s);
  QGuiApplication::clipboard()->setText(clipboard_doc.toString(2));

  PJ::PlotWidget target(&session, &catalog);
  ASSERT_TRUE(QMetaObject::invokeMethod(&target, "pasteWidgetFromClipboard", Qt::DirectConnection));

  ASSERT_EQ(target.curveList().size(), 1U);
  EXPECT_EQ(target.curveList().front().source_name, key1)
      << "an unqualified ambiguous topic+field must keep the copied concrete key, not rebind to either dataset";
}

// FIX C: a copied curve is QUALIFIED for a dataset that is gone at paste time, but
// its stale concrete key (from the copy session) happens to still name a LIVE series
// in a different dataset. Because the curve is qualified and its identity fails to
// resolve, rebindClipboardCurveKeys must CLEAR the concrete key so it can never
// collide with the unrelated live series — the curve then drops in xmlLoadState.
TEST(DockWidgetPlaceholderTest, PlotWidgetPasteOfQualifiedMissingDatasetDropsInsteadOfBindingStaleKey) {
  QGuiApplication::clipboard()->clear();

  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "present.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const PJ::TopicId topic_id = addScalarTopic(session, *dataset, "/imu/accel");
  ASSERT_NE(topic_id, 0U);
  const QString live_key = keyForTopic(catalog, topic_id);
  ASSERT_FALSE(live_key.isEmpty());

  PJ::PlotWidget source(&session, &catalog);
  ASSERT_NE(source.addCurve(live_key), nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(&source, "copyWidgetToClipboard", Qt::DirectConnection));

  // Rewrite the clipboard so the curve carries the SAME live concrete key but a
  // dataset qualifier naming an ABSENT source (a copy from a since-closed dataset).
  QDomDocument clipboard_doc;
  ASSERT_TRUE(clipboard_doc.setContent(QGuiApplication::clipboard()->text()));
  QDomElement curve = clipboard_doc.documentElement().firstChildElement(u"curve"_s);
  ASSERT_FALSE(curve.isNull());
  EXPECT_EQ(curve.attribute(u"name"_s), live_key);
  curve.setAttribute(u"dataset_source"_s, u"gone.mcap"_s);
  curve.setAttribute(u"dataset_id"_s, u"999"_s);
  QGuiApplication::clipboard()->setText(clipboard_doc.toString(2));

  PJ::PlotWidget target(&session, &catalog);
  ASSERT_TRUE(QMetaObject::invokeMethod(&target, "pasteWidgetFromClipboard", Qt::DirectConnection));

  EXPECT_TRUE(target.curveList().empty())
      << "a qualified curve whose dataset is gone must drop, not bind its stale key to a live different series";
}

TEST(DockWidgetPlaceholderTest, PlaceholderPasteCreatesPlotFromClipboardXml) {
  QGuiApplication::clipboard()->clear();

  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const PJ::TopicId topic_id = addScalarTopic(session, *dataset, "/imu/accel");
  ASSERT_NE(topic_id, 0U);
  const QString key = keyForTopic(catalog, topic_id);
  ASSERT_FALSE(key.isEmpty());

  PJ::PlotWidget source(&session, &catalog);
  ASSERT_NE(source.addCurve(key), nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(&source, "copyWidgetToClipboard", Qt::DirectConnection));

  PJ::DockWidget target(&session, &catalog);
  int undo_count = 0;
  QObject::connect(&target, &PJ::DockWidget::undoableChange, &target, [&]() { ++undo_count; });

  ASSERT_TRUE(QMetaObject::invokeMethod(&target, "updatePlaceholderPasteAction", Qt::DirectConnection));
  auto* placeholder = target.findChild<PJ::VisualizationPlaceholderWidget*>();
  ASSERT_NE(placeholder, nullptr);
  auto* paste_action = findActionByText(placeholder, u"Paste"_s);
  ASSERT_NE(paste_action, nullptr);
  EXPECT_TRUE(paste_action->isEnabled());

  ASSERT_TRUE(QMetaObject::invokeMethod(&target, "pastePlaceholderWidgetFromClipboard", Qt::DirectConnection));

  ASSERT_NE(target.plotWidget(), nullptr);
  EXPECT_EQ(target.objectWidget(), nullptr);
  ASSERT_EQ(target.plotWidget()->curveList().size(), 1U);
  EXPECT_EQ(target.plotWidget()->curveList().front().source_name, key);
  EXPECT_EQ(undo_count, 1);
}

TEST(DockWidgetPlaceholderTest, ObjectWidgetClipboardPasteRequiresSameFamilyTag) {
  QGuiApplication::clipboard()->clear();

  PJ::DockWidget source;
  auto* source_widget = new FakeClipboardObjectWidget(u"scene2d"_s, u"camera"_s);
  source.setObjectWidget(source_widget);

  PJ::DockWidget same_family_target;
  auto* same_family_widget = new FakeClipboardObjectWidget(u"scene2d"_s, u"old"_s);
  same_family_target.setObjectWidget(same_family_widget);
  int same_family_undo = 0;
  QObject::connect(
      &same_family_target, &PJ::DockWidget::undoableChange, &same_family_target, [&]() { ++same_family_undo; });

  PJ::DockWidget other_family_target;
  auto* other_family_widget = new FakeClipboardObjectWidget(u"scene3d"_s, u"old"_s);
  other_family_target.setObjectWidget(other_family_widget);

  ASSERT_TRUE(QMetaObject::invokeMethod(&source, "copyObjectWidgetToClipboard", Qt::DirectConnection));
  ASSERT_TRUE(QGuiApplication::clipboard()->text().contains("<scene2d"_L1));

  ASSERT_TRUE(QMetaObject::invokeMethod(&same_family_target, "pasteObjectWidgetFromClipboard", Qt::DirectConnection));
  EXPECT_EQ(same_family_widget->loadCount(), 1);
  EXPECT_EQ(same_family_widget->loadedValue(), u"camera"_s);
  EXPECT_EQ(same_family_undo, 1);

  ASSERT_TRUE(QMetaObject::invokeMethod(&other_family_target, "pasteObjectWidgetFromClipboard", Qt::DirectConnection));
  EXPECT_EQ(other_family_widget->loadCount(), 0);
}

TEST(DockWidgetPlaceholderTest, PlaceholderPasteCreatesObjectWidgetFromClipboardXml) {
  QGuiApplication::clipboard()->clear();

  PJ::DockWidget source;
  auto* source_widget = new FakeClipboardObjectWidget(u"scene2d"_s, u"camera"_s);
  source.setObjectWidget(source_widget);
  ASSERT_TRUE(QMetaObject::invokeMethod(&source, "copyObjectWidgetToClipboard", Qt::DirectConnection));

  PJ::DockWidget target;
  FakeClipboardObjectWidget* pasted_widget = nullptr;
  target.setObjectWidgetFactory(
      [&](const QString& kind, const PJ::ObjectDropSeed* seed, QWidget* parent) -> PJ::IDataWidget* {
        if (kind != "scene2d"_L1 || seed != nullptr) {
          return nullptr;
        }
        pasted_widget = new FakeClipboardObjectWidget(kind, u"fresh"_s, parent);
        return pasted_widget;
      });
  int undo_count = 0;
  QObject::connect(&target, &PJ::DockWidget::undoableChange, &target, [&]() { ++undo_count; });

  ASSERT_TRUE(QMetaObject::invokeMethod(&target, "updatePlaceholderPasteAction", Qt::DirectConnection));
  auto* placeholder = target.findChild<PJ::VisualizationPlaceholderWidget*>();
  ASSERT_NE(placeholder, nullptr);
  auto* paste_action = findActionByText(placeholder, u"Paste"_s);
  ASSERT_NE(paste_action, nullptr);
  EXPECT_TRUE(paste_action->isEnabled());

  ASSERT_TRUE(QMetaObject::invokeMethod(&target, "pastePlaceholderWidgetFromClipboard", Qt::DirectConnection));

  ASSERT_NE(pasted_widget, nullptr);
  EXPECT_EQ(target.objectWidget(), pasted_widget);
  EXPECT_EQ(target.plotWidget(), nullptr);
  EXPECT_EQ(pasted_widget->loadCount(), 1);
  EXPECT_EQ(pasted_widget->loadedValue(), u"camera"_s);
  EXPECT_EQ(undo_count, 1);
}

TEST(DockWidgetPlaceholderTest, EmptyPlotDoesNotBroadcastLinkedZoom) {
  PJ::PlotWidget plot;
  int rect_changed_count = 0;
  QObject::connect(&plot, &PJ::PlotWidget::rectChanged, &plot, [&]() { ++rect_changed_count; });

  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          &plot, "onExternallyResized", Qt::DirectConnection, Q_ARG(QRectF, QRectF(0.0, 1.0, 1.0, -1.0))));

  EXPECT_EQ(rect_changed_count, 0);
}

TEST(DockWidgetPlaceholderTest, PlaceholderSplitActionCreatesSiblingDock) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  PJ::PlotDocker docker(u"test"_s, &session, &catalog);

  ASSERT_EQ(docker.plotCount(), 1);
  auto* dock = docker.plotAt(0);
  ASSERT_NE(dock, nullptr);
  auto* placeholder = dock->findChild<PJ::VisualizationPlaceholderWidget*>();
  ASSERT_NE(placeholder, nullptr);
  auto* horizontal_action = findActionByText(placeholder, u"Split Horizontally"_s);
  ASSERT_NE(horizontal_action, nullptr);

  horizontal_action->trigger();

  EXPECT_EQ(docker.plotCount(), 2);
}

TEST_F(DockFocusTest, DropFocusesReceivingDock) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  ASSERT_NE(addScalarTopic(session, *dataset, "/imu/accel"), 0U);
  const auto curves = catalog.curves();
  ASSERT_EQ(curves.size(), 1U);

  PJ::PlotDocker docker(u"test"_s, &session, &catalog);
  docker.show();  // focusedDockWidgetChanged only fires for visible docks.

  auto* dock0 = docker.plotAt(0);
  ASSERT_NE(dock0, nullptr);
  auto* dock1 = splitFrom(docker, dock0, 2);
  ASSERT_NE(dock1, nullptr);

  PJ::DockWidget* focused = nullptr;
  QObject::connect(&docker, &PJ::PlotDocker::dockFocused, [&](PJ::DockWidget* d) { focused = d; });

  // Drop a scalar onto the second dock — it should immediately gain focus so
  // its settings show, rather than waiting for a manual click.
  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          dock1, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{curves[0].name})));

  EXPECT_EQ(focused, dock1);
  EXPECT_EQ(docker.focusedDock(), dock1);
}

TEST_F(DockFocusTest, ObjectDropFocusesReceivingDock) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  auto object_topic = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset,
          .topic_name = "/camera/image_raw/compressed",
          .metadata_json = R"({"builtin_object_type":"kImage"})",
      });
  ASSERT_TRUE(object_topic.has_value()) << object_topic.error();
  catalog.rebuildFromDatastore();
  const auto items = catalog.items();
  ASSERT_EQ(items.size(), 1U);

  PJ::PlotDocker docker(u"test"_s, &session, &catalog);
  docker.setObjectWidgetFactory(
      [](const QString& /*kind*/, const PJ::ObjectDropSeed* /*seed*/, QWidget* parent) -> PJ::IDataWidget* {
        return new FakeObjectWidget(parent);
      });
  docker.show();

  auto* dock0 = docker.plotAt(0);
  ASSERT_NE(dock0, nullptr);
  auto* dock1 = splitFrom(docker, dock0, 2);
  ASSERT_NE(dock1, nullptr);

  PJ::DockWidget* focused = nullptr;
  QObject::connect(&docker, &PJ::PlotDocker::dockFocused, [&](PJ::DockWidget* d) { focused = d; });

  // Dropping an object topic (factory-created widget) focuses its dock too.
  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          dock1, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{items[0].key})));

  ASSERT_NE(dock1->objectWidget(), nullptr);
  EXPECT_EQ(focused, dock1);
  EXPECT_EQ(docker.focusedDock(), dock1);
}

TEST_F(DockFocusTest, ClosingFocusedDockRefocusesPreviouslyFocused) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  PJ::PlotDocker docker(u"test"_s, &session, &catalog);
  docker.show();

  auto* dock0 = docker.plotAt(0);
  ASSERT_NE(dock0, nullptr);
  auto* dock1 = splitFrom(docker, dock0, 2);
  ASSERT_NE(dock1, nullptr);
  auto* dock2 = splitFrom(docker, dock1, 3);
  ASSERT_NE(dock2, nullptr);

  // Establish focus history: … → dock1 → dock2, so the previous is dock1.
  docker.setDockWidgetFocused(dock1);
  docker.setDockWidgetFocused(dock2);
  ASSERT_EQ(docker.focusedDock(), dock2);

  PJ::DockWidget* focused = nullptr;
  QObject::connect(&docker, &PJ::PlotDocker::dockFocused, [&](PJ::DockWidget* d) { focused = d; });

  dock2->closeDockWidget();

  // Focus returns to the previously focused dock (dock1), not merely the first
  // surviving sibling (dock0) — and never stays on the deleted dock.
  EXPECT_EQ(focused, dock1);
  EXPECT_EQ(docker.focusedDock(), dock1);
  EXPECT_EQ(docker.plotCount(), 2);
}

TEST_F(DockFocusTest, ClosingLastRealDockFocusesPlaceholder) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  PJ::PlotDocker docker(u"test"_s, &session, &catalog);
  docker.show();

  auto* dock0 = docker.plotAt(0);
  ASSERT_NE(dock0, nullptr);
  docker.setDockWidgetFocused(dock0);

  PJ::DockWidget* focused = dock0;
  QObject::connect(&docker, &PJ::PlotDocker::dockFocused, [&](PJ::DockWidget* d) { focused = d; });

  dock0->closeDockWidget();

  // A fresh placeholder is created and focused; it is not the deleted dock,
  // so the panel falls back to its empty page instead of stale settings.
  ASSERT_EQ(docker.plotCount(), 1);
  EXPECT_NE(focused, dock0);
  EXPECT_EQ(focused, docker.plotAt(0));
}

TEST_F(DockFocusTest, DropOntoAlreadyFocusedPlaceholderRefreshes) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  ASSERT_NE(addScalarTopic(session, *dataset, "/imu/accel"), 0U);
  const auto curves = catalog.curves();
  ASSERT_EQ(curves.size(), 1U);

  PJ::PlotDocker docker(u"test"_s, &session, &catalog);
  docker.show();

  // Close the only widget: a fresh placeholder is created and *takes focus*.
  auto* dock0 = docker.plotAt(0);
  ASSERT_NE(dock0, nullptr);
  docker.setDockWidgetFocused(dock0);
  dock0->closeDockWidget();
  auto* placeholder = docker.plotAt(0);
  ASSERT_NE(placeholder, nullptr);
  ASSERT_EQ(docker.focusedDock(), placeholder);

  PJ::DockWidget* focused = nullptr;
  QObject::connect(&docker, &PJ::PlotDocker::dockFocused, [&](PJ::DockWidget* d) { focused = d; });

  // Dropping onto the already-focused placeholder populates it; ADS won't emit a
  // focus change (focus didn't move), but the panel must still refresh.
  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          placeholder, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{curves[0].name})));

  ASSERT_NE(placeholder->plotWidget(), nullptr);
  EXPECT_EQ(focused, placeholder);
}

TEST(DockWidgetPlaceholderTest, ScalarDropConvertsPlaceholderToPlot) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const PJ::TopicId topic_id = addScalarTopic(session, *dataset, "/imu/accel");
  ASSERT_NE(topic_id, 0U);

  const auto curves = catalog.curves();
  ASSERT_EQ(curves.size(), 1U);

  PJ::PlotDocker docker(u"test"_s, &session, &catalog);
  auto* dock = docker.plotAt(0);
  ASSERT_NE(dock, nullptr);

  const bool invoked = QMetaObject::invokeMethod(
      dock, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{curves[0].name}));
  ASSERT_TRUE(invoked);

  ASSERT_NE(dock->plotWidget(), nullptr);
  EXPECT_EQ(dock->objectWidget(), nullptr);
  EXPECT_EQ(dock->plotWidget()->curveList().size(), 1U);
}

// A single-column topic of an arbitrary primitive, via the schema path (the
// registerScalarSeries shortcut is float-only).
PJ::TopicId addTypedTopic(
    PJ::SessionManager& session, PJ::DatasetId dataset_id, std::string_view topic_name, PJ::PrimitiveType type) {
  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto schema_or = writer.registerSchema(std::string(topic_name), PJ::makePrimitive("value", type));
  EXPECT_TRUE(schema_or.has_value());
  PJ::TopicDescriptor descriptor;
  descriptor.name = std::string(topic_name);
  descriptor.schema_id = *schema_or;
  auto topic_or = writer.registerTopic(dataset_id, descriptor);
  EXPECT_TRUE(topic_or.has_value());
  EXPECT_TRUE(writer.bindTopicWriter(*topic_or).has_value());
  EXPECT_TRUE(writer.beginRow(*topic_or, 100).has_value());
  if (type == PJ::PrimitiveType::kString) {
    writer.set(*topic_or, 0, std::string_view{"IDLE"});
  } else {
    writer.set(*topic_or, 0, static_cast<int64_t>(3));
  }
  EXPECT_TRUE(writer.finishRow(*topic_or).has_value());
  EXPECT_FALSE(session.commitChunks(writer.flushAll()).empty());
  return *topic_or;
}

// An integer field is discrete AND plottable; plottable wins the empty-tile
// tie, so the drop builds a plot — never a state-transitions strip.
TEST(DockWidgetPlaceholderTest, IntegerDropConvertsPlaceholderToPlotNotStrip) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value());
  ASSERT_NE(addTypedTopic(session, *dataset, "/robot/mode", PJ::PrimitiveType::kInt64), 0U);
  const auto curves = catalog.curves();
  ASSERT_EQ(curves.size(), 1U);  // integer fields ARE plottable curves

  PJ::PlotDocker docker(u"test"_s, &session, &catalog);
  PJ::PlaybackEngine playback;
  docker.setObjectWidgetFactory(
      [&](const QString& kind, const PJ::ObjectDropSeed*, QWidget* parent) -> PJ::IDataWidget* {
        if (kind != u"state_transitions"_s) {
          return nullptr;
        }
        return new PJ::StateTransitionsDockWidget(&session, &catalog, &playback, parent);
      });
  auto* dock = docker.plotAt(0);
  ASSERT_NE(dock, nullptr);
  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          dock, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{curves[0].name})));

  EXPECT_NE(dock->plotWidget(), nullptr);
  EXPECT_EQ(dock->objectWidget(), nullptr);
}

// String-only drop mounts the strip (nothing plottable); a follow-up integer
// drop routes through tryAcceptSeriesKeys into the SAME mounted strip.
TEST(DockWidgetPlaceholderTest, IntegerDropOntoMountedStripAddsRow) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value());
  ASSERT_NE(addTypedTopic(session, *dataset, "/robot/state", PJ::PrimitiveType::kString), 0U);
  ASSERT_NE(addTypedTopic(session, *dataset, "/robot/mode", PJ::PrimitiveType::kInt64), 0U);
  QString string_key;
  QString int_key;
  for (const PJ::CatalogItem& item : catalog.items()) {
    (item.topic_name == u"/robot/state"_s ? string_key : int_key) = item.key;
  }
  ASSERT_FALSE(string_key.isEmpty());
  ASSERT_FALSE(int_key.isEmpty());

  PJ::PlotDocker docker(u"test"_s, &session, &catalog);
  PJ::PlaybackEngine playback;
  docker.setObjectWidgetFactory(
      [&](const QString& kind, const PJ::ObjectDropSeed*, QWidget* parent) -> PJ::IDataWidget* {
        if (kind != u"state_transitions"_s) {
          return nullptr;
        }
        return new PJ::StateTransitionsDockWidget(&session, &catalog, &playback, parent);
      });
  auto* dock = docker.plotAt(0);
  ASSERT_NE(dock, nullptr);

  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          dock, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{string_key})));
  ASSERT_NE(dock->objectWidget(), nullptr);
  EXPECT_EQ(dock->plotWidget(), nullptr);
  auto* mounted = qobject_cast<PJ::StateTransitionsDockWidget*>(dock->objectWidget()->widget());
  ASSERT_NE(mounted, nullptr);
  EXPECT_EQ(mounted->controller()->rowCount(), 1);

  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          dock, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{int_key})));
  EXPECT_EQ(mounted->controller()->rowCount(), 2);
}

// Shared setup for the DockToolbar inline-rename behaviour: a PlotDocker with one
// dock, exposing that dock's toolbar, title label, and (hidden) rename editor.
class DockToolbarRenameTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dock_ = docker_.plotAt(0);
    ASSERT_NE(dock_, nullptr);
    toolbar_ = dock_->toolBar();
    ASSERT_NE(toolbar_, nullptr);
    label_ = toolbar_->label();
    edit_ = toolbar_->findChild<QLineEdit*>(u"lineEditRename"_s);
    ASSERT_NE(label_, nullptr);
    ASSERT_NE(edit_, nullptr);
  }

  // Double-click the label through its public seam (eventFilter), entering edit
  // mode. Asserts the event is consumed.
  void doubleClickLabel() {
    QMouseEvent dbl(
        QEvent::MouseButtonDblClick, QPointF(1, 1), QPointF(1, 1), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    EXPECT_TRUE(toolbar_->eventFilter(label_, &dbl));
  }

  PJ::SessionManager session_;
  PJ::CatalogModel catalog_{&session_};
  PJ::PlotDocker docker_{u"test"_s, &session_, &catalog_};
  PJ::DockWidget* dock_ = nullptr;
  PJ::DockToolbar* toolbar_ = nullptr;
  QLabel* label_ = nullptr;
  QLineEdit* edit_ = nullptr;
};

TEST_F(DockToolbarRenameTest, DoubleClickEntersInlineEditAndEnterCommits) {
  // Double-clicking the title label swaps it for an in-place QLineEdit (no modal
  // dialog); pressing Enter writes the edited text back and emits titleChanged.
  label_->setText(u"original"_s);

  QString emitted_title;
  int emit_count = 0;
  QObject::connect(toolbar_, &PJ::DockToolbar::titleChanged, toolbar_, [&](const QString& title) {
    emitted_title = title;
    ++emit_count;
  });

  // Enter inline edit mode: label hidden, edit shown and seeded with the text.
  doubleClickLabel();
  EXPECT_TRUE(label_->isHidden());
  EXPECT_FALSE(edit_->isHidden());
  EXPECT_EQ(edit_->text(), u"original"_s);

  // Edit the text and press Enter -> commit.
  edit_->setText(u"renamed"_s);
  QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
  QApplication::sendEvent(edit_, &enter);

  EXPECT_FALSE(label_->isHidden());
  EXPECT_TRUE(edit_->isHidden());
  EXPECT_EQ(label_->text(), u"renamed"_s);
  EXPECT_EQ(dock_->name(), u"renamed"_s);
  EXPECT_EQ(emit_count, 1);
  EXPECT_EQ(emitted_title, u"renamed"_s);
}

TEST_F(DockToolbarRenameTest, InlineEditEscapeRevertsWithoutRenaming) {
  // Escape (and focus loss) cancels the inline edit, leaving the original name —
  // matching the tab-rename behaviour.
  label_->setText(u"original"_s);

  int emit_count = 0;
  QObject::connect(toolbar_, &PJ::DockToolbar::titleChanged, toolbar_, [&](const QString&) { ++emit_count; });

  doubleClickLabel();
  edit_->setText(u"discarded"_s);

  QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
  EXPECT_TRUE(toolbar_->eventFilter(edit_, &escape));

  EXPECT_FALSE(label_->isHidden());
  EXPECT_TRUE(edit_->isHidden());
  EXPECT_EQ(label_->text(), u"original"_s);
  EXPECT_EQ(dock_->name(), u"original"_s);
  EXPECT_EQ(emit_count, 0);
}

TEST_F(DockToolbarRenameTest, InlineEditDefaultsToCompactWidthAndGrowsWithText) {
  // The editor is a compact 300 px by default (not the full bar width) and only
  // grows when the text would not fit.
  toolbar_->resize(1200, 30);  // wide geometry so the growth cap is large
  label_->setText(u"short"_s);

  doubleClickLabel();
  // A short name fits in the default width, so the editor stays at exactly 300 px
  // (well under the wide toolbar).
  EXPECT_EQ(edit_->width(), 300);

  // Typing a long name grows the editor past the default, but not unbounded.
  edit_->setText(QString(200, QLatin1Char('W')));
  EXPECT_GT(edit_->width(), 300);
  EXPECT_LE(edit_->width(), toolbar_->width());
}

TEST_F(DockToolbarRenameTest, TitleBarUsesGrabCursorWithButtonAndEditorOverrides) {
  // The bar hints it is draggable with an open-hand cursor; clickable buttons
  // and the rename editor keep their own (pointing-hand / text) cursors so the
  // inherited grab hand doesn't bleed onto them.
  EXPECT_EQ(toolbar_->cursor().shape(), Qt::OpenHandCursor);
  EXPECT_EQ(toolbar_->buttonClose()->cursor().shape(), Qt::PointingHandCursor);
  EXPECT_EQ(toolbar_->buttonFullscreen()->cursor().shape(), Qt::PointingHandCursor);
  EXPECT_EQ(toolbar_->buttonSplitHorizontal()->cursor().shape(), Qt::PointingHandCursor);
  EXPECT_EQ(toolbar_->buttonSplitVertical()->cursor().shape(), Qt::PointingHandCursor);
  EXPECT_EQ(edit_->cursor().shape(), Qt::IBeamCursor);
}

TEST(DockWidgetPlaceholderTest, CurveListChangedSeesDisplayTitleAfterCatalogKeyAdd) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const PJ::TopicId topic_id = addScalarTopic(session, *dataset, "/imu/accel");
  ASSERT_NE(topic_id, 0U);

  const auto curves = catalog.curves();
  ASSERT_EQ(curves.size(), 1U);
  ASSERT_NE(curves[0].name, u"imu/accel/value"_s);

  PJ::PlotWidget plot(&session, &catalog);
  QStringList observed_titles;
  QObject::connect(&plot, &PJ::PlotWidget::curveListChanged, &plot, [&]() {
    for (const auto& info : plot.curveList()) {
      if (info.curve != nullptr) {
        observed_titles << info.curve->title().text();
      }
    }
  });

  const auto* info = plot.addCurve(curves[0].name);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->source_name, curves[0].name);
  EXPECT_EQ(info->curve->title().text(), u"imu/accel/value"_s);
  EXPECT_EQ(observed_titles, QStringList{u"imu/accel/value"_s});
}

TEST(DockWidgetPlaceholderTest, ImageObjectDropConvertsPlaceholderToMedia2D) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  auto object_topic = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset,
          .topic_name = "/camera/image_raw/compressed",
          .metadata_json = R"({"builtin_object_type":"kImage"})",
      });
  ASSERT_TRUE(object_topic.has_value()) << object_topic.error();
  catalog.rebuildFromDatastore();

  const auto items = catalog.items();
  ASSERT_EQ(items.size(), 1U);
  ASSERT_TRUE(PJ::isObjectTopic(items[0]));
  ASSERT_EQ(PJ::asObjectTopic(items[0])->object_type, PJ::sdk::BuiltinObjectType::kImage);

  PJ::PlotDocker docker(u"test"_s, &session, &catalog);
  bool factory_called = false;
  docker.setObjectWidgetFactory(
      [&](const QString& kind, const PJ::ObjectDropSeed* seed, QWidget* parent) -> PJ::IDataWidget* {
        factory_called = true;
        // Drop path: empty kind, a seed carrying the dropped topic.
        EXPECT_TRUE(kind.isEmpty());
        EXPECT_NE(seed, nullptr);
        if (seed != nullptr) {
          EXPECT_EQ(seed->topic_id, *object_topic);
          EXPECT_EQ(seed->object_type, PJ::sdk::BuiltinObjectType::kImage);
          // Single dataset loaded -> title drops the redundant "drive.mcap/" prefix.
          EXPECT_EQ(seed->title, u"/camera/image_raw/compressed"_s);
        }
        return new FakeObjectWidget(parent);
      });
  auto* dock = docker.plotAt(0);
  ASSERT_NE(dock, nullptr);

  const bool invoked = QMetaObject::invokeMethod(
      dock, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{items[0].key}));
  ASSERT_TRUE(invoked);

  EXPECT_EQ(dock->plotWidget(), nullptr);
  EXPECT_TRUE(factory_called);
  EXPECT_NE(dock->objectWidget(), nullptr);
  // Dragging a topic onto the placeholder does NOT rename the dock; it keeps "...".
  EXPECT_EQ(dock->name(), u"..."_s);
}

TEST(DockWidgetPlaceholderTest, ClearObjectContentRestoresPlaceholder) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  auto object_topic = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset,
          .topic_name = "/camera/image_raw/compressed",
          .metadata_json = R"({"builtin_object_type":"kImage"})",
      });
  ASSERT_TRUE(object_topic.has_value()) << object_topic.error();
  catalog.rebuildFromDatastore();

  const auto items = catalog.items();
  ASSERT_EQ(items.size(), 1U);

  PJ::PlotDocker docker(u"test"_s, &session, &catalog);
  docker.setObjectWidgetFactory([](const QString&, const PJ::ObjectDropSeed*, QWidget* parent) -> PJ::IDataWidget* {
    return new FakeObjectWidget(parent);
  });
  auto* dock = docker.plotAt(0);
  ASSERT_NE(dock, nullptr);
  int undoable_count = 0;
  QObject::connect(dock, &PJ::DockWidget::undoableChange, dock, [&]() { ++undoable_count; });

  const bool drop_invoked = QMetaObject::invokeMethod(
      dock, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{items[0].key}));
  ASSERT_TRUE(drop_invoked);
  ASSERT_NE(dock->objectWidget(), nullptr);
  EXPECT_EQ(undoable_count, 1);

  const bool clear_invoked = QMetaObject::invokeMethod(dock, "clearToPlaceholder", Qt::DirectConnection);
  ASSERT_TRUE(clear_invoked);

  EXPECT_EQ(dock->plotWidget(), nullptr);
  EXPECT_EQ(dock->objectWidget(), nullptr);
  EXPECT_NE(dock->findChild<PJ::VisualizationPlaceholderWidget*>(), nullptr);
  EXPECT_EQ(dock->name(), u"..."_s);
  EXPECT_EQ(undoable_count, 2);
}

TEST(DockWidgetPlaceholderTest, RestoreRoutesObjectWidgetTagsToFactoryByKind) {
  // Regression: a saved <scene2d> dock (and <scene3d>) must be recognized on
  // restore and routed to the object-widget factory keyed by its XML tag. Before
  // the generic-restore fix the layout parser collected only <plot> and
  // <scene3d>, so <scene2d> docks silently vanished on reload.
  for (const QString& kind : {u"scene2d"_s, u"scene3d"_s}) {
    PJ::SessionManager session;
    PJ::CatalogModel catalog(&session);
    PJ::PlotDocker docker(u"test"_s, &session, &catalog);

    QString seen_kind;
    bool seed_was_null = false;
    docker.setObjectWidgetFactory(
        [&](const QString& factory_kind, const PJ::ObjectDropSeed* seed, QWidget* parent) -> PJ::IDataWidget* {
          seen_kind = factory_kind;
          seed_was_null = (seed == nullptr);
          return new FakeObjectWidget(parent);
        });

    QDomDocument doc;
    const QString xml = QStringLiteral(
                            "<Tab id=\"t1\" containers=\"1\"><Container><DockArea id=\"a1\" name=\"View\">"
                            "<%1/></DockArea></Container></Tab>")
                            .arg(kind);
    ASSERT_TRUE(doc.setContent(xml));

    EXPECT_TRUE(docker.xmlLoadState(doc.documentElement()));
    EXPECT_EQ(seen_kind, kind);
    EXPECT_TRUE(seed_was_null);  // restore passes a null seed (the dock reloads from XML)
    auto* dock = docker.plotAt(0);
    ASSERT_NE(dock, nullptr);
    EXPECT_NE(dock->objectWidget(), nullptr);
    EXPECT_EQ(dock->plotWidget(), nullptr);
  }
}

TEST(DockWidgetPlaceholderTest, ObjectPayloadFailureFailsDockerRestore) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  PJ::PlotDocker docker(u"test"_s, &session, &catalog);
  docker.setObjectWidgetFactory([](const QString&, const PJ::ObjectDropSeed*, QWidget* parent) -> PJ::IDataWidget* {
    return new FakeRejectingObjectWidget(parent);
  });

  QDomDocument doc;
  ASSERT_TRUE(
      doc.setContent(uR"(
      <Tab id="t1" containers="1"><Container><DockArea id="a1" name="View">
      <scene2d version="1"/></DockArea></Container></Tab>)"_s));

  EXPECT_FALSE(docker.xmlLoadState(doc.documentElement()))
      << "a partial object restore must propagate to the transactional caller";
}

TEST(DockWidgetPlaceholderTest, PlaceholderAcceptsCatalogDragMoveAndIconDrop) {
  TestPlaceholderWidget placeholder;
  int drop_count = 0;
  QStringList dropped_keys;
  QObject::connect(
      &placeholder, &PJ::VisualizationPlaceholderWidget::catalogItemsDropped, &placeholder,
      [&](const QStringList& keys) {
        ++drop_count;
        dropped_keys = keys;
      });

  QMimeData mime_data;
  mime_data.setData(
      PJ::CurveTreeView::catalogItemsMimeType(),
      PJ::CurveTreeView::encodeCatalogKeys(QStringList{u"dataset:/camera/image"_s}));
  QDragEnterEvent drag_enter(QPoint(1, 1), Qt::CopyAction, &mime_data, Qt::LeftButton, Qt::NoModifier);
  placeholder.sendDragEnter(&drag_enter);
  EXPECT_TRUE(drag_enter.isAccepted());

  QDragMoveEvent drag_move(QPoint(1, 1), Qt::CopyAction, &mime_data, Qt::LeftButton, Qt::NoModifier);
  placeholder.sendDragMove(&drag_move);
  EXPECT_TRUE(drag_move.isAccepted());

  QToolButton* icon_button = nullptr;
  for (auto* button : placeholder.findChildren<QToolButton*>()) {
    if (button->isEnabled()) {
      icon_button = button;
      break;
    }
  }
  ASSERT_NE(icon_button, nullptr);

  QDragMoveEvent icon_drag_move(QPoint(1, 1), Qt::CopyAction, &mime_data, Qt::LeftButton, Qt::NoModifier);
  EXPECT_TRUE(placeholder.sendFilteredEvent(icon_button, &icon_drag_move));
  EXPECT_TRUE(icon_drag_move.isAccepted());

  QDropEvent icon_drop(QPointF(1, 1), Qt::CopyAction, &mime_data, Qt::LeftButton, Qt::NoModifier);
  EXPECT_TRUE(placeholder.sendFilteredEvent(icon_button, &icon_drop));
  EXPECT_TRUE(icon_drop.isAccepted());
  EXPECT_EQ(drop_count, 1);
  EXPECT_EQ(dropped_keys, QStringList{u"dataset:/camera/image"_s});
}

TEST(VisualizationPlaceholderTest, ThreeDIconIsEnabledAndNamed) {
  // The 3D icon used to render disabled (greyed); it must now be enabled and
  // full-tinted like the others. All three carry stable objectNames.
  TestPlaceholderWidget placeholder;
  for (const char* name : {"buttonVizPlot", "buttonVizScene2D", "buttonVizScene3D"}) {
    auto* button = iconButton(&placeholder, name);
    ASSERT_NE(button, nullptr) << name;
    EXPECT_TRUE(button->isEnabled()) << name;
  }
}

TEST(VisualizationPlaceholderTest, IconClicksEmitVisualizationRequested) {
  TestPlaceholderWidget placeholder;
  std::vector<PJ::VisualizationKind> requested;
  QObject::connect(
      &placeholder, &PJ::VisualizationPlaceholderWidget::visualizationRequested, &placeholder,
      [&](PJ::VisualizationKind kind) { requested.push_back(kind); });

  iconButton(&placeholder, "buttonVizPlot")->click();
  iconButton(&placeholder, "buttonVizScene2D")->click();
  iconButton(&placeholder, "buttonVizScene3D")->click();

  ASSERT_EQ(requested.size(), 3U);
  EXPECT_EQ(requested[0], PJ::VisualizationKind::kPlot);
  EXPECT_EQ(requested[1], PJ::VisualizationKind::kScene2D);
  EXPECT_EQ(requested[2], PJ::VisualizationKind::kScene3D);
}

TEST(VisualizationPlaceholderTest, RealMouseClickPassesThroughDragFilterAndEmits) {
  // The icon buttons carry the placeholder's drag event filter (for catalog
  // drops). A plain mouse click must still reach the button and emit the request
  // — i.e. the filter must not swallow press/release. click() bypasses the
  // filter, so drive real QMouseEvents through it.
  TestPlaceholderWidget placeholder;
  placeholder.resize(400, 200);
  placeholder.show();
  auto* button = iconButton(&placeholder, "buttonVizScene3D");
  ASSERT_NE(button, nullptr);
  ASSERT_TRUE(button->isEnabled());

  std::optional<PJ::VisualizationKind> got;
  QObject::connect(
      &placeholder, &PJ::VisualizationPlaceholderWidget::visualizationRequested, &placeholder,
      [&](PJ::VisualizationKind kind) { got = kind; });

  const QPointF local = button->rect().center();
  const QPointF global = button->mapToGlobal(local.toPoint());
  QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(button, &press);
  QApplication::sendEvent(button, &release);

  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(*got, PJ::VisualizationKind::kScene3D);
}

TEST(DockWidgetPlaceholderTest, PlotIconClickConvertsPlaceholderToEmptyPlot) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  PJ::PlotDocker docker(u"test"_s, &session, &catalog);
  auto* dock = docker.plotAt(0);
  ASSERT_NE(dock, nullptr);
  auto* placeholder = dock->findChild<PJ::VisualizationPlaceholderWidget*>();
  ASSERT_NE(placeholder, nullptr);

  iconButton(placeholder, "buttonVizPlot")->click();

  ASSERT_NE(dock->plotWidget(), nullptr);
  EXPECT_EQ(dock->objectWidget(), nullptr);
  EXPECT_EQ(dock->plotWidget()->curveList().size(), 0U);  // empty plot, no curves
}

TEST(DockWidgetPlaceholderTest, SceneIconClickRequestsObjectFamilyWithoutBuildingWidget) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  PJ::PlotDocker docker(u"test"_s, &session, &catalog);
  auto* dock = docker.plotAt(0);
  ASSERT_NE(dock, nullptr);

  // Both the DockWidget signal and the PlotDocker re-emit must carry the dock +
  // family, so MainWindow (the only scene-kind-aware module) can build the dock.
  PJ::DockWidget* dock_signal_arg = nullptr;
  PJ::VisualizationKind dock_kind = PJ::VisualizationKind::kPlot;
  int dock_count = 0;
  QObject::connect(dock, &PJ::DockWidget::objectFamilyRequested, dock, [&](PJ::DockWidget* d, PJ::VisualizationKind k) {
    dock_signal_arg = d;
    dock_kind = k;
    ++dock_count;
  });
  PJ::VisualizationKind docker_kind = PJ::VisualizationKind::kPlot;
  int docker_count = 0;
  QObject::connect(
      &docker, &PJ::PlotDocker::objectFamilyRequested, &docker, [&](PJ::DockWidget* /*d*/, PJ::VisualizationKind k) {
        docker_kind = k;
        ++docker_count;
      });

  auto* placeholder = dock->findChild<PJ::VisualizationPlaceholderWidget*>();
  ASSERT_NE(placeholder, nullptr);
  iconButton(placeholder, "buttonVizScene3D")->click();

  EXPECT_EQ(dock_count, 1);
  EXPECT_EQ(dock_signal_arg, dock);
  EXPECT_EQ(dock_kind, PJ::VisualizationKind::kScene3D);
  EXPECT_EQ(docker_count, 1);
  EXPECT_EQ(docker_kind, PJ::VisualizationKind::kScene3D);
  // The dock does not build the widget itself — that stays MainWindow's job.
  EXPECT_EQ(dock->objectWidget(), nullptr);
  EXPECT_EQ(dock->plotWidget(), nullptr);
}

TEST(DockWidgetPlaceholderTest, AdoptObjectWidgetInstallsEmptyObjectWidget) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  PJ::PlotDocker docker(u"test"_s, &session, &catalog);
  auto* dock = docker.plotAt(0);
  ASSERT_NE(dock, nullptr);
  int undoable_count = 0;
  QObject::connect(dock, &PJ::DockWidget::undoableChange, dock, [&]() { ++undoable_count; });

  auto* widget = new FakeObjectWidget();
  dock->adoptObjectWidget(widget);

  EXPECT_EQ(dock->objectWidget(), static_cast<PJ::IDataWidget*>(widget));
  EXPECT_EQ(dock->plotWidget(), nullptr);
  EXPECT_EQ(dock->name(), u"..."_s);
  EXPECT_EQ(undoable_count, 1);
}

TEST(DockWidgetPlaceholderTest, AdoptNullObjectWidgetRevertsToPlaceholder) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  PJ::PlotDocker docker(u"test"_s, &session, &catalog);
  auto* dock = docker.plotAt(0);
  ASSERT_NE(dock, nullptr);

  // A null build (unknown kind / factory refusal) must leave a usable placeholder
  // rather than a blank dock.
  dock->adoptObjectWidget(nullptr);

  EXPECT_EQ(dock->objectWidget(), nullptr);
  EXPECT_EQ(dock->plotWidget(), nullptr);
  EXPECT_NE(dock->findChild<PJ::VisualizationPlaceholderWidget*>(), nullptr);
}

TEST(DockWidgetPlaceholderTest, EmptyObjectWidgetSurvivesLayoutSaveRestore) {
  // Corner case behind the click-to-create-empty path: an empty object dock must
  // round-trip through xmlSaveState/xmlLoadState (undo/redo + layout save). It must
  // NOT be skipped on save, and must come back as an object widget, not a plot.
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);

  PJ::PlotDocker docker(u"test"_s, &session, &catalog);
  docker.setObjectWidgetFactory(
      [](const QString& kind, const PJ::ObjectDropSeed*, QWidget* parent) -> PJ::IDataWidget* {
        return new FakeStatefulObjectWidget(kind.isEmpty() ? u"scene3d"_s : kind, parent);
      });
  auto* dock = docker.plotAt(0);
  ASSERT_NE(dock, nullptr);
  dock->adoptObjectWidget(new FakeStatefulObjectWidget(u"scene3d"_s));
  ASSERT_NE(dock->objectWidget(), nullptr);

  QDomDocument doc;
  const QDomElement saved = docker.xmlSaveState(doc);
  doc.appendChild(saved);
  EXPECT_FALSE(saved.elementsByTagName(u"scene3d"_s).isEmpty()) << "empty object dock was skipped on save";

  PJ::PlotDocker restored(u"test2"_s, &session, &catalog);
  restored.setObjectWidgetFactory(
      [](const QString& kind, const PJ::ObjectDropSeed*, QWidget* parent) -> PJ::IDataWidget* {
        return new FakeStatefulObjectWidget(kind, parent);
      });
  ASSERT_TRUE(restored.xmlLoadState(saved));
  auto* restored_dock = restored.plotAt(0);
  ASSERT_NE(restored_dock, nullptr);
  EXPECT_NE(restored_dock->objectWidget(), nullptr);
  EXPECT_EQ(restored_dock->plotWidget(), nullptr);
}

TEST(DockLayoutSizeTest, RestorePreservesSplitterProportions) {
  // Save an asymmetric split, then restore it into a *fresh* docker that has not
  // been shown/sized yet (the path TabbedPlotWidget takes: every tab gets a brand
  // new PlotDocker, then xmlLoadState runs before it is laid out). The restored
  // splitter must keep the saved proportions once shown.
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);

  PJ::PlotDocker source(u"src"_s, &session, &catalog);
  source.resize(1000, 600);
  source.show();
  QApplication::processEvents();

  auto* dock0 = source.plotAt(0);
  ASSERT_NE(dock0, nullptr);
  auto* dock1 = splitFrom(source, dock0, 2);
  ASSERT_NE(dock1, nullptr);

  // Force a known, strongly asymmetric 80/20 horizontal split.
  QSplitter* src_splitter = nullptr;
  for (QSplitter* splitter : source.findChildren<QSplitter*>()) {
    if (splitter->count() == 2) {
      src_splitter = splitter;
      break;
    }
  }
  ASSERT_NE(src_splitter, nullptr);
  src_splitter->setSizes({800, 200});
  QApplication::processEvents();

  QDomDocument doc;
  const QDomElement saved = source.xmlSaveState(doc);

  // Restore into a fresh docker, never shown before xmlLoadState.
  PJ::PlotDocker restored(u"dst"_s, &session, &catalog);
  ASSERT_TRUE(restored.xmlLoadState(saved));
  restored.resize(1000, 600);
  restored.show();
  QApplication::processEvents();

  QSplitter* dst_splitter = nullptr;
  for (QSplitter* splitter : restored.findChildren<QSplitter*>()) {
    if (splitter->count() == 2) {
      dst_splitter = splitter;
      break;
    }
  }
  ASSERT_NE(dst_splitter, nullptr);
  const QList<int> sizes = dst_splitter->sizes();
  const double total = static_cast<double>(sizes[0] + sizes[1]);
  ASSERT_GT(total, 0.0);
  const double ratio0 = static_cast<double>(sizes[0]) / total;
  // The saved layout is 80/20. The bug collapsed it toward 50/50 because the
  // restore measured widget geometry before the docker was laid out.
  EXPECT_NEAR(ratio0, 0.8, 0.02) << "restored sizes=" << sizes[0] << "," << sizes[1];
}

TEST(DockLayoutSizeTest, RestorePreservesThreeWaySplitterProportions) {
  // A 3-way asymmetric split exercises a multi-child splitter, where geometry-based
  // restore distorts every pane, not just one boundary.
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);

  PJ::PlotDocker source(u"src"_s, &session, &catalog);
  source.resize(1200, 600);
  source.show();
  QApplication::processEvents();

  auto* dock0 = source.plotAt(0);
  ASSERT_NE(dock0, nullptr);
  auto* dock1 = splitFrom(source, dock0, 2);
  ASSERT_NE(dock1, nullptr);
  auto* dock2 = splitFrom(source, dock1, 3);
  ASSERT_NE(dock2, nullptr);

  QSplitter* src_splitter = nullptr;
  for (QSplitter* splitter : source.findChildren<QSplitter*>()) {
    if (splitter->count() == 3) {
      src_splitter = splitter;
      break;
    }
  }
  ASSERT_NE(src_splitter, nullptr);
  src_splitter->setSizes({600, 360, 240});  // 50 / 30 / 20
  QApplication::processEvents();

  QDomDocument doc;
  const QDomElement saved = source.xmlSaveState(doc);

  PJ::PlotDocker restored(u"dst"_s, &session, &catalog);
  ASSERT_TRUE(restored.xmlLoadState(saved));
  restored.resize(1200, 600);
  restored.show();
  QApplication::processEvents();

  QSplitter* dst_splitter = nullptr;
  for (QSplitter* splitter : restored.findChildren<QSplitter*>()) {
    if (splitter->count() == 3) {
      dst_splitter = splitter;
      break;
    }
  }
  ASSERT_NE(dst_splitter, nullptr);
  const QList<int> sizes = dst_splitter->sizes();
  const double total = static_cast<double>(sizes[0] + sizes[1] + sizes[2]);
  ASSERT_GT(total, 0.0);
  EXPECT_NEAR(sizes[0] / total, 0.50, 0.02) << "sizes=" << sizes[0] << "," << sizes[1] << "," << sizes[2];
  EXPECT_NEAR(sizes[1] / total, 0.30, 0.02) << "sizes=" << sizes[0] << "," << sizes[1] << "," << sizes[2];
  EXPECT_NEAR(sizes[2] / total, 0.20, 0.02) << "sizes=" << sizes[0] << "," << sizes[1] << "," << sizes[2];
}

TEST(DockWidgetPlaceholderTest, FirstObjectTopicAddedFiresOnceForAdoptedEmptyWidget) {
  // Streaming seed: an empty click-created dock has no data to play, so adopt does
  // NOT seed playback. When its FIRST topic is dropped (absorbed in place), the
  // dock signals firstObjectTopicAdded so the host can seed live playback — and it
  // must fire exactly once (subsequent topics are normal additions).
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "stream.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  registerImageObjectTopic(session, *dataset, "/camera/a");
  registerImageObjectTopic(session, *dataset, "/camera/b");
  catalog.rebuildFromDatastore();
  const auto items = catalog.items();
  ASSERT_EQ(items.size(), 2U);

  PJ::PlotDocker docker(u"test"_s, &session, &catalog);
  // A factory is required for the object-drop path to reach the in-place "offer",
  // even though the adopted widget is the one that absorbs the topics.
  docker.setObjectWidgetFactory([](const QString&, const PJ::ObjectDropSeed*, QWidget* parent) -> PJ::IDataWidget* {
    return new FakeAcceptingObjectWidget(parent);
  });
  auto* dock = docker.plotAt(0);
  ASSERT_NE(dock, nullptr);
  dock->adoptObjectWidget(new FakeAcceptingObjectWidget());

  int seed_count = 0;
  QObject::connect(dock, &PJ::DockWidget::firstObjectTopicAdded, dock, [&]() { ++seed_count; });

  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          dock, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{items[0].key})));
  EXPECT_EQ(seed_count, 1);
  // Dragging a topic must NOT rename the dock — it keeps its default "..." name
  // (the user renames it explicitly via the title bar if they want to).
  EXPECT_EQ(dock->name(), u"..."_s);

  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          dock, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{items[1].key})));
  EXPECT_EQ(seed_count, 1);           // not re-fired for the second topic
  EXPECT_EQ(dock->name(), u"..."_s);  // still the default name after a second drop
}

TEST(DockWidgetPlaceholderTest, IncompatibleObjectDropOntoCommittedObjectWidgetIsRejected) {
  // A committed object dock that refuses the dropped topic (a different family,
  // e.g. an image dropped on a 3D view or a pointcloud on a 2D view) must keep
  // its widget — NOT be replaced by a factory-created one of the other family.
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  registerImageObjectTopic(session, *dataset, "/camera/image");
  catalog.rebuildFromDatastore();
  const auto items = catalog.items();
  ASSERT_EQ(items.size(), 1U);

  PJ::PlotDocker docker(u"test"_s, &session, &catalog);
  int factory_calls = 0;
  docker.setObjectWidgetFactory([&](const QString&, const PJ::ObjectDropSeed*, QWidget* parent) -> PJ::IDataWidget* {
    ++factory_calls;
    return new FakeObjectWidget(parent);
  });
  auto* dock = docker.plotAt(0);
  ASSERT_NE(dock, nullptr);

  auto* mounted = new FakeObjectWidget();  // refuses every offered topic
  dock->adoptObjectWidget(mounted);
  factory_calls = 0;

  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          dock, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{items[0].key})));

  EXPECT_EQ(dock->objectWidget(), static_cast<PJ::IDataWidget*>(mounted));  // same widget, not replaced
  EXPECT_EQ(factory_calls, 0);                                              // no replacement built
}

TEST(DockWidgetPlaceholderTest, ScalarDropOntoCommittedObjectWidgetIsRejected) {
  // The mirror case: a scalar dropped on a committed object dock must not replace
  // it with a plot.
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  ASSERT_NE(addScalarTopic(session, *dataset, "/imu/accel"), 0U);
  const auto curves = catalog.curves();
  ASSERT_EQ(curves.size(), 1U);

  PJ::PlotDocker docker(u"test"_s, &session, &catalog);
  auto* dock = docker.plotAt(0);
  ASSERT_NE(dock, nullptr);
  auto* mounted = new FakeObjectWidget();
  dock->adoptObjectWidget(mounted);

  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          dock, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{curves[0].name})));

  EXPECT_EQ(dock->objectWidget(), static_cast<PJ::IDataWidget*>(mounted));
  EXPECT_EQ(dock->plotWidget(), nullptr);  // no plot replaced the object dock
}

TEST(DockWidgetPlaceholderTest, CompatibleObjectDropOntoCommittedWidgetIsAcceptedInPlace) {
  // Regression: a compatible object drop is still absorbed in place (not rejected).
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  registerImageObjectTopic(session, *dataset, "/camera/image");
  catalog.rebuildFromDatastore();
  const auto items = catalog.items();
  ASSERT_EQ(items.size(), 1U);

  PJ::PlotDocker docker(u"test"_s, &session, &catalog);
  int factory_calls = 0;
  docker.setObjectWidgetFactory([&](const QString&, const PJ::ObjectDropSeed*, QWidget* parent) -> PJ::IDataWidget* {
    ++factory_calls;
    return new FakeAcceptingObjectWidget(parent);
  });
  auto* dock = docker.plotAt(0);
  ASSERT_NE(dock, nullptr);
  auto* mounted = new FakeAcceptingObjectWidget();  // accepts every offered topic
  dock->adoptObjectWidget(mounted);
  factory_calls = 0;

  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          dock, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{items[0].key})));

  EXPECT_EQ(dock->objectWidget(), static_cast<PJ::IDataWidget*>(mounted));  // same widget kept
  EXPECT_EQ(factory_calls, 0);                                              // absorbed, not rebuilt
  EXPECT_EQ(mounted->acceptedCount(), 1);                                   // topic was taken
}

TEST(DockWidgetPlaceholderTest, ObjectDropOntoCommittedPlotIsRejected) {
  // The fourth cross-family combination: an object topic dropped on a committed
  // plot dock must be rejected, not replace the plot (which would destroy its
  // curves).
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  ASSERT_NE(addScalarTopic(session, *dataset, "/imu/accel"), 0U);
  registerImageObjectTopic(session, *dataset, "/camera/image");
  catalog.rebuildFromDatastore();

  const auto curves = catalog.curves();
  ASSERT_EQ(curves.size(), 1U);
  QString image_key;
  for (const auto& item : catalog.items()) {
    if (PJ::isObjectTopic(item)) {
      image_key = item.key;
      break;
    }
  }
  ASSERT_FALSE(image_key.isEmpty());

  PJ::PlotDocker docker(u"test"_s, &session, &catalog);
  int factory_calls = 0;
  docker.setObjectWidgetFactory([&](const QString&, const PJ::ObjectDropSeed*, QWidget* parent) -> PJ::IDataWidget* {
    ++factory_calls;
    return new FakeObjectWidget(parent);
  });
  auto* dock = docker.plotAt(0);
  ASSERT_NE(dock, nullptr);

  // Commit the dock to a plot with one curve.
  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          dock, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{curves[0].name})));
  ASSERT_NE(dock->plotWidget(), nullptr);
  ASSERT_EQ(dock->plotWidget()->curveList().size(), 1U);

  // Dropping an object topic must be rejected: the plot and its curve are kept.
  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          dock, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{image_key})));
  EXPECT_NE(dock->plotWidget(), nullptr);
  EXPECT_EQ(dock->plotWidget()->curveList().size(), 1U);
  EXPECT_EQ(dock->objectWidget(), nullptr);
  EXPECT_EQ(factory_calls, 0);
}

// ---------- plotWidgetAdded fires for every creation path -------------------
//
// MainWindow applies the global view toggles (grid, dots, legend, and the time
// TRACKER display level) to each plot through its onPlotAdded slot, which is
// wired to PlotDocker::plotWidgetAdded. If a creation path forgets to emit that
// signal, the new plot silently keeps CurveTracker's constructor default
// (kValue) and ignores the user's global tracker setting. These tests pin the
// signal for each way a plot can be born.

// Simulates MainWindow: every plot that announces itself gets the global tracker
// parameter applied, exactly as applyGlobalToggles() does.
struct TrackerConfigHarness {
  TrackerConfigHarness(PJ::PlotDocker& docker, PJ::CurveTracker::Parameter global) : global_param(global) {
    QObject::connect(&docker, &PJ::PlotDocker::plotWidgetAdded, &docker, [this](PJ::PlotWidget* plot) {
      ++added_count;
      if (plot != nullptr) {
        plot->setTrackerParameter(global_param);
      }
    });
  }
  PJ::CurveTracker::Parameter global_param;
  int added_count = 0;
};

TEST(NewPlotTrackerConfig, DropCreatedPlotInheritsGlobalTrackerParameter) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  ASSERT_NE(addScalarTopic(session, *dataset, "/imu/accel"), 0U);
  const auto curves = catalog.curves();
  ASSERT_EQ(curves.size(), 1U);

  PJ::PlotDocker docker(u"test"_s, &session, &catalog);
  TrackerConfigHarness harness(docker, PJ::CurveTracker::kLineOnly);

  auto* dock = docker.plotAt(0);
  ASSERT_NE(dock, nullptr);
  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          dock, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{curves[0].name})));

  ASSERT_NE(dock->plotWidget(), nullptr);
  EXPECT_EQ(harness.added_count, 1) << "plotWidgetAdded must fire when a drop creates the plot";
  EXPECT_EQ(dock->plotWidget()->trackerParameter(), PJ::CurveTracker::kLineOnly);
}

TEST(NewPlotTrackerConfig, IconClickCreatedPlotInheritsGlobalTrackerParameter) {
  // The PR #211 path: clicking the placeholder's plot icon builds an empty plot
  // via onVisualizationRequested(kPlot) -> ensurePlotWidget().
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  PJ::PlotDocker docker(u"test"_s, &session, &catalog);
  TrackerConfigHarness harness(docker, PJ::CurveTracker::kLineOnly);

  auto* dock = docker.plotAt(0);
  ASSERT_NE(dock, nullptr);
  auto* placeholder = dock->findChild<PJ::VisualizationPlaceholderWidget*>();
  ASSERT_NE(placeholder, nullptr);
  iconButton(placeholder, "buttonVizPlot")->click();

  ASSERT_NE(dock->plotWidget(), nullptr);
  EXPECT_EQ(harness.added_count, 1) << "plotWidgetAdded must fire when the placeholder icon creates the plot";
  EXPECT_EQ(dock->plotWidget()->trackerParameter(), PJ::CurveTracker::kLineOnly);
}

TEST(NewPlotTrackerConfig, SplitCreatedPlotInheritsGlobalTrackerParameter) {
  // Splitting yields a new placeholder dock; dropping a curve into it creates the
  // plot. The split-born dock must also be wired so its plot announces itself.
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  ASSERT_NE(addScalarTopic(session, *dataset, "/imu/accel"), 0U);
  const auto curves = catalog.curves();
  ASSERT_EQ(curves.size(), 1U);

  PJ::PlotDocker docker(u"test"_s, &session, &catalog);
  TrackerConfigHarness harness(docker, PJ::CurveTracker::kLineOnly);

  auto* dock0 = docker.plotAt(0);
  ASSERT_NE(dock0, nullptr);
  auto* dock1 = splitFrom(docker, dock0, 2);
  ASSERT_NE(dock1, nullptr);
  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          dock1, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{curves[0].name})));

  ASSERT_NE(dock1->plotWidget(), nullptr);
  EXPECT_EQ(dock1->plotWidget()->trackerParameter(), PJ::CurveTracker::kLineOnly);
}

TEST(NewPlotTrackerConfig, PlotInSecondTabInheritsGlobalTrackerParameter) {
  // The screenshot scenario: a plot created in a SECOND tab. Mimics MainWindow's
  // tab wiring exactly — tabAdded wires the new docker's plotWidgetAdded, and the
  // tab created in the TabbedPlotWidget constructor is wired separately (the role
  // wireExistingPlots() plays at startup).
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  ASSERT_NE(addScalarTopic(session, *dataset, "/imu/accel"), 0U);
  const auto curves = catalog.curves();
  ASSERT_EQ(curves.size(), 1U);

  PJ::TabbedPlotWidget tabbed(u"main"_s);
  tabbed.setDataServices(&session, &catalog);

  const auto global_param = PJ::CurveTracker::kLineOnly;
  const auto wire_docker = [global_param](PJ::PlotDocker* docker) {
    QObject::connect(docker, &PJ::PlotDocker::plotWidgetAdded, docker, [global_param](PJ::PlotWidget* plot) {
      if (plot != nullptr) {
        plot->setTrackerParameter(global_param);
      }
    });
  };
  QObject::connect(
      &tabbed, &PJ::TabbedPlotWidget::tabAdded, &tabbed, [&](PJ::PlotDocker* docker) { wire_docker(docker); });
  for (int index = 0; index < tabbed.dockerCount(); ++index) {
    wire_docker(tabbed.dockerAt(index));
  }

  PJ::PlotDocker* tab2 = tabbed.addTab(u"tab2"_s);
  ASSERT_NE(tab2, nullptr);
  auto* dock = tab2->plotAt(0);
  ASSERT_NE(dock, nullptr);
  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          dock, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{curves[0].name})));

  ASSERT_NE(dock->plotWidget(), nullptr);
  EXPECT_EQ(dock->plotWidget()->trackerParameter(), global_param);
}

// ---------- kLineOnly tracker keeps its value box hidden --------------------
//
// The real bug behind "the new plot's tracker ignores the global line-only
// setting and shows a box": the tracker PARAMETER is applied correctly, but
// CurveTracker::setEnabled() force-shows the value box, and several callers
// (PlotWidget::addCurve, setTrackerEnabled, …) re-enable the tracker WITHOUT a
// following setPosition to re-apply the parameter-based visibility. So adding a
// curve to a line-only plot resurrects a stale "time : …" box.

TEST(NewPlotTrackerConfig, LineOnlyTrackerKeepsValueBoxHiddenWhenCurveAdded) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  ASSERT_NE(addScalarTopic(session, *dataset, "/imu/accel"), 0U);
  const auto curves = catalog.curves();
  ASSERT_EQ(curves.size(), 1U);

  PJ::PlotWidget plot(&session, &catalog);
  plot.setTrackerParameter(PJ::CurveTracker::kLineOnly);
  ASSERT_FALSE(plot.trackerValueBoxVisible());

  // The user's path: drag a curve in. addCurve() re-enables the tracker; a
  // line-only tracker must not resurrect its value box.
  ASSERT_NE(plot.addCurve(curves[0].name), nullptr);
  EXPECT_FALSE(plot.trackerValueBoxVisible());
}

TEST(NewPlotTrackerConfig, LineOnlyTrackerStaysHiddenWhenReEnabled) {
  // The restore/undo path: xmlLoadState calls setTrackerEnabled(true) after the
  // global toggles already set kLineOnly. Re-enabling must not show the box.
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  ASSERT_NE(addScalarTopic(session, *dataset, "/imu/accel"), 0U);
  const auto curves = catalog.curves();
  ASSERT_EQ(curves.size(), 1U);

  PJ::PlotWidget plot(&session, &catalog);
  ASSERT_NE(plot.addCurve(curves[0].name), nullptr);
  plot.setTrackerParameter(PJ::CurveTracker::kLineOnly);
  ASSERT_FALSE(plot.trackerValueBoxVisible());

  plot.setTrackerEnabled(true);
  EXPECT_FALSE(plot.trackerValueBoxVisible());
}

TEST(NewPlotTrackerConfig, ValueTrackerShowsValueBoxWhenCurveAdded) {
  // The mirror invariant: a line+value tracker MUST keep its value box when a
  // curve is added. (Regression guard — an over-broad kLineOnly fix once routed
  // setEnabled through setPosition, which hid the box for kValue because the
  // view isn't fit yet at addCurve time.)
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  ASSERT_NE(addScalarTopic(session, *dataset, "/imu/accel"), 0U);
  const auto curves = catalog.curves();
  ASSERT_EQ(curves.size(), 1U);

  PJ::PlotWidget plot(&session, &catalog);
  plot.setTrackerParameter(PJ::CurveTracker::kValue);
  ASSERT_NE(plot.addCurve(curves[0].name), nullptr);
  EXPECT_TRUE(plot.trackerValueBoxVisible());
}

// --- Object-classified placeholder drops (demand-driven streaming) ---
//
// An advertised topic classified as an object (e.g. kPointCloud) has no
// storage id yet — data only starts flowing once the drop registers demand.
// Dropping one on an empty tile must materialize a dock of the right family
// through the factory (null-id seed) and emit placeholderTopicDropped so the
// shell can stage the pending drop that completes when data arrives.

namespace {

QString advertisedKey(PJ::CatalogModel& catalog, const QString& topic_name) {
  for (const auto& item : catalog.items()) {
    if (item.topic_name == topic_name && PJ::asAdvertisedTopic(item) != nullptr) {
      return item.key;
    }
  }
  return {};
}

}  // namespace

TEST(DockWidgetPlaceholderTest, ObjectPlaceholderDropOnEmptyTileCreatesDockAndStagesPendingDrop) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "robot"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  catalog.setAdvertisedTopics(
      *dataset, {{.topic_name = "/cloud", .classification = PJ::sdk::BuiltinObjectType::kPointCloud}});
  const QString key = advertisedKey(catalog, u"/cloud"_s);
  ASSERT_FALSE(key.isEmpty());

  PJ::PlotDocker docker(u"test"_s, &session, &catalog);
  bool factory_called = false;
  docker.setObjectWidgetFactory(
      [&](const QString& kind, const PJ::ObjectDropSeed* seed, QWidget* parent) -> PJ::IDataWidget* {
        factory_called = true;
        EXPECT_TRUE(kind.isEmpty());
        EXPECT_NE(seed, nullptr);
        if (seed != nullptr) {
          EXPECT_EQ(seed->topic_id, PJ::ObjectTopicId{});  // placeholder: no storage id yet
          EXPECT_EQ(seed->object_type, PJ::sdk::BuiltinObjectType::kPointCloud);
        }
        return new FakeObjectWidget(parent);
      });
  auto* dock = docker.plotAt(0);
  ASSERT_NE(dock, nullptr);

  PJ::DockWidget* signal_dock = nullptr;
  PJ::DatasetId signal_dataset = 0;
  QString signal_topic;
  auto signal_type = PJ::sdk::BuiltinObjectType::kNone;
  QObject::connect(
      &docker, &PJ::PlotDocker::placeholderTopicDropped, &docker,
      [&](PJ::DockWidget* d, PJ::DatasetId ds, QString topic, PJ::sdk::BuiltinObjectType type) {
        signal_dock = d;
        signal_dataset = ds;
        signal_topic = std::move(topic);
        signal_type = type;
      });

  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          dock, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{key})));

  EXPECT_TRUE(factory_called);
  EXPECT_NE(dock->objectWidget(), nullptr);
  EXPECT_EQ(dock->plotWidget(), nullptr);
  EXPECT_EQ(signal_dock, dock);
  EXPECT_EQ(signal_dataset, *dataset);
  EXPECT_EQ(signal_topic, u"/cloud"_s);
  EXPECT_EQ(signal_type, PJ::sdk::BuiltinObjectType::kPointCloud);
}

TEST(DockWidgetPlaceholderTest, MultiObjectPlaceholderDropPendsEveryKey) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "robot"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  catalog.setAdvertisedTopics(
      *dataset, {
                    {.topic_name = "/lidar/front", .classification = PJ::sdk::BuiltinObjectType::kPointCloud},
                    {.topic_name = "/lidar/back", .classification = PJ::sdk::BuiltinObjectType::kPointCloud},
                });
  const QString front_key = advertisedKey(catalog, u"/lidar/front"_s);
  const QString back_key = advertisedKey(catalog, u"/lidar/back"_s);
  ASSERT_FALSE(front_key.isEmpty());
  ASSERT_FALSE(back_key.isEmpty());

  PJ::PlotDocker docker(u"test"_s, &session, &catalog);
  int factory_calls = 0;
  docker.setObjectWidgetFactory([&](const QString&, const PJ::ObjectDropSeed*, QWidget* parent) -> PJ::IDataWidget* {
    ++factory_calls;
    return new FakeObjectWidget(parent);
  });
  auto* dock = docker.plotAt(0);
  ASSERT_NE(dock, nullptr);

  QStringList dropped_topics;
  QObject::connect(
      &docker, &PJ::PlotDocker::placeholderTopicDropped, &docker,
      [&](PJ::DockWidget*, PJ::DatasetId, QString topic, PJ::sdk::BuiltinObjectType) { dropped_topics << topic; });

  // A two-key drop must build ONE dock and stage a pending drop for EACH topic.
  const QStringList both_keys{front_key, back_key};
  ASSERT_TRUE(
      QMetaObject::invokeMethod(dock, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, both_keys)));

  EXPECT_EQ(factory_calls, 1);
  EXPECT_NE(dock->objectWidget(), nullptr);
  EXPECT_EQ(dropped_topics, (QStringList{u"/lidar/front"_s, u"/lidar/back"_s}));
}

TEST(DockWidgetPlaceholderTest, ObjectPlaceholderDropKeepsPlaceholderWhenFactoryRefuses) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "robot"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  catalog.setAdvertisedTopics(
      *dataset, {{.topic_name = "/camera_info", .classification = PJ::sdk::BuiltinObjectType::kCameraInfo}});
  const QString key = advertisedKey(catalog, u"/camera_info"_s);
  ASSERT_FALSE(key.isEmpty());

  PJ::PlotDocker docker(u"test"_s, &session, &catalog);
  docker.setObjectWidgetFactory(
      [](const QString&, const PJ::ObjectDropSeed*, QWidget*) -> PJ::IDataWidget* { return nullptr; });
  auto* dock = docker.plotAt(0);
  ASSERT_NE(dock, nullptr);
  int signal_count = 0;
  QObject::connect(&docker, &PJ::PlotDocker::placeholderTopicDropped, &docker, [&]() { ++signal_count; });

  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          dock, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{key})));

  // Unhostable family: no widget, no pending drop, placeholder still usable.
  EXPECT_EQ(dock->objectWidget(), nullptr);
  EXPECT_EQ(dock->plotWidget(), nullptr);
  EXPECT_EQ(signal_count, 0);
  EXPECT_NE(dock->findChild<PJ::VisualizationPlaceholderWidget*>(), nullptr);
}

TEST(DockWidgetPlaceholderTest, ObjectPlaceholderDropOntoCommittedPlotIsRejected) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "robot"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  ASSERT_NE(addScalarTopic(session, *dataset, "/imu/accel"), 0U);
  catalog.setAdvertisedTopics(
      *dataset, {{.topic_name = "/cloud", .classification = PJ::sdk::BuiltinObjectType::kPointCloud}});
  const auto curves = catalog.curves();
  ASSERT_EQ(curves.size(), 1U);
  const QString placeholder_key = advertisedKey(catalog, u"/cloud"_s);
  ASSERT_FALSE(placeholder_key.isEmpty());

  PJ::PlotDocker docker(u"test"_s, &session, &catalog);
  bool factory_called = false;
  docker.setObjectWidgetFactory([&](const QString&, const PJ::ObjectDropSeed*, QWidget* parent) -> PJ::IDataWidget* {
    factory_called = true;
    return new FakeObjectWidget(parent);
  });
  auto* dock = docker.plotAt(0);
  ASSERT_NE(dock, nullptr);

  // Commit the dock as a plot first.
  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          dock, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{curves[0].name})));
  ASSERT_NE(dock->plotWidget(), nullptr);

  // An object placeholder dropped on a committed plot must not replace it.
  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          dock, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{placeholder_key})));

  EXPECT_FALSE(factory_called);
  EXPECT_NE(dock->plotWidget(), nullptr);
  EXPECT_EQ(dock->objectWidget(), nullptr);
}
