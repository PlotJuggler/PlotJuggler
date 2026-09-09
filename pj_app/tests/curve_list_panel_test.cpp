// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The curve list must not offer an object topic as a drag payload when no view
// can display its type (e.g. a CameraInfo calibration topic): the drop could
// only end in the "Cannot display topic" dead end. The row stays selectable
// (context menu, force-streaming) and carries a tooltip saying why it cannot
// be dragged; displayable object topics keep dragging. Attempting the gesture
// anyway is answered, not swallowed: the tree's not-draggable drag signals surface as
// the panel's toastRequested (the shell shows a toast).

#include <gtest/gtest.h>

#include <QApplication>
#include <QHelpEvent>
#include <QString>
#include <QToolTip>
#include <QTreeWidgetItem>

#include "dataset_test_helpers.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"
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

  auto calib_topic = app_session.sessionManager().objectStore().registerTopic(
      ObjectTopicDescriptor{
          .dataset_id = dataset,
          .topic_name = "/camera/camera_info",
          .metadata_json = R"({"builtin_object_type":"kCameraInfo"})",
      });
  ASSERT_TRUE(calib_topic.has_value()) << calib_topic.error();
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

  const QString calib_key = keyOfTopic(app_session.catalogModel(), u"/camera/camera_info"_s);
  const QString image_key = keyOfTopic(app_session.catalogModel(), u"/camera/image"_s);
  ASSERT_FALSE(calib_key.isEmpty());
  ASSERT_FALSE(image_key.isEmpty());
  QTreeWidgetItem* calib_row = findRowByCatalogKey(tree->invisibleRootItem(), calib_key);
  QTreeWidgetItem* image_row = findRowByCatalogKey(tree->invisibleRootItem(), image_key);
  ASSERT_NE(calib_row, nullptr);
  ASSERT_NE(image_row, nullptr);

  EXPECT_TRUE(calib_row->flags().testFlag(Qt::ItemIsSelectable));
  EXPECT_FALSE(calib_row->flags().testFlag(Qt::ItemIsDragEnabled));
  EXPECT_FALSE(calib_row->toolTip(0).isEmpty());

  EXPECT_TRUE(image_row->flags().testFlag(Qt::ItemIsDragEnabled));
  EXPECT_TRUE(image_row->toolTip(0).isEmpty());

  // Not-draggable drag feedback wiring: the tree's signals must surface as
  // toastRequested. Invoked directly (signals are public-callable) — the
  // gesture mechanics that fire them are covered by curve_tree_view_test.
  QStringList toasts;
  QObject::connect(
      &panel, &CurveListPanel::toastRequested, [&toasts](const QString& message) { toasts.append(message); });
  emit tree->dragAttemptedOnNotDraggableRow(calib_row->toolTip(0));
  ASSERT_EQ(toasts.size(), 1);
  EXPECT_EQ(toasts.front(), calib_row->toolTip(0));  // tooltip doubles as the toast text
  emit tree->dragAttemptedOnNotDraggableRow(QString{});
  ASSERT_EQ(toasts.size(), 2);
  EXPECT_FALSE(toasts.back().isEmpty()) << "empty reason must fall back to a generic wording";
  emit tree->dragPayloadKeysSkipped({calib_key});
  ASSERT_EQ(toasts.size(), 3);
  EXPECT_TRUE(toasts.back().contains(u"/camera/camera_info"_s)) << toasts.back().toStdString();
}

}  // namespace
}  // namespace PJ
