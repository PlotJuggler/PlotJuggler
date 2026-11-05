// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The curve list must not offer an object topic as a drag payload when no view
// can display its type (e.g. an occupancy grid update): the drop could
// only end in the "Cannot display topic" dead end. The row stays selectable
// (context menu, force-streaming) and carries a tooltip saying why it cannot
// be dragged; displayable object topics keep dragging. Attempting the gesture
// anyway is answered, not swallowed: the tree's not-draggable drag signals surface as
// the panel's toastRequested (the shell shows a toast).

#include <gtest/gtest.h>

#include <QApplication>
#include <QContextMenuEvent>
#include <QHelpEvent>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPushButton>
#include <QSignalSpy>
#include <QString>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QTreeWidgetItem>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "dataset_test_helpers.h"
#include "pj_base/sdk/platform.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/TopicDemandTracker.h"
#include "pj_widgets/CurveTreeView.h"
#include "support/gui_test_env.h"
#include "ui/CurveListPanel.h"
using namespace Qt::StringLiterals;

namespace PJ {
namespace {

// The organization name sandboxes the panel's QSettings reads away from the
// user's real preferences; only this suite may select that settings scope.
struct QtEnvironment : ::testing::Environment {
  void SetUp() override {
    if (!pj_app_test::isTestSuiteSelected("CurveListPanelTest")) {
      return;
    }
    QCoreApplication::setOrganizationName(u"PJ4CurveListPanelTest"_s);
    QCoreApplication::setApplicationName(u"PJ4CurveListPanelTest"_s);
  }
};
static auto* const kEnv = ::testing::AddGlobalTestEnvironment(new QtEnvironment);

// Depth-first search for the row resolving to `catalog_key`, independent of the
// tree's view mode (hierarchical vs show-topics lay the same topic out under
// different node chains).
QTreeWidgetItem* findRowByCatalogKey(QTreeWidgetItem* item, const QString& catalog_key) {
  if (CurveTreeView::catalogKeyOf(item) == catalog_key) {
    return item;
  }
  for (int i = 0; i < item->childCount(); ++i) {
    if (QTreeWidgetItem* found = findRowByCatalogKey(item->child(i), catalog_key)) {
      return found;
    }
  }
  return nullptr;
}

QString keyOfTopic(const CatalogModel& catalog, const QString& topic_name) {
  for (const CatalogItem& item : catalog.items()) {
    if (item.topic_name == topic_name && isObjectTopic(item)) {
      return item.key;
    }
  }
  return {};
}

// Drive the right-button press plus the context event Linux delivers before a
// drag can start. Close a real menu through its nested loop, recording its UI.
QStringList rightClickMenuButtons(CurveTreeView& tree, QTreeWidgetItem* row) {
  const QPoint pos = tree.visualItemRect(row).center();
  const QPoint global = tree.viewport()->mapToGlobal(pos);
  QMouseEvent press(QEvent::MouseButtonPress, pos, global, Qt::RightButton, Qt::RightButton, Qt::NoModifier);
  QApplication::sendEvent(tree.viewport(), &press);
  QStringList labels;
  QObject scope;
  QTimer::singleShot(0, &scope, [&labels]() {
    if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
      for (const auto* button : menu->findChildren<QPushButton*>()) {
        labels.push_back(button->text());
      }
      menu->close();
    }
  });
  QContextMenuEvent context(QContextMenuEvent::Mouse, pos, global);
  QApplication::sendEvent(tree.viewport(), &context);
  QApplication::processEvents();
  QMouseEvent release(QEvent::MouseButtonRelease, pos, global, Qt::RightButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(tree.viewport(), &release);
  return labels;
}

TEST(CurveListPanelTest, MultiSelectionLeavesRightDragAvailableForXYAndCtrlForSelection) {
  AppSession app_session;
  const DatasetId stream = pj_test::createDataset(app_session, "stream");
  auto writer = app_session.sessionManager().dataEngine().createWriter();
  const auto topic = writer.registerTopic(stream, TopicDescriptor{.name = "/xy"});
  ASSERT_TRUE(topic.has_value());
  ASSERT_TRUE(writer.ensureColumn(*topic, "x", PrimitiveType::kFloat64).has_value());
  ASSERT_TRUE(writer.ensureColumn(*topic, "y", PrimitiveType::kFloat64).has_value());
  ASSERT_TRUE(writer.beginRow(*topic, 100).has_value());
  writer.set(*topic, 0, 1.0);
  writer.set(*topic, 1, 2.0);
  ASSERT_TRUE(writer.finishRow(*topic).has_value());
  EXPECT_FALSE(app_session.sessionManager().commitChunks(writer.flushAll()).empty());
  auto& catalog = app_session.catalogModel();
  catalog.setPerTopicPauseCapable(stream, true);
  catalog.rebuildFromDatastore();
  CurveListPanel panel;
  panel.setCatalog(&catalog);
  panel.setTopicDemandTracker(&app_session.topicDemandTracker());
  panel.resize(500, 500);
  panel.show();
  auto* tree = panel.findChild<CurveTreeView*>(u"treeView"_s);
  ASSERT_NE(tree, nullptr);
  tree->expandAll();
  QApplication::processEvents();
  std::vector<QTreeWidgetItem*> rows;
  for (const auto& item : catalog.items()) {
    if (isScalarField(item)) {
      rows.push_back(findRowByCatalogKey(tree->invisibleRootItem(), item.key));
    }
  }
  ASSERT_EQ(rows.size(), 2u);
  ASSERT_NE(rows[0], nullptr);
  ASSERT_NE(rows[1], nullptr);
  QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, tree->visualItemRect(rows[0]).center());
  QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::ControlModifier, tree->visualItemRect(rows[1]).center());
  ASSERT_EQ(tree->selectedItems().size(), 2);
  EXPECT_TRUE(rightClickMenuButtons(*tree, rows.front()).isEmpty());
  ASSERT_EQ(tree->selectedItems().size(), 2);
  const std::unique_ptr<QMimeData> xy(tree->createDragMimeData(Qt::RightButton));
  ASSERT_NE(xy, nullptr);
  EXPECT_TRUE(xy->hasFormat(CurveTreeView::newXyAxisMimeType()));
  const std::unique_ptr<QMimeData> plain(tree->createDragMimeData(Qt::LeftButton));
  ASSERT_NE(plain, nullptr);
  EXPECT_TRUE(plain->hasFormat(u"curveslist/add_curve"_s));
  EXPECT_FALSE(plain->hasFormat(CurveTreeView::newXyAxisMimeType()));

  rows.back()->setSelected(false);
  EXPECT_EQ(rightClickMenuButtons(*tree, rows.front()), QStringList{u"Force topic streaming"_s});
  app_session.topicDemandTracker().setTopicForced(stream, catalog.items().front().topic_name, true);
  EXPECT_EQ(rightClickMenuButtons(*tree, rows.front()), QStringList{u"Stop forced streaming"_s});
  catalog.setPerTopicPauseCapable(stream, false);
  EXPECT_TRUE(rightClickMenuButtons(*tree, rows.front()).isEmpty());
}

TEST(CurveListPanelTest, MultiSelectedDatasetsKeepTheirMenu) {
  AppSession app_session;
  for (const char* name : {"first.mcap", "second.mcap"}) {
    const DatasetId file = pj_test::createDataset(app_session, name);
    ASSERT_NE(pj_test::addScalarTopic(app_session, file, "/value"), 0U);
  }
  CurveListPanel panel;
  panel.setCatalog(&app_session.catalogModel());
  panel.setTopicDemandTracker(&app_session.topicDemandTracker());
  panel.resize(500, 500);
  panel.show();
  auto* tree = panel.findChild<CurveTreeView*>(u"treeView"_s);
  ASSERT_NE(tree, nullptr);
  tree->expandAll();
  QApplication::processEvents();
  ASSERT_EQ(tree->topLevelItemCount(), 2);
  tree->topLevelItem(0)->setSelected(true);
  tree->topLevelItem(1)->setSelected(true);
  EXPECT_TRUE(rightClickMenuButtons(*tree, tree->topLevelItem(0)).contains(u"Merge"_s));
  EXPECT_EQ(tree->selectedItems().size(), 2);
}

TEST(CurveListPanelTest, SelectionAcrossDatasetsAndCustomTopicsAlsoKeepsTheStreamingMenuHidden) {
  AppSession app_session;
  const DatasetId stream = pj_test::createDataset(app_session, "stream");
  ASSERT_NE(pj_test::addScalarTopic(app_session, stream, "/source"), 0U);
  ASSERT_NE(pj_test::addScalarTopic(app_session, stream, "/derived"), 0U);
  auto& catalog = app_session.catalogModel();
  catalog.setPerTopicPauseCapable(stream, true);
  catalog.rebuildFromDatastore();
  const auto items = catalog.items();
  ASSERT_EQ(items.size(), 2u);
  CurveListPanel panel;
  panel.setCatalog(&catalog);
  panel.setTopicDemandTracker(&app_session.topicDemandTracker());
  panel.setCustomTopics({{.key = items.back().key, .owner = u"test"_s, .name = u"derived"_s}});
  panel.resize(500, 600);
  panel.show();
  auto* tree = panel.findChild<CurveTreeView*>(u"treeView"_s);
  auto* custom = panel.findChild<CurveTreeView*>(u"customView"_s);
  ASSERT_NE(tree, nullptr);
  ASSERT_NE(custom, nullptr);
  tree->expandAll();
  custom->expandAll();
  QApplication::processEvents();
  auto* source_row = findRowByCatalogKey(tree->invisibleRootItem(), items.front().key);
  auto* custom_row = findRowByCatalogKey(custom->invisibleRootItem(), items.back().key);
  ASSERT_NE(source_row, nullptr);
  ASSERT_NE(custom_row, nullptr);
  source_row->setSelected(true);
  custom_row->setSelected(true);
  EXPECT_TRUE(rightClickMenuButtons(*tree, source_row).isEmpty());
  const std::unique_ptr<QMimeData> xy(tree->createDragMimeData(Qt::RightButton));
  ASSERT_NE(xy, nullptr);
  EXPECT_TRUE(xy->hasFormat(CurveTreeView::newXyAxisMimeType()));
}

TEST(CurveListPanelTest, HoverTooltipReportsTypeAndCounts) {
  AppSession app_session;
  const DatasetId dataset = pj_test::createDataset(app_session, "drive.mcap");
  ASSERT_NE(dataset, 0U);
  ASSERT_NE(pj_test::addScalarTopic(app_session, dataset, "/imu/x"), 0U);
  auto image_topic = app_session.sessionManager().objectStore().registerTopic(
      ObjectTopicDescriptor{
          .dataset_id = dataset,
          .topic_name = "/camera/image",
          .metadata_json = R"({"builtin_object_type":"kImage"})",
      });
  ASSERT_TRUE(image_topic.has_value()) << image_topic.error();
  app_session.catalogModel().rebuildFromDatastore();

  CurveListPanel panel;
  panel.setCatalog(&app_session.catalogModel());
  panel.resize(400, 300);
  panel.show();
  auto* tree = panel.findChild<CurveTreeView*>(u"treeView"_s);
  ASSERT_NE(tree, nullptr);
  tree->expandAll();
  QApplication::processEvents();

  QString scalar_key;
  for (const CatalogItem& item : app_session.catalogModel().items()) {
    if (isScalarField(item)) {
      scalar_key = item.key;
    }
  }
  const QString image_key = keyOfTopic(app_session.catalogModel(), u"/camera/image"_s);
  QTreeWidgetItem* scalar_row = findRowByCatalogKey(tree->invisibleRootItem(), scalar_key);
  QTreeWidgetItem* image_row = findRowByCatalogKey(tree->invisibleRootItem(), image_key);
  ASSERT_NE(scalar_row, nullptr);
  ASSERT_NE(image_row, nullptr);

  // Drive the real hover path; QToolTip::text() reads back what the view showed.
  const auto hover = [&](QTreeWidgetItem* item) {
    const QRect rect = tree->visualItemRect(item);
    const QPoint pos(rect.left() + 4, rect.center().y());
    QHelpEvent event(QEvent::ToolTip, pos, tree->viewport()->mapToGlobal(pos));
    QApplication::sendEvent(tree->viewport(), &event);
    return QToolTip::text();
  };
  const QString scalar_tip = hover(scalar_row);
  EXPECT_TRUE(scalar_tip.contains(u"float64"_s)) << scalar_tip.toStdString();
  EXPECT_TRUE(scalar_tip.contains(u"<span style=\"font-weight:600\">Samples:</span> 2"_s)) << scalar_tip.toStdString();
  const QString image_tip = hover(image_row);
  EXPECT_TRUE(image_tip.contains(u"<span style=\"font-weight:600\">Type:</span> Image"_s)) << image_tip.toStdString();
  EXPECT_TRUE(image_tip.contains(u"<span style=\"font-weight:600\">Messages:</span> 0"_s)) << image_tip.toStdString();
  const QString dataset_tip = hover(tree->topLevelItem(0));
  EXPECT_TRUE(dataset_tip.contains(u"<span style=\"font-weight:600\">Topics:</span> 2"_s)) << dataset_tip.toStdString();
}

TEST(CurveListPanelTest, UndisplayableObjectTopicRowIsDragInertWithTooltip) {
  AppSession app_session;
  const DatasetId dataset = pj_test::createDataset(app_session, "drive.mcap");
  ASSERT_NE(dataset, 0U);

  auto update_topic = app_session.sessionManager().objectStore().registerTopic(
      ObjectTopicDescriptor{
          .dataset_id = dataset,
          .topic_name = "/map/updates",
          .metadata_json = R"({"builtin_object_type":"kOccupancyGridUpdate"})",
      });
  ASSERT_TRUE(update_topic.has_value()) << update_topic.error();
  auto image_topic = app_session.sessionManager().objectStore().registerTopic(
      ObjectTopicDescriptor{
          .dataset_id = dataset,
          .topic_name = "/camera/image",
          .metadata_json = R"({"builtin_object_type":"kImage"})",
      });
  ASSERT_TRUE(image_topic.has_value()) << image_topic.error();
  app_session.catalogModel().rebuildFromDatastore();

  CurveListPanel panel;
  panel.setCatalog(&app_session.catalogModel());
  auto* tree = panel.findChild<CurveTreeView*>(u"treeView"_s);
  ASSERT_NE(tree, nullptr);

  const QString update_key = keyOfTopic(app_session.catalogModel(), u"/map/updates"_s);
  const QString image_key = keyOfTopic(app_session.catalogModel(), u"/camera/image"_s);
  ASSERT_FALSE(update_key.isEmpty());
  ASSERT_FALSE(image_key.isEmpty());
  QTreeWidgetItem* update_row = findRowByCatalogKey(tree->invisibleRootItem(), update_key);
  QTreeWidgetItem* image_row = findRowByCatalogKey(tree->invisibleRootItem(), image_key);
  ASSERT_NE(update_row, nullptr);
  ASSERT_NE(image_row, nullptr);

  EXPECT_TRUE(update_row->flags().testFlag(Qt::ItemIsSelectable));
  EXPECT_FALSE(update_row->flags().testFlag(Qt::ItemIsDragEnabled));
  EXPECT_FALSE(update_row->toolTip(0).isEmpty());

  EXPECT_TRUE(image_row->flags().testFlag(Qt::ItemIsDragEnabled));
  EXPECT_TRUE(image_row->toolTip(0).isEmpty());

  // Not-draggable drag feedback wiring: the tree's signals must surface as
  // toastRequested. Invoked directly (signals are public-callable) — the
  // gesture mechanics that fire them are covered by curve_tree_view_test.
  QStringList toasts;
  QObject::connect(
      &panel, &CurveListPanel::toastRequested, [&toasts](const QString& message) { toasts.append(message); });
  emit tree->dragAttemptedOnNotDraggableRow(update_row->toolTip(0));
  ASSERT_EQ(toasts.size(), 1);
  EXPECT_EQ(toasts.front(), update_row->toolTip(0));  // tooltip doubles as the toast text
  emit tree->dragAttemptedOnNotDraggableRow(QString{});
  ASSERT_EQ(toasts.size(), 2);
  EXPECT_FALSE(toasts.back().isEmpty()) << "empty reason must fall back to a generic wording";
  emit tree->dragPayloadKeysSkipped({update_key});
  ASSERT_EQ(toasts.size(), 3);
  EXPECT_TRUE(toasts.back().contains(u"/map/updates"_s)) << toasts.back().toStdString();
}

// No view displays a calibration topic, but a plugin panel reads it as an input, so its row starts a drag.
TEST(CurveListPanelTest, CameraInfoTopicRowCanBeDraggedToAPanel) {
  AppSession app_session;
  const DatasetId dataset = pj_test::createDataset(app_session, "drive.mcap");
  ASSERT_NE(dataset, 0U);
  auto topic = app_session.sessionManager().objectStore().registerTopic(
      ObjectTopicDescriptor{
          .dataset_id = dataset,
          .topic_name = "/camera/camera_info",
          .metadata_json = R"({"builtin_object_type":"kCameraInfo"})",
      });
  ASSERT_TRUE(topic.has_value()) << topic.error();
  app_session.catalogModel().rebuildFromDatastore();

  CurveListPanel panel;
  panel.setCatalog(&app_session.catalogModel());
  auto* tree = panel.findChild<CurveTreeView*>(u"treeView"_s);
  ASSERT_NE(tree, nullptr);
  const QString key = keyOfTopic(app_session.catalogModel(), u"/camera/camera_info"_s);
  QTreeWidgetItem* row = findRowByCatalogKey(tree->invisibleRootItem(), key);
  ASSERT_NE(row, nullptr);

  EXPECT_TRUE(row->flags().testFlag(Qt::ItemIsDragEnabled));
  EXPECT_TRUE(row->toolTip(0).contains(u"plugin panel"_s)) << row->toolTip(0).toStdString();

  row->setSelected(true);
  const std::unique_ptr<QMimeData> payload(tree->createDragMimeData(Qt::LeftButton));
  ASSERT_NE(payload, nullptr);
  EXPECT_EQ(CurveTreeView::decodeCatalogKeys(payload.get()), QStringList{key});
}

// A topic of a per-topic-pause-capable dataset that nothing references is dimmed; displaying it undims it; a dataset
// that cannot pause topics never dims; a panel without a tracker dims nothing.
TEST(CurveListPanelTest, UnsubscribedRowsFollowTheDemandTracker) {
  AppSession app_session;
  const DatasetId stream = pj_test::createDataset(app_session, "stream");
  const DatasetId file = pj_test::createDataset(app_session, "file.mcap");
  ASSERT_NE(stream, 0U);
  ASSERT_NE(file, 0U);
  for (const auto& [dataset, topic] : {std::pair{stream, "/a"}, {stream, "/b"}, {file, "/c"}}) {
    ASSERT_TRUE(app_session.sessionManager()
                    .objectStore()
                    .registerTopic(
                        ObjectTopicDescriptor{
                            .dataset_id = dataset,
                            .topic_name = topic,
                            .metadata_json = R"({"builtin_object_type":"kPointCloud"})"})
                    .has_value());
  }
  CatalogModel& catalog = app_session.catalogModel();
  catalog.setPerTopicPauseCapable(stream, true);
  catalog.rebuildFromDatastore();

  CurveListPanel panel;
  panel.setCatalog(&catalog);
  auto* tree = panel.findChild<CurveTreeView*>(u"treeView"_s);
  ASSERT_NE(tree, nullptr);
  const QString key_a = keyOfTopic(catalog, u"/a"_s);
  const QString key_b = keyOfTopic(catalog, u"/b"_s);
  const QString key_c = keyOfTopic(catalog, u"/c"_s);
  ASSERT_FALSE(key_a.isEmpty());
  ASSERT_FALSE(key_b.isEmpty());
  ASSERT_FALSE(key_c.isEmpty());

  EXPECT_FALSE(tree->isKeyUnsubscribed(key_a)) << "no tracker: nothing is dimmed";
  panel.setTopicDemandTracker(&app_session.topicDemandTracker());
  EXPECT_TRUE(tree->isKeyUnsubscribed(key_a));
  EXPECT_TRUE(tree->isKeyUnsubscribed(key_b));
  EXPECT_FALSE(tree->isKeyUnsubscribed(key_c)) << "a dataset that cannot pause topics never dims";

  app_session.topicDemandTracker().addReference(stream, u"/a"_s);
  EXPECT_FALSE(tree->isKeyUnsubscribed(key_a));
  EXPECT_TRUE(tree->isKeyUnsubscribed(key_b));
  app_session.topicDemandTracker().removeReference(stream, u"/a"_s);
  EXPECT_TRUE(tree->isKeyUnsubscribed(key_a));

  panel.setTopicDemandTracker(nullptr);
  EXPECT_FALSE(tree->isKeyUnsubscribed(key_a)) << "a detached tracker clears every flag";
  EXPECT_FALSE(tree->isKeyUnsubscribed(key_b));
}

// A demand change walks the catalog only while some dataset can pause topics, and it reads that capability at every
// change, not once when the tracker attached: a dataset that becomes pause-capable afterwards dims its idle topics, and
// one that stops being so is cleared (the walk is skipped, the flags still pushed).
TEST(CurveListPanelTest, DemandChangeWalksTheCatalogOnlyWhileADatasetCanPauseTopics) {
  AppSession app_session;
  const DatasetId stream = pj_test::createDataset(app_session, "stream");
  const DatasetId file = pj_test::createDataset(app_session, "file.mcap");
  ASSERT_NE(stream, 0U);
  ASSERT_NE(file, 0U);
  for (const auto& [dataset, topic] : {std::pair{stream, "/a"}, {stream, "/b"}, {file, "/c"}}) {
    ASSERT_TRUE(app_session.sessionManager()
                    .objectStore()
                    .registerTopic(
                        ObjectTopicDescriptor{
                            .dataset_id = dataset,
                            .topic_name = topic,
                            .metadata_json = R"({"builtin_object_type":"kPointCloud"})"})
                    .has_value());
  }
  CatalogModel& catalog = app_session.catalogModel();
  catalog.rebuildFromDatastore();
  TopicDemandTracker& tracker = app_session.topicDemandTracker();

  CurveListPanel panel;
  panel.setCatalog(&catalog);
  panel.setTopicDemandTracker(&tracker);
  auto* tree = panel.findChild<CurveTreeView*>(u"treeView"_s);
  ASSERT_NE(tree, nullptr);
  const QString key_a = keyOfTopic(catalog, u"/a"_s);
  const QString key_b = keyOfTopic(catalog, u"/b"_s);
  const QString key_c = keyOfTopic(catalog, u"/c"_s);
  ASSERT_FALSE(key_a.isEmpty());
  ASSERT_FALSE(key_b.isEmpty());
  ASSERT_FALSE(key_c.isEmpty());

  std::size_t walks = catalog.wholeCatalogWalks();
  tracker.addReference(stream, u"/a"_s);
  tracker.removeReference(stream, u"/a"_s);
  EXPECT_EQ(catalog.wholeCatalogWalks(), walks) << "no dataset can pause topics: nothing to dim, nothing to walk";
  EXPECT_FALSE(tree->isKeyUnsubscribed(key_b));

  catalog.setPerTopicPauseCapable(stream, true);
  walks = catalog.wholeCatalogWalks();
  tracker.addReference(stream, u"/a"_s);
  EXPECT_EQ(catalog.wholeCatalogWalks(), walks + 1) << "a dataset that can pause topics: one walk per change";
  EXPECT_FALSE(tree->isKeyUnsubscribed(key_a));
  EXPECT_TRUE(tree->isKeyUnsubscribed(key_b)) << "pause-capable after the panel attached";
  EXPECT_FALSE(tree->isKeyUnsubscribed(key_c)) << "a dataset that cannot pause topics never dims";

  catalog.setPerTopicPauseCapable(stream, false);
  walks = catalog.wholeCatalogWalks();
  tracker.removeReference(stream, u"/a"_s);
  EXPECT_EQ(catalog.wholeCatalogWalks(), walks) << "the capability is gone: nothing to walk";
  EXPECT_FALSE(tree->isKeyUnsubscribed(key_b)) << "the skipped walk still clears the flags it set";
}

// PJ_BENCH=1: one demand change in a catalog of 10k object topics, a dataset that cannot pause topics (nothing to
// dim) apart from one that can (the flags are recomputed). The fakes are production's: the real panel, catalog
// and tracker.
TEST(CurveListPanelTest, BenchDemandChangeAmongTenThousandTopics) {
  if (!PJ::sdk::getEnv("PJ_BENCH")) {
    GTEST_SKIP() << "set PJ_BENCH=1";
  }
  AppSession app_session;
  const DatasetId fixed = pj_test::createDataset(app_session, "file.mcap");
  const DatasetId stream = pj_test::createDataset(app_session, "stream");
  constexpr int kTopics = 10'000;
  for (const DatasetId dataset : {fixed, stream}) {
    for (int index = 0; index < kTopics / 2; ++index) {
      ASSERT_TRUE(app_session.sessionManager()
                      .objectStore()
                      .registerTopic(
                          ObjectTopicDescriptor{
                              .dataset_id = dataset,
                              .topic_name = "/bench/cloud_" + std::to_string(index),
                              .metadata_json = R"({"builtin_object_type":"kPointCloud"})"})
                      .has_value());
    }
  }
  CatalogModel& catalog = app_session.catalogModel();
  catalog.rebuildFromDatastore();
  CurveListPanel panel;
  panel.setCatalog(&catalog);
  TopicDemandTracker& tracker = app_session.topicDemandTracker();
  panel.setTopicDemandTracker(&tracker);

  const auto median_micros = [&](DatasetId dataset) {
    std::vector<double> micros;
    for (int call = 0; call < 51; ++call) {
      const QString topic = u"/bench/cloud_"_s + QString::number(call);
      const auto begin = std::chrono::steady_clock::now();
      tracker.addReference(dataset, topic);
      micros.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count());
      tracker.removeReference(dataset, topic);
    }
    std::nth_element(micros.begin(), micros.begin() + micros.size() / 2, micros.end());
    return micros[micros.size() / 2];
  };
  const double without_pause = median_micros(fixed);
  catalog.setPerTopicPauseCapable(stream, true);
  const double with_pause = median_micros(stream);
  std::printf(
      "CurveListPanel demand change among %d topics: dataset that cannot pause topics median %.1f us, one that can "
      "%.1f us\n",
      kTopics, without_pause, with_pause);
}

// Registers kPointCloud topics named `names` in `dataset` and relists the catalog.
void registerClouds(AppSession& app_session, DatasetId dataset, std::initializer_list<const char*> names) {
  for (const char* name : names) {
    auto topic = app_session.sessionManager().objectStore().registerTopic(
        ObjectTopicDescriptor{
            .dataset_id = dataset,
            .topic_name = name,
            .metadata_json = R"({"builtin_object_type":"kPointCloud"})",
        });
    ASSERT_TRUE(topic.has_value()) << topic.error();
  }
  app_session.catalogModel().rebuildFromDatastore();
}

QPushButton* deleteButtonOf(CurveListPanel& panel) {
  for (QPushButton* button : panel.findChildren<QPushButton*>()) {
    if (button->text() == u"Delete"_s) {
      return button;
    }
  }
  return nullptr;
}

constexpr const char* kEditor = "toolbox-transform-editor";

// UI-07: an object row of the Custom Topics view is built like its Datasets-tree twin (drags, family icon), names
// its creator, shows the glyph of its node's state (computing while an older result is shown, the lag in the
// tooltip), and the pencil is enabled only for a row the declared editor owns. A state-only change repaints the
// glyph without rebuilding the rows.
TEST(CurveListPanelTest, CustomTopicsObjectRowDragsShowsGlyphAndGatesEdit) {
  AppSession app_session;
  const DatasetId dataset = pj_test::createDataset(app_session, "drive.mcap");
  ASSERT_NE(dataset, 0U);
  registerClouds(app_session, dataset, {"derived/a", "derived/b"});

  CurveListPanel panel;
  panel.setEditorPluginId(QString::fromLatin1(kEditor));
  panel.setCatalog(&app_session.catalogModel());
  auto* tree = panel.findChild<CurveTreeView*>(u"treeView"_s);
  auto* custom = panel.findChild<CurveTreeView*>(u"customView"_s);
  auto* edit = panel.findChild<QToolButton*>(u"buttonEditCustom"_s);
  ASSERT_NE(tree, nullptr);
  ASSERT_NE(custom, nullptr);
  ASSERT_NE(edit, nullptr);
  const QString key_a = keyOfTopic(app_session.catalogModel(), u"derived/a"_s);
  const QString key_b = keyOfTopic(app_session.catalogModel(), u"derived/b"_s);
  ASSERT_FALSE(key_a.isEmpty());
  ASSERT_FALSE(key_b.isEmpty());

  NodeScheduler::NodeState computing;
  computing.status = NodeScheduler::NodeState::Status::kPending;
  computing.lag = 250'000'000;
  std::vector<CurveListPanel::CustomTopicRow> rows{
      {.key = key_a, .owner = u"assistant"_s, .name = u"a"_s, .state = computing},
      {.key = key_b, .owner = QString::fromLatin1(kEditor), .name = u"b"_s, .state = NodeScheduler::NodeState{}},
  };
  panel.setCustomTopics(rows);
  tree->expandAll();
  EXPECT_EQ(findRowByCatalogKey(tree->invisibleRootItem(), key_a), nullptr) << "a derived topic leaves Datasets";
  QTreeWidgetItem* row_a = findRowByCatalogKey(custom->invisibleRootItem(), key_a);
  QTreeWidgetItem* row_b = findRowByCatalogKey(custom->invisibleRootItem(), key_b);
  ASSERT_NE(row_a, nullptr);
  ASSERT_NE(row_b, nullptr);
  EXPECT_TRUE(row_a->flags().testFlag(Qt::ItemIsDragEnabled));
  EXPECT_FALSE(row_a->icon(0).isNull()) << "the 3D family icon, as in the tree";

  CurveTreeView::CurvePath spec;
  spec.dataset = u"drive.mcap"_s;
  spec.topic = u"derived/a"_s;
  const QString path_a = CurveTreeView::treePathFromCurvePath(spec);
  spec.topic = u"derived/b"_s;
  const QString path_b = CurveTreeView::treePathFromCurvePath(spec);
  EXPECT_EQ(custom->derivedGlyphStateAt(path_a), CurveTreeView::DerivedGlyphState::kPending);
  EXPECT_EQ(custom->derivedGlyphStateAt(path_b), CurveTreeView::DerivedGlyphState::kIdle);
  EXPECT_TRUE(row_a->toolTip(0).contains(u"Created by assistant"_s)) << row_a->toolTip(0).toStdString();
  EXPECT_TRUE(row_a->toolTip(0).contains(u"Computing; the result shown is 0.250 s behind"_s))
      << row_a->toolTip(0).toStdString();

  // Pencil: another plugin's row is read-only, the editor's is editable.
  custom->clearSelection();
  row_a->setSelected(true);
  EXPECT_FALSE(edit->isEnabled());
  EXPECT_EQ(edit->toolTip(), u"Created by assistant; read-only"_s);
  custom->clearSelection();
  row_b->setSelected(true);
  EXPECT_TRUE(edit->isEnabled());

  // The node fails: same rows, the glyph and the tooltip follow, the row items stay.
  NodeScheduler::NodeState failed;
  failed.status = NodeScheduler::NodeState::Status::kError;
  failed.detail = "script raised";
  rows[0].state = failed;
  panel.setCustomTopics(rows);
  EXPECT_EQ(findRowByCatalogKey(custom->invisibleRootItem(), key_a), row_a) << "a state change is not a refill";
  EXPECT_EQ(custom->derivedGlyphStateAt(path_a), CurveTreeView::DerivedGlyphState::kError);
  EXPECT_TRUE(row_a->toolTip(0).contains(u"Error: script raised"_s)) << row_a->toolTip(0).toStdString();
  EXPECT_FALSE(row_a->toolTip(0).contains(u"Computing"_s));
}

// UI-08: Custom Topics object rows are flat (one top-level row per topic, named after the recipe, `recipe/output`
// never split), an external plugin's rows carry its badge (presentation only), and the tooltip names the creator
// and the dataset.
TEST(CurveListPanelTest, CustomTopicsRowsAreFlatNamedAndBadged) {
  AppSession app_session;
  const DatasetId dataset = pj_test::createDataset(app_session, "drive.mcap");
  ASSERT_NE(dataset, 0U);
  registerClouds(
      app_session, dataset,
      {"assistant/lidar_top_crop/cropped", "assistant/multi/a", "assistant/multi/b",
       "toolbox-transform-editor/mine/out"});
  CurveListPanel panel;
  panel.setEditorPluginId(QString::fromLatin1(kEditor));
  panel.setCatalog(&app_session.catalogModel());
  auto* custom = panel.findChild<CurveTreeView*>(u"customView"_s);
  ASSERT_NE(custom, nullptr);
  const QString crop = keyOfTopic(app_session.catalogModel(), u"assistant/lidar_top_crop/cropped"_s);
  const QString multi_a = keyOfTopic(app_session.catalogModel(), u"assistant/multi/a"_s);
  const QString multi_b = keyOfTopic(app_session.catalogModel(), u"assistant/multi/b"_s);
  const QString mine = keyOfTopic(app_session.catalogModel(), u"toolbox-transform-editor/mine/out"_s);
  for (const QString& key : {crop, multi_a, multi_b, mine}) {
    ASSERT_FALSE(key.isEmpty());
  }
  panel.setOwnerResolver([](const QString& id) {
    return id == u"assistant"_s ? CurveListPanel::OwnerInfo{u"Assistant Agent"_s, u"AI"_s}
                                : CurveListPanel::OwnerInfo{u"Transform Editor"_s, u"TE"_s};
  });
  const QString editor = QString::fromLatin1(kEditor);
  panel.setCustomTopics({
      {.key = crop, .owner = u"assistant"_s, .name = u"lidar_top_crop"_s},
      {.key = multi_a, .owner = u"assistant"_s, .name = u"multi/a"_s},
      {.key = multi_b, .owner = u"assistant"_s, .name = u"multi/b"_s},
      {.key = mine, .owner = editor, .name = u"mine"_s},
  });

  ASSERT_EQ(custom->topLevelItemCount(), 4);
  for (int i = 0; i < custom->topLevelItemCount(); ++i) {
    EXPECT_EQ(custom->topLevelItem(i)->childCount(), 0);
    EXPECT_FALSE(CurveTreeView::catalogKeyOf(custom->topLevelItem(i)).isEmpty());
  }
  QTreeWidgetItem* row_crop = findRowByCatalogKey(custom->invisibleRootItem(), crop);
  QTreeWidgetItem* row_a = findRowByCatalogKey(custom->invisibleRootItem(), multi_a);
  QTreeWidgetItem* row_mine = findRowByCatalogKey(custom->invisibleRootItem(), mine);
  ASSERT_NE(row_crop, nullptr);
  ASSERT_NE(row_a, nullptr);
  ASSERT_NE(row_mine, nullptr);
  EXPECT_EQ(row_crop->text(0), u"lidar_top_crop"_s);
  EXPECT_EQ(row_a->text(0), u"multi/a"_s);
  EXPECT_EQ(row_mine->text(0), u"mine"_s);
  EXPECT_TRUE(row_crop->flags().testFlag(Qt::ItemIsDragEnabled));

  EXPECT_EQ(CurveTreeView::badgeOf(row_crop), u"AI"_s);
  EXPECT_TRUE(CurveTreeView::badgeOf(row_mine).isEmpty()) << "the editor's own rows carry no badge";
  EXPECT_FALSE(row_crop->text(0).contains(u"AI"_s));
  EXPECT_TRUE(row_crop->toolTip(0).contains(u"Created by Assistant Agent"_s)) << row_crop->toolTip(0).toStdString();
  EXPECT_TRUE(row_crop->toolTip(0).contains(u"dataset drive.mcap"_s)) << row_crop->toolTip(0).toStdString();

  // Without a resolver (owner not loaded) the plugin id stands in for the badge.
  CurveListPanel bare;
  bare.setCatalog(&app_session.catalogModel());
  bare.setCustomTopics({{.key = crop, .owner = u"assistant"_s, .name = u"lidar_top_crop"_s}});
  auto* bare_custom = bare.findChild<CurveTreeView*>(u"customView"_s);
  EXPECT_EQ(CurveTreeView::badgeOf(findRowByCatalogKey(bare_custom->invisibleRootItem(), crop)), u"assistant"_s);
}

// UI-09: the pencil AND Delete follow the owner: another plugin's rows are use-only, the editor's rows can be
// edited and deleted, and one use-only row in a selection blocks Delete for all of it.
TEST(CurveListPanelTest, ReadOnlyRowsDisableEditAndDelete) {
  AppSession app_session;
  const DatasetId dataset = pj_test::createDataset(app_session, "drive.mcap");
  ASSERT_NE(dataset, 0U);
  registerClouds(app_session, dataset, {"assistant/a/out", "toolbox-transform-editor/b/out"});
  CurveListPanel panel;
  panel.setEditorPluginId(QString::fromLatin1(kEditor));
  panel.setCatalog(&app_session.catalogModel());
  panel.setOwnerResolver([](const QString& id) {
    return CurveListPanel::OwnerInfo{id == u"assistant"_s ? u"Assistant Agent"_s : id, {}};
  });
  auto* custom = panel.findChild<CurveTreeView*>(u"customView"_s);
  auto* edit = panel.findChild<QToolButton*>(u"buttonEditCustom"_s);
  QPushButton* remove = deleteButtonOf(panel);
  ASSERT_NE(custom, nullptr);
  ASSERT_NE(edit, nullptr);
  ASSERT_NE(remove, nullptr);
  const QString external = keyOfTopic(app_session.catalogModel(), u"assistant/a/out"_s);
  const QString editor = keyOfTopic(app_session.catalogModel(), u"toolbox-transform-editor/b/out"_s);
  panel.setCustomTopics({
      {.key = external, .owner = u"assistant"_s, .name = u"a"_s},
      {.key = editor, .owner = QString::fromLatin1(kEditor), .name = u"b"_s},
  });
  QStringList deleted;
  QObject::connect(
      &panel, &CurveListPanel::deleteCustomTopicsRequested, [&](const QStringList& keys) { deleted << keys; });

  custom->clearSelection();
  findRowByCatalogKey(custom->invisibleRootItem(), external)->setSelected(true);
  EXPECT_FALSE(edit->isEnabled());
  EXPECT_EQ(edit->toolTip(), u"Created by Assistant Agent; read-only"_s);
  EXPECT_FALSE(remove->isEnabled());
  EXPECT_EQ(remove->toolTip(), u"Created by Assistant Agent; read-only"_s);
  remove->click();  // a disabled button never emits
  EXPECT_TRUE(deleted.isEmpty());

  custom->clearSelection();
  findRowByCatalogKey(custom->invisibleRootItem(), editor)->setSelected(true);
  EXPECT_TRUE(edit->isEnabled());
  EXPECT_TRUE(remove->isEnabled());
  remove->click();
  EXPECT_EQ(deleted, QStringList{editor});

  custom->selectAll();
  EXPECT_FALSE(remove->isEnabled()) << "a mixed selection cannot be deleted";
}

// UI-03: a row no plugin owns (a recipe restored without an owner) is no editor's: read-only, like another plugin's.
TEST(CurveListPanelTest, RowsWithNoOwnerAreReadOnly) {
  AppSession app_session;
  const DatasetId dataset = pj_test::createDataset(app_session, "drive.mcap");
  ASSERT_NE(dataset, 0U);
  registerClouds(app_session, dataset, {"legacy/out"});
  CurveListPanel panel;
  panel.setEditorPluginId(QString::fromLatin1(kEditor));
  panel.setCatalog(&app_session.catalogModel());
  auto* custom = panel.findChild<CurveTreeView*>(u"customView"_s);
  auto* edit = panel.findChild<QToolButton*>(u"buttonEditCustom"_s);
  QPushButton* remove = deleteButtonOf(panel);
  ASSERT_NE(custom, nullptr);
  ASSERT_NE(edit, nullptr);
  ASSERT_NE(remove, nullptr);
  const QString key = keyOfTopic(app_session.catalogModel(), u"legacy/out"_s);
  panel.setCustomTopics({{.key = key, .name = u"out"_s}});

  custom->clearSelection();
  findRowByCatalogKey(custom->invisibleRootItem(), key)->setSelected(true);
  EXPECT_FALSE(edit->isEnabled());
  EXPECT_FALSE(remove->isEnabled());
  EXPECT_EQ(remove->toolTip(), u"Read-only"_s);
}

// PIN-09: a finding says when it was computed on the display axis (resolved by the host), never in raw
// nanoseconds, and never as a second line of the row text (D26).
TEST(CurveListPanelTest, PinnedRowTooltipShowsDisplaySeconds) {
  AppSession app_session;
  const DatasetId dataset = pj_test::createDataset(app_session, "drive.mcap");
  ASSERT_NE(dataset, 0U);
  registerClouds(app_session, dataset, {"assistant/pin/out"});
  CurveListPanel panel;
  panel.setCatalog(&app_session.catalogModel());
  const QString key = keyOfTopic(app_session.catalogModel(), u"assistant/pin/out"_s);
  ASSERT_FALSE(key.isEmpty());
  // The timeline origin sits 1000 s after the raw clock's zero.
  panel.setDisplayTimeResolver([&](DatasetId id, Timestamp raw_ns) -> std::optional<double> {
    return id == dataset ? std::optional<double>(static_cast<double>(raw_ns - 1'000'000'000'000LL) / 1e9)
                         : std::nullopt;
  });
  panel.setCustomTopics(
      {{.key = key, .owner = u"assistant"_s, .name = u"pin"_s, .pinned_t = Timestamp{1'012'345'000'000LL}}});

  auto* custom = panel.findChild<CurveTreeView*>(u"customView"_s);
  QTreeWidgetItem* row = findRowByCatalogKey(custom->invisibleRootItem(), key);
  ASSERT_NE(row, nullptr);
  EXPECT_TRUE(row->toolTip(0).contains(u"Pinned at 12.345 s"_s)) << row->toolTip(0).toStdString();
  EXPECT_FALSE(row->toolTip(0).contains(u"ns"_s)) << row->toolTip(0).toStdString();
  EXPECT_EQ(row->text(0), u"pin"_s) << "no subtitle line in the row text";

  // An unknown dataset leaves the line out rather than printing raw time.
  panel.setDisplayTimeResolver([](DatasetId, Timestamp) { return std::optional<double>{}; });
  panel.setCustomTopics({{.key = key, .owner = u"assistant"_s, .name = u"pin2"_s, .pinned_t = Timestamp{5}}});
  row = findRowByCatalogKey(custom->invisibleRootItem(), key);
  ASSERT_NE(row, nullptr);
  EXPECT_FALSE(row->toolTip(0).contains(u"Pinned at"_s)) << row->toolTip(0).toStdString();
}

// UI-15: a state change of one node writes only its row: another row, hovered for its error, sees no model change at
// all, since its glyph and its whole tooltip (creator, state) are written in one pass and are equal.
TEST(CurveListPanelTest, OneNodeStateChangeLeavesOtherRowsUntouched) {
  AppSession app_session;
  const DatasetId dataset = pj_test::createDataset(app_session, "drive.mcap");
  ASSERT_NE(dataset, 0U);
  registerClouds(app_session, dataset, {"derived/a", "derived/b"});
  CurveListPanel panel;
  panel.setCatalog(&app_session.catalogModel());
  auto* custom = panel.findChild<CurveTreeView*>(u"customView"_s);
  ASSERT_NE(custom, nullptr);
  const QString key_a = keyOfTopic(app_session.catalogModel(), u"derived/a"_s);
  const QString key_b = keyOfTopic(app_session.catalogModel(), u"derived/b"_s);
  NodeScheduler::NodeState computing;
  computing.status = NodeScheduler::NodeState::Status::kPending;
  NodeScheduler::NodeState failed;
  failed.status = NodeScheduler::NodeState::Status::kError;
  failed.detail = "boom";
  std::vector<CurveListPanel::CustomTopicRow> rows{
      {.key = key_a, .owner = u"assistant"_s, .name = u"a"_s, .state = computing},
      {.key = key_b, .owner = u"assistant"_s, .name = u"b"_s, .state = failed},
  };
  panel.setCustomTopics(rows);
  QTreeWidgetItem* row_b = findRowByCatalogKey(custom->invisibleRootItem(), key_b);
  ASSERT_NE(row_b, nullptr);
  const QString tooltip_b = row_b->toolTip(0);
  EXPECT_TRUE(tooltip_b.contains(u"Created by assistant"_s)) << tooltip_b.toStdString();
  EXPECT_TRUE(tooltip_b.contains(u"Error: boom"_s)) << tooltip_b.toStdString();
  const int top_row_b = custom->indexOfTopLevelItem(row_b);
  ASSERT_GE(top_row_b, 0);

  QSignalSpy changes(custom->model(), &QAbstractItemModel::dataChanged);
  rows[0].state->lag = 1'000'000'000;
  panel.setCustomTopics(rows);
  int touching_b = 0;
  for (const QList<QVariant>& change : changes) {
    const auto top_left = change.at(0).value<QModelIndex>();
    const auto bottom_right = change.at(1).value<QModelIndex>();
    if (!top_left.parent().isValid() && top_left.row() <= top_row_b && top_row_b <= bottom_right.row()) {
      ++touching_b;
    }
  }
  EXPECT_GT(changes.count(), 0) << "the changed row is written";
  EXPECT_EQ(touching_b, 0) << "the unchanged row is not";
  EXPECT_EQ(row_b->toolTip(0), tooltip_b);
}

// UI-08: a row kept across a merge follows the catalog: the anchor's new label reaches its tooltip although the
// recipe rows did not change.
TEST(CurveListPanelTest, CustomRowFollowsTheCatalogAcrossAMerge) {
  AppSession app_session;
  const DatasetId anchor = pj_test::createDataset(app_session, "run", /*own_time_domain=*/true);
  const DatasetId other = pj_test::createDataset(app_session, "other", /*own_time_domain=*/true);
  ASSERT_NE(anchor, 0U);
  ASSERT_NE(other, 0U);
  ASSERT_NE(pj_test::addScalarTopic(app_session, anchor, "/s", 100, 200), 0U);
  ASSERT_NE(pj_test::addScalarTopic(app_session, other, "/t", 300, 400), 0U);
  registerClouds(app_session, anchor, {"derived/a"});
  CurveListPanel panel;
  panel.setCatalog(&app_session.catalogModel());
  auto* custom = panel.findChild<CurveTreeView*>(u"customView"_s);
  ASSERT_NE(custom, nullptr);
  const QString key = keyOfTopic(app_session.catalogModel(), u"derived/a"_s);
  ASSERT_FALSE(key.isEmpty());
  const std::vector<CurveListPanel::CustomTopicRow> rows{{.key = key, .owner = u"assistant"_s, .name = u"a"_s}};
  panel.setCustomTopics(rows);
  QTreeWidgetItem* row = findRowByCatalogKey(custom->invisibleRootItem(), key);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(row->toolTip(0).contains(u"dataset run"_s)) << row->toolTip(0).toStdString();

  ASSERT_EQ(app_session.mergeDatasets({anchor, other}), anchor);
  app_session.catalogModel().rebuildFromDatastore();
  ASSERT_EQ(keyOfTopic(app_session.catalogModel(), u"derived/a"_s), key) << "the merge keeps the key";
  panel.setCustomTopics(rows);
  row = findRowByCatalogKey(custom->invisibleRootItem(), key);
  ASSERT_NE(row, nullptr);
  EXPECT_TRUE(row->toolTip(0).contains(u"dataset run_merged"_s)) << row->toolTip(0).toStdString();
}

// A custom object row's subscription dimming follows the demand like its Datasets twin: a reference clears it.
TEST(CurveListPanelTest, CustomRowDimmingFollowsTheDemand) {
  AppSession app_session;
  const DatasetId dataset = pj_test::createDataset(app_session, "drive.mcap");
  ASSERT_NE(dataset, 0U);
  registerClouds(app_session, dataset, {"derived/a"});
  app_session.catalogModel().setPerTopicPauseCapable(dataset, true);
  TopicDemandTracker tracker;
  CurveListPanel panel;
  panel.setCatalog(&app_session.catalogModel());
  panel.setTopicDemandTracker(&tracker);
  auto* custom = panel.findChild<CurveTreeView*>(u"customView"_s);
  ASSERT_NE(custom, nullptr);
  const QString key = keyOfTopic(app_session.catalogModel(), u"derived/a"_s);
  panel.setCustomTopics({{.key = key, .owner = u"assistant"_s, .name = u"a"_s}});
  ASSERT_TRUE(custom->isKeyUnsubscribed(key)) << "nothing references it yet";

  tracker.addReference(dataset, u"derived/a"_s);
  EXPECT_FALSE(custom->isKeyUnsubscribed(key));
}

// PIN-09: re-sent rows equal to the shown ones still re-word the pin when the display axis moved.
TEST(CurveListPanelTest, PinnedTooltipFollowsTheAxisWithEqualRows) {
  AppSession app_session;
  const DatasetId dataset = pj_test::createDataset(app_session, "drive.mcap");
  ASSERT_NE(dataset, 0U);
  registerClouds(app_session, dataset, {"assistant/pin/out"});
  CurveListPanel panel;
  panel.setCatalog(&app_session.catalogModel());
  const QString key = keyOfTopic(app_session.catalogModel(), u"assistant/pin/out"_s);
  double origin_s = 0.0;
  panel.setDisplayTimeResolver([&](DatasetId, Timestamp raw_ns) -> std::optional<double> {
    return static_cast<double>(raw_ns) / 1e9 - origin_s;
  });
  const std::vector<CurveListPanel::CustomTopicRow> rows{
      {.key = key, .owner = u"assistant"_s, .name = u"pin"_s, .pinned_t = Timestamp{12'000'000'000LL}}};
  panel.setCustomTopics(rows);
  auto* custom = panel.findChild<CurveTreeView*>(u"customView"_s);
  QTreeWidgetItem* row = findRowByCatalogKey(custom->invisibleRootItem(), key);
  ASSERT_NE(row, nullptr);
  ASSERT_TRUE(row->toolTip(0).contains(u"Pinned at 12.000 s"_s)) << row->toolTip(0).toStdString();

  origin_s = 2.0;
  panel.setCustomTopics(rows);
  EXPECT_TRUE(row->toolTip(0).contains(u"Pinned at 10.000 s"_s)) << row->toolTip(0).toStdString();
}

// UI-10: the "+" stands for an editor to open: hidden until a toolbox declaring custom_topics_editor is named,
// hidden again when it goes.
TEST(CurveListPanelTest, AddButtonFollowsTheDeclaredEditor) {
  CurveListPanel panel;
  auto* add = panel.findChild<QToolButton*>(u"buttonAddCustom"_s);
  ASSERT_NE(add, nullptr);
  EXPECT_TRUE(add->isHidden());
  panel.setEditorPluginId(u"some-editor"_s);
  EXPECT_FALSE(add->isHidden());
  panel.setEditorPluginId(QString{});
  EXPECT_TRUE(add->isHidden());
}

}  // namespace
}  // namespace PJ
