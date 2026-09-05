// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QApplication>
#include <QHelpEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QScrollBar>
#include <QTimer>
#include <QTreeWidgetItem>
#include <QVariant>
#include <QtGlobal>
#include <memory>
#include <string>
#include <vector>

#include "pj_widgets/CurveTreeView.h"
using namespace Qt::StringLiterals;

namespace PJ {

class CurveTreeViewTestPeer {
 public:
  // The ✕ (stop and keep). The bin's rect is discardButtonRect below.
  static QRect cancelButtonRect(const CurveTreeView& view, const QTreeWidgetItem* item) {
    return rowGeometry(view, item).keep_button_rect;
  }

  // The bin (stop and discard), inboard of the ✕.
  static QRect discardButtonRect(const CurveTreeView& view, const QTreeWidgetItem* item) {
    return rowGeometry(view, item).discard_button_rect;
  }

  // The tooltip the view answers a QEvent::ToolTip at `viewport_pos` with.
  // Empty means the position offers none and the event falls through.
  static QString stopButtonTooltip(const CurveTreeView& view, const QPoint& viewport_pos) {
    return view.stopButtonTooltip(viewport_pos);
  }

  // Through the view's own widening + geometry, so a test can never measure
  // from a rect the widget does not use.
  static CurveTreeView::ProgressGeometry rowGeometry(const CurveTreeView& view, const QTreeWidgetItem* item) {
    return view.progressGeometry(view.fullWidthRowRect(view.visualItemRect(item)));
  }

  static bool suppressNextRelease(const CurveTreeView& view) {
    return view.suppress_next_release_;
  }

  // Which stop affordance the view currently believes is hovered. 0 = none,
  // 1 = the ✕, 2 = the bin.
  static int hoveredStopButton(const CurveTreeView& view) {
    if (!view.hovered_stop_row_.isValid()) {
      return 0;
    }
    switch (view.hovered_stop_button_) {
      case CurveTreeView::StopButton::kNone:
        return 0;
      case CurveTreeView::StopButton::kKeep:
        return 1;
      case CurveTreeView::StopButton::kDiscard:
        return 2;
    }
    return 0;
  }

  static Qt::MouseButton dragButton(const CurveTreeView& view) {
    return view.drag_button_;
  }

  static bool dragPayloadIsEmpty(const CurveTreeView& view) {
    return view.drag_curve_names_.empty() && view.drag_catalog_keys_.isEmpty();
  }

  // How many row keys are still filed under a tree path. Observed because the
  // map is otherwise invisible, and a mapping that outlives its key would
  // accumulate for the whole session.
  static int datasetPathMappingCount(const CurveTreeView& view) {
    return static_cast<int>(view.row_key_to_tree_path_.size());
  }
};

}  // namespace PJ

namespace {

// Keep these in lockstep with CurveTreeView.cpp. The roles stay private because
// only the view's painting and interaction internals consume them in production.
constexpr int kDatasetProgressRoleForTest = Qt::UserRole + 13;
constexpr int kDatasetGhostRoleForTest = Qt::UserRole + 14;
constexpr int kDatasetRowKeyRoleForTest = Qt::UserRole + 15;

class TestCurveTreeView : public PJ::CurveTreeView {
 public:
  using PJ::CurveTreeView::mouseMoveEvent;
  using PJ::CurveTreeView::mousePressEvent;
  using PJ::CurveTreeView::mouseReleaseEvent;
  using PJ::CurveTreeView::viewportEvent;
};

std::vector<std::string> toStdStrings(const std::vector<QString>& names) {
  std::vector<std::string> result;
  result.reserve(names.size());
  for (const QString& name : names) {
    result.push_back(name.toStdString());
  }
  return result;
}

std::vector<std::string> topLevelNames(const PJ::CurveTreeView& view) {
  std::vector<std::string> names;
  names.reserve(static_cast<std::size_t>(view.topLevelItemCount()));
  for (int i = 0; i < view.topLevelItemCount(); ++i) {
    names.push_back(view.topLevelItem(i)->text(0).toStdString());
  }
  return names;
}

std::vector<std::string> childNames(const QTreeWidgetItem* item) {
  std::vector<std::string> names;
  names.reserve(static_cast<std::size_t>(item->childCount()));
  for (int i = 0; i < item->childCount(); ++i) {
    names.push_back(item->child(i)->text(0).toStdString());
  }
  return names;
}

QTreeWidgetItem* findChild(QTreeWidgetItem* parent, const QString& name) {
  if (parent == nullptr) {
    return nullptr;
  }
  for (int i = 0; i < parent->childCount(); ++i) {
    if (parent->child(i)->text(0) == name) {
      return parent->child(i);
    }
  }
  return nullptr;
}

QTreeWidgetItem* findTopLevel(PJ::CurveTreeView& view, const QString& name) {
  for (int i = 0; i < view.topLevelItemCount(); ++i) {
    if (view.topLevelItem(i)->text(0) == name) {
      return view.topLevelItem(i);
    }
  }
  return nullptr;
}

void expectDatasetProgress(const QTreeWidgetItem* item, const PJ::CurveTreeView::DatasetProgress& expected) {
  ASSERT_NE(item, nullptr);
  const QVariant data = item->data(0, kDatasetProgressRoleForTest);
  ASSERT_TRUE(data.isValid());
  const auto actual = data.value<PJ::CurveTreeView::DatasetProgress>();
  EXPECT_EQ(actual.display_name, expected.display_name);
  EXPECT_DOUBLE_EQ(actual.fraction, expected.fraction);
  EXPECT_EQ(actual.indeterminate, expected.indeterminate);
  EXPECT_EQ(actual.cancellable, expected.cancellable);
  EXPECT_EQ(actual.state, expected.state);
  EXPECT_EQ(actual.flash_on, expected.flash_on);
}

QHash<quint64, PJ::CurveTreeView::DatasetProgress> progressSet(
    quint64 row_key, const PJ::CurveTreeView::DatasetProgress& progress) {
  QHash<quint64, PJ::CurveTreeView::DatasetProgress> result;
  result.insert(row_key, progress);
  return result;
}

// Gesture events dispatched straight to the protected handlers; `pos` is in
// viewport coordinates (visualItemRect space).
bool sendMousePress(TestCurveTreeView& view, const QPoint& pos, Qt::MouseButton button = Qt::LeftButton) {
  const QPointF local(pos);
  const QPointF global(view.viewport()->mapToGlobal(pos));
  QMouseEvent event(QEvent::MouseButtonPress, local, local, global, button, button, Qt::NoModifier);
  event.ignore();
  view.mousePressEvent(&event);
  return event.isAccepted();
}

void sendMouseMove(TestCurveTreeView& view, const QPoint& pos) {
  const QPointF local(pos);
  const QPointF global(view.viewport()->mapToGlobal(pos));
  QMouseEvent event(QEvent::MouseMove, local, local, global, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
  view.mouseMoveEvent(&event);
}

void sendMouseRelease(TestCurveTreeView& view, const QPoint& pos, Qt::MouseButton button = Qt::LeftButton) {
  const QPointF local(pos);
  const QPointF global(view.viewport()->mapToGlobal(pos));
  QMouseEvent event(QEvent::MouseButtonRelease, local, local, global, button, Qt::NoButton, Qt::NoModifier);
  view.mouseReleaseEvent(&event);
}

QPoint cancelButtonPosition(const TestCurveTreeView& view, const QTreeWidgetItem* item) {
  return PJ::CurveTreeViewTestPeer::cancelButtonRect(view, item).center();
}

QPoint discardButtonPosition(const TestCurveTreeView& view, const QTreeWidgetItem* item) {
  return PJ::CurveTreeViewTestPeer::discardButtonRect(view, item).center();
}

// A buttonless move — hover tracking must work without a button held, which the
// existing sendMouseMove (LeftButton down, for drag tests) does not exercise.
void sendHoverMove(TestCurveTreeView& view, const QPoint& pos) {
  const QPointF local(pos);
  const QPointF global(view.viewport()->mapToGlobal(pos));
  QMouseEvent event(QEvent::MouseMove, local, local, global, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
  view.mouseMoveEvent(&event);
}

// The not-draggable object-topic row (an undisplayable type, host policy) shared
// by the not-draggable drag tests.
PJ::CurveTreeView::CurvePath calibCurvePath() {
  return PJ::CurveTreeView::CurvePath{
      .key = u"object:calib"_s,
      .dataset = u"drive.mcap"_s,
      .topic = u"/camera/camera_info"_s,
      .field = {},
      .selectable = false,
      .draggable = false,
      .tooltip = u"Camera calibration topic"_s,
  };
}

}  // namespace

TEST(CurveTreeViewTest, SortsTopLevelGroupsAndChildren) {
  PJ::CurveTreeView view;

  view.addCurve(u"gamma/zeta"_s);
  view.addCurve(u"alpha/delta"_s);
  view.addCurve(u"beta/root"_s);
  view.addCurve(u"alpha/charlie"_s);
  view.addCurve(u"alpha/bravo"_s);

  EXPECT_EQ(topLevelNames(view), (std::vector<std::string>{"alpha", "beta", "gamma"}));

  ASSERT_EQ(view.topLevelItemCount(), 3);
  QTreeWidgetItem* alpha = view.topLevelItem(0);
  ASSERT_EQ(alpha->text(0), u"alpha"_s);
  EXPECT_EQ(childNames(alpha), (std::vector<std::string>{"bravo", "charlie", "delta"}));
}

TEST(CurveTreeViewTest, BatchedCatalogInsertSortsTopLevelGroupsAndChildren) {
  PJ::CurveTreeView view;

  view.addCatalogItems({
      PJ::CurveTreeView::CurvePath{
          .key = u"gamma/zeta"_s,
          .dataset = u"gamma"_s,
          .topic = {},
          .field = u"zeta"_s,
      },
      PJ::CurveTreeView::CurvePath{
          .key = u"alpha/delta"_s,
          .dataset = u"alpha"_s,
          .topic = {},
          .field = u"delta"_s,
      },
      PJ::CurveTreeView::CurvePath{
          .key = u"beta/root"_s,
          .dataset = u"beta"_s,
          .topic = {},
          .field = u"root"_s,
      },
      PJ::CurveTreeView::CurvePath{
          .key = u"alpha/charlie"_s,
          .dataset = u"alpha"_s,
          .topic = {},
          .field = u"charlie"_s,
      },
      PJ::CurveTreeView::CurvePath{
          .key = u"alpha/bravo"_s,
          .dataset = u"alpha"_s,
          .topic = {},
          .field = u"bravo"_s,
      },
  });

  EXPECT_EQ(topLevelNames(view), (std::vector<std::string>{"alpha", "beta", "gamma"}));

  ASSERT_EQ(view.topLevelItemCount(), 3);
  QTreeWidgetItem* alpha = view.topLevelItem(0);
  ASSERT_EQ(alpha->text(0), u"alpha"_s);
  EXPECT_EQ(childNames(alpha), (std::vector<std::string>{"bravo", "charlie", "delta"}));
}

// A filter typed BEFORE data is loaded must apply to the rows that arrive later
// (the catalog-insert path), not just to rows already in the tree.
TEST(CurveTreeViewTest, FilterAppliesToRowsInsertedAfterItWasSet) {
  PJ::CurveTreeView view;

  view.applyFilter(u"imu"_s);

  view.addCatalogItems({
      PJ::CurveTreeView::CurvePath{
          .key = u"veh/imu/x"_s,
          .dataset = u"veh"_s,
          .topic = u"imu"_s,
          .field = u"x"_s,
      },
      PJ::CurveTreeView::CurvePath{
          .key = u"veh/gps/lat"_s,
          .dataset = u"veh"_s,
          .topic = u"gps"_s,
          .field = u"lat"_s,
      },
  });

  ASSERT_EQ(view.topLevelItemCount(), 1);
  QTreeWidgetItem* veh = view.topLevelItem(0);
  EXPECT_FALSE(veh->isHidden());

  QTreeWidgetItem* imu = findChild(veh, u"imu"_s);
  ASSERT_NE(imu, nullptr);
  EXPECT_FALSE(imu->isHidden());
  EXPECT_FALSE(findChild(imu, u"x"_s)->isHidden());

  QTreeWidgetItem* gps = findChild(veh, u"gps"_s);
  ASSERT_NE(gps, nullptr);
  EXPECT_TRUE(gps->isHidden());
  EXPECT_TRUE(findChild(gps, u"lat"_s)->isHidden());
}

TEST(CurveTreeViewTest, TopLevelGroupsSelectableIntermediateGroupsLeafOnly) {
  PJ::CurveTreeView view;

  // Two-level path: "dataset" (top-level group) / "folder" (intermediate) / leaf.
  view.addCurve(u"dataset/folder/b"_s);
  view.addCurve(u"dataset/folder/a"_s);

  ASSERT_EQ(view.selectionMode(), QAbstractItemView::ExtendedSelection);
  ASSERT_EQ(view.selectionBehavior(), QAbstractItemView::SelectRows);

  // Top-level groups are datasets: selectable (so they can be multi-selected for
  // the merge / remove context menu) but never drag sources.
  QTreeWidgetItem* dataset = view.topLevelItem(0);
  ASSERT_NE(dataset, nullptr);
  EXPECT_TRUE(dataset->flags().testFlag(Qt::ItemIsSelectable));
  EXPECT_FALSE(dataset->flags().testFlag(Qt::ItemIsDragEnabled));

  // Intermediate (topic-path) folders stay non-selectable + non-draggable.
  ASSERT_EQ(dataset->childCount(), 1);
  QTreeWidgetItem* folder = dataset->child(0);
  EXPECT_FALSE(folder->flags().testFlag(Qt::ItemIsSelectable));
  EXPECT_FALSE(folder->flags().testFlag(Qt::ItemIsDragEnabled));

  // Leaves remain selectable + draggable.
  ASSERT_EQ(folder->childCount(), 2);
  EXPECT_TRUE(folder->child(0)->flags().testFlag(Qt::ItemIsSelectable));
  EXPECT_TRUE(folder->child(1)->flags().testFlag(Qt::ItemIsSelectable));
}

TEST(CurveTreeViewTest, ReturnsSortedSelectedLeafCurveNames) {
  PJ::CurveTreeView view;

  view.addCurve(u"root/b"_s);
  view.addCurve(u"root/a"_s);
  view.addCurve(u"z"_s);

  QTreeWidgetItem* root = view.topLevelItem(0);
  ASSERT_NE(root, nullptr);
  ASSERT_EQ(root->text(0), u"root"_s);
  ASSERT_EQ(root->childCount(), 2);
  root->child(1)->setSelected(true);
  root->child(0)->setSelected(true);
  view.topLevelItem(1)->setSelected(true);

  EXPECT_EQ(toStdStrings(view.selectedCurveNames()), (std::vector<std::string>{"root/a", "root/b", "z"}));
  EXPECT_EQ(toStdStrings(view.selectedCurveNamesRecursive()), (std::vector<std::string>{"root/a", "root/b", "z"}));
}

TEST(CurveTreeViewTest, PressingSelectedItemDoesNotCollapseMultiSelection) {
  TestCurveTreeView view;
  view.resize(240, 200);

  view.addCurve(u"root/b"_s);
  view.addCurve(u"root/a"_s);
  view.addCurve(u"z"_s);
  view.expandAll();
  view.show();
  QApplication::processEvents();

  QTreeWidgetItem* root = view.topLevelItem(0);
  ASSERT_NE(root, nullptr);
  ASSERT_EQ(root->childCount(), 2);
  QTreeWidgetItem* first_leaf = root->child(0);
  QTreeWidgetItem* second_leaf = root->child(1);
  QTreeWidgetItem* top_leaf = view.topLevelItem(1);
  ASSERT_NE(first_leaf, nullptr);
  ASSERT_NE(second_leaf, nullptr);
  ASSERT_NE(top_leaf, nullptr);

  first_leaf->setSelected(true);
  second_leaf->setSelected(true);
  top_leaf->setSelected(true);

  const QPoint press_pos = view.visualItemRect(first_leaf).center();
  sendMousePress(view, press_pos);

  EXPECT_TRUE(first_leaf->isSelected());
  EXPECT_TRUE(second_leaf->isSelected());
  EXPECT_TRUE(top_leaf->isSelected());

  sendMouseRelease(view, press_pos);

  EXPECT_TRUE(first_leaf->isSelected());
  EXPECT_TRUE(second_leaf->isSelected());
  EXPECT_TRUE(top_leaf->isSelected());
}

TEST(CurveTreeViewTest, DoubleClickOnDatasetTogglesOnlyDatasetExpansion) {
  PJ::CurveTreeView view;

  view.addCurve(u"root/branch/leaf_a"_s);
  view.addCurve(u"root/branch/leaf_b"_s);

  QTreeWidgetItem* root = view.topLevelItem(0);
  ASSERT_NE(root, nullptr);
  ASSERT_EQ(root->childCount(), 1);
  QTreeWidgetItem* branch = root->child(0);
  ASSERT_NE(branch, nullptr);

  view.collapseAll();
  EXPECT_FALSE(root->isExpanded());
  EXPECT_FALSE(branch->isExpanded());

  Q_EMIT view.itemDoubleClicked(root, 0);
  EXPECT_TRUE(root->isExpanded());
  EXPECT_FALSE(branch->isExpanded());

  Q_EMIT view.itemDoubleClicked(root, 0);
  EXPECT_FALSE(root->isExpanded());
  EXPECT_FALSE(branch->isExpanded());
}

TEST(CurveTreeViewTest, DoubleClickBelowDatasetTogglesWholeSubtreeExpansion) {
  PJ::CurveTreeView view;

  view.addCurve(u"root/branch/subbranch/leaf_a"_s);
  view.addCurve(u"root/branch/subbranch/leaf_b"_s);

  QTreeWidgetItem* root = view.topLevelItem(0);
  ASSERT_NE(root, nullptr);
  ASSERT_EQ(root->childCount(), 1);
  QTreeWidgetItem* branch = root->child(0);
  ASSERT_NE(branch, nullptr);
  ASSERT_EQ(branch->childCount(), 1);
  QTreeWidgetItem* subbranch = branch->child(0);
  ASSERT_NE(subbranch, nullptr);

  view.collapseAll();
  EXPECT_FALSE(branch->isExpanded());
  EXPECT_FALSE(subbranch->isExpanded());

  Q_EMIT view.itemDoubleClicked(branch, 0);
  EXPECT_TRUE(branch->isExpanded());
  EXPECT_TRUE(subbranch->isExpanded());

  Q_EMIT view.itemDoubleClicked(branch, 0);
  EXPECT_FALSE(branch->isExpanded());
  EXPECT_FALSE(subbranch->isExpanded());
}

TEST(CurveTreeViewTest, ObjectTopicsUseTopicNodeWithoutEnteringCurveSelection) {
  PJ::CurveTreeView view;

  view.addCatalogItem(
      PJ::CurveTreeView::CurvePath{
          .key = u"object:1"_s,
          .dataset = u"drive.mcap"_s,
          .topic = u"/camera/image"_s,
          .field = {},
          .selectable = false,
          .is_image_topic = true,
      });
  view.addCurve(
      PJ::CurveTreeView::CurvePath{
          .key = u"curve:1"_s,
          .dataset = u"drive.mcap"_s,
          .topic = u"/camera/image"_s,
          .field = u"byte_count"_s,
      });

  ASSERT_EQ(view.topLevelItemCount(), 1);
  QTreeWidgetItem* dataset = view.topLevelItem(0);
  ASSERT_NE(dataset, nullptr);
  QTreeWidgetItem* camera = findChild(dataset, u"camera"_s);
  ASSERT_NE(camera, nullptr);
  QTreeWidgetItem* image = findChild(camera, u"image"_s);
  ASSERT_NE(image, nullptr);
  EXPECT_FALSE(image->font(0).italic());
  EXPECT_FALSE(image->icon(0).isNull());
  EXPECT_TRUE(image->flags().testFlag(Qt::ItemIsSelectable));
  EXPECT_TRUE(image->flags().testFlag(Qt::ItemIsDragEnabled));

  ASSERT_EQ(image->childCount(), 1);
  EXPECT_EQ(image->child(0)->text(0), u"byte_count"_s);
  EXPECT_TRUE(image->child(0)->flags().testFlag(Qt::ItemIsSelectable));

  image->setSelected(true);
  EXPECT_TRUE(view.selectedCurveNamesRecursive().empty());
  EXPECT_EQ(toStdStrings(view.selectedCatalogKeysRecursive()), (std::vector<std::string>{"object:1"}));

  image->child(0)->setSelected(true);
  EXPECT_EQ(toStdStrings(view.selectedCurveNamesRecursive()), (std::vector<std::string>{"curve:1"}));
  EXPECT_EQ(toStdStrings(view.selectedCatalogKeysRecursive()), (std::vector<std::string>{"curve:1", "object:1"}));
}

TEST(CurveTreeViewTest, EncodesCatalogItemDragPayloads) {
  QMimeData mime_data;
  mime_data.setData(
      PJ::CurveTreeView::catalogItemsMimeType(), PJ::CurveTreeView::encodeCatalogKeys({u"object:1"_s, u"curve:1"_s}));

  const QStringList keys = PJ::CurveTreeView::decodeCatalogKeys(&mime_data);
  ASSERT_EQ(keys.size(), 2);
  EXPECT_EQ(keys[0], u"object:1"_s);
  EXPECT_EQ(keys[1], u"curve:1"_s);
}

// Regression: dragging a multi-selection onto an empty pane (which consumes the
// catalog-key payload) must add every selected curve, not just one. The catalog
// payload used to carry only the row under the cursor at press time.
TEST(CurveTreeViewTest, DragPayloadCarriesEverySelectedScalarCurve) {
  PJ::CurveTreeView view;
  view.addCurve(u"vehicle/speed"_s);
  view.addCurve(u"vehicle/rpm"_s);
  view.addCurve(u"vehicle/temp"_s);

  QTreeWidgetItem* group = view.topLevelItem(0);
  ASSERT_NE(group, nullptr);
  for (const char* leaf : {"speed", "rpm", "temp"}) {
    QTreeWidgetItem* item = findChild(group, QString::fromLatin1(leaf));
    ASSERT_NE(item, nullptr) << leaf;
    item->setSelected(true);
  }

  std::unique_ptr<QMimeData> mime(view.createDragMimeData(Qt::LeftButton));
  ASSERT_NE(mime, nullptr);

  // Catalog payload — consumed when dropping on an empty pane / placeholder.
  const QStringList catalog_keys = PJ::CurveTreeView::decodeCatalogKeys(mime.get());
  EXPECT_EQ(catalog_keys.size(), 3);
  EXPECT_TRUE(catalog_keys.contains("vehicle/speed"_L1));
  EXPECT_TRUE(catalog_keys.contains("vehicle/rpm"_L1));
  EXPECT_TRUE(catalog_keys.contains("vehicle/temp"_L1));

  // Curve-name payload — consumed when dropping on an existing plot.
  ASSERT_TRUE(mime->hasFormat(u"curveslist/add_curve"_s));
  QByteArray encoded = mime->data(u"curveslist/add_curve"_s);
  QDataStream stream(&encoded, QIODevice::ReadOnly);
  int curve_count = 0;
  while (!stream.atEnd()) {
    QString name;
    stream >> name;
    if (!name.isEmpty()) {
      ++curve_count;
    }
  }
  EXPECT_EQ(curve_count, 3);
}

// Regression: the same defect on the object-topic side — a multi-selection of
// image/object topics (which carry no scalar curve names) must still ship every
// selected catalog key.
TEST(CurveTreeViewTest, DragPayloadCarriesEverySelectedObjectTopic) {
  PJ::CurveTreeView view;
  view.addCatalogItem(
      PJ::CurveTreeView::CurvePath{
          .key = u"object:a"_s,
          .dataset = u"drive.mcap"_s,
          .topic = u"/camera/front"_s,
          .field = {},
          .selectable = false,
          .is_image_topic = true,
      });
  view.addCatalogItem(
      PJ::CurveTreeView::CurvePath{
          .key = u"object:b"_s,
          .dataset = u"drive.mcap"_s,
          .topic = u"/camera/rear"_s,
          .field = {},
          .selectable = false,
          .is_image_topic = true,
      });

  QTreeWidgetItem* dataset = view.topLevelItem(0);
  ASSERT_NE(dataset, nullptr);
  QTreeWidgetItem* camera = findChild(dataset, u"camera"_s);
  ASSERT_NE(camera, nullptr);
  QTreeWidgetItem* front = findChild(camera, u"front"_s);
  QTreeWidgetItem* rear = findChild(camera, u"rear"_s);
  ASSERT_NE(front, nullptr);
  ASSERT_NE(rear, nullptr);
  front->setSelected(true);
  rear->setSelected(true);

  std::unique_ptr<QMimeData> mime(view.createDragMimeData(Qt::LeftButton));
  ASSERT_NE(mime, nullptr);

  const QStringList catalog_keys = PJ::CurveTreeView::decodeCatalogKeys(mime.get());
  EXPECT_EQ(catalog_keys.size(), 2);
  EXPECT_TRUE(catalog_keys.contains("object:a"_L1));
  EXPECT_TRUE(catalog_keys.contains("object:b"_L1));
}

// An object topic whose type no view can display (draggable=false, e.g. a
// CameraInfo topic) stays selectable but never starts a drag, shows the
// caller-supplied "why" tooltip, and is skipped by the drag payload even when
// it sits inside a dragged multi-selection.
TEST(CurveTreeViewTest, UndisplayableObjectTopicIsNotDraggableAndExcludedFromDragPayload) {
  PJ::CurveTreeView view;
  view.addCatalogItem(
      PJ::CurveTreeView::CurvePath{
          .key = u"object:cloud"_s,
          .dataset = u"drive.mcap"_s,
          .topic = u"/lidar/points"_s,
          .field = {},
          .selectable = false,
          .is_3d_object_topic = true,
      });
  view.addCatalogItem(calibCurvePath());

  QTreeWidgetItem* dataset = view.topLevelItem(0);
  ASSERT_NE(dataset, nullptr);
  QTreeWidgetItem* calib = findChild(findChild(dataset, u"camera"_s), u"camera_info"_s);
  QTreeWidgetItem* cloud = findChild(findChild(dataset, u"lidar"_s), u"points"_s);
  ASSERT_NE(calib, nullptr);
  ASSERT_NE(cloud, nullptr);

  EXPECT_TRUE(calib->flags().testFlag(Qt::ItemIsSelectable));
  EXPECT_FALSE(calib->flags().testFlag(Qt::ItemIsDragEnabled));
  EXPECT_EQ(calib->toolTip(0), u"Camera calibration topic"_s);
  EXPECT_TRUE(cloud->flags().testFlag(Qt::ItemIsDragEnabled));
  EXPECT_TRUE(cloud->toolTip(0).isEmpty());

  // Alone, the undisplayable topic produces no drag payload at all — but the
  // COMPLETE selection collector still sees it: deletion and selection-count
  // logic must not mistake "only not-draggable rows selected" for "nothing selected"
  // (which the trash path widens to "everything").
  calib->setSelected(true);
  EXPECT_EQ(toStdStrings(view.selectedCatalogKeysRecursive()), (std::vector<std::string>{"object:calib"}));
  std::unique_ptr<QMimeData> solo(view.createDragMimeData(Qt::LeftButton));
  EXPECT_EQ(solo, nullptr);

  // In a mixed selection, only the displayable topic's key ships; the not-draggable
  // key is reported through the skipped-keys out-param. The complete collector
  // keeps returning both.
  cloud->setSelected(true);
  QStringList skipped_keys;
  std::unique_ptr<QMimeData> mixed(view.createDragMimeData(Qt::LeftButton, &skipped_keys));
  ASSERT_NE(mixed, nullptr);
  const QStringList catalog_keys_mixed = PJ::CurveTreeView::decodeCatalogKeys(mixed.get());
  const std::vector<QString> mixed_keys(catalog_keys_mixed.begin(), catalog_keys_mixed.end());
  EXPECT_EQ(toStdStrings(mixed_keys), (std::vector<std::string>{"object:cloud"}));
  EXPECT_EQ(skipped_keys, QStringList{u"object:calib"_s});
  EXPECT_EQ(
      toStdStrings(view.selectedCatalogKeysRecursive()), (std::vector<std::string>{"object:calib", "object:cloud"}));
}

// A pull past the drag threshold on a not-draggable object row starts no drag,
// so the view must say why: dragAttemptedOnNotDraggableRow fires once per press gesture,
// carrying the row's tooltip for the host to surface (PJ4 shows a toast).
TEST(CurveTreeViewTest, PullOnNotDraggableObjectTopicEmitsNoticeOnce) {
  TestCurveTreeView view;
  view.resize(320, 240);
  view.addCatalogItem(calibCurvePath());
  view.expandAll();
  view.show();
  QApplication::processEvents();

  QStringList attempt_reasons;
  QObject::connect(
      &view, &PJ::CurveTreeView::dragAttemptedOnNotDraggableRow,
      [&attempt_reasons](const QString& reason) { attempt_reasons.append(reason); });

  QTreeWidgetItem* calib = findChild(findChild(view.topLevelItem(0), u"camera"_s), u"camera_info"_s);
  ASSERT_NE(calib, nullptr);
  const QPoint press_pos = view.visualItemRect(calib).center();
  sendMousePress(view, press_pos);

  // Below the threshold the gesture still reads as a click: no notice.
  sendMouseMove(view, press_pos + QPoint(QApplication::startDragDistance() / 2, 0));
  EXPECT_TRUE(attempt_reasons.isEmpty());

  // Past the threshold: exactly one notice, even if the pull continues.
  sendMouseMove(view, press_pos + QPoint(QApplication::startDragDistance() + 10, 0));
  sendMouseMove(view, press_pos + QPoint(QApplication::startDragDistance() + 30, 0));
  EXPECT_EQ(attempt_reasons, QStringList{u"Camera calibration topic"_s});
}

// The "Value" column keeps decimal points vertically aligned in a monospace
// right-aligned cell by formatting at a fixed precision then blanking trailing
// zeros (and a bare trailing dot) with spaces. Ported from PJ3.
TEST(CurveTreeViewTest, FormatScalarForColumnTrimsTrailingZerosToAlignDecimals) {
  EXPECT_EQ(PJ::formatScalarForColumn(1.2, 3), u"1.2"_s + QString(3, QChar(' ')));
  EXPECT_EQ(PJ::formatScalarForColumn(5.0, 3), u"5"_s + QString(5, QChar(' ')));
  EXPECT_EQ(PJ::formatScalarForColumn(-0.001, 3), u"-0.001 "_s);
  EXPECT_EQ(PJ::formatScalarForColumn(123.456, 3), u"123.456 "_s);
}

// refreshVisibleValues fills column 1 only for leaf rows, via the supplied
// provider keyed on each leaf's catalog key; group (non-leaf) rows stay empty.
TEST(CurveTreeViewTest, RefreshVisibleValuesFillsScalarLeavesAndSkipsGroups) {
  PJ::CurveTreeView view;
  view.addCurve(u"vehicle/speed"_s);
  view.addCurve(u"vehicle/rpm"_s);
  view.setValuesColumnHidden(false);
  view.expandAll();
  view.resize(400, 300);
  view.show();
  QApplication::processEvents();

  view.refreshVisibleValues([](const QString& key) { return key.isEmpty() ? QString() : u"42 "_s; });

  QTreeWidgetItem* group = view.topLevelItem(0);
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(group->text(1), QString()) << "group node carries no value";
  ASSERT_EQ(group->childCount(), 2);
  EXPECT_EQ(group->child(0)->text(1), u"42 "_s);
  EXPECT_EQ(group->child(1)->text(1), u"42 "_s);
}

// When the value column is hidden the refresh is a no-op, so cells are not
// recomputed (and the per-row data reads are skipped entirely).
TEST(CurveTreeViewTest, RefreshVisibleValuesIsNoOpWhenValueColumnHidden) {
  PJ::CurveTreeView view;
  view.addCurve(u"vehicle/speed"_s);
  view.setValuesColumnHidden(false);
  view.expandAll();
  view.resize(400, 300);
  view.show();
  QApplication::processEvents();

  view.refreshVisibleValues([](const QString&) { return u"42 "_s; });
  QTreeWidgetItem* leaf = view.topLevelItem(0)->child(0);
  ASSERT_NE(leaf, nullptr);
  ASSERT_EQ(leaf->text(1), u"42 "_s);

  view.setValuesColumnHidden(true);
  view.refreshVisibleValues([](const QString&) { return u"99 "_s; });
  EXPECT_EQ(leaf->text(1), u"42 "_s) << "hidden value column must not refresh";
}

// Regression: expanding a collapsed group must fill the newly-revealed leaves
// from the retained provider, without the caller re-driving the refresh
// (previously a freshly-expanded row stayed blank until the tracker moved).
TEST(CurveTreeViewTest, RefreshVisibleValuesRefillsRowsRevealedByExpansion) {
  PJ::CurveTreeView view;
  view.addCurve(u"vehicle/speed"_s);
  view.setValuesColumnHidden(false);
  view.resize(400, 300);
  view.show();
  view.collapseAll();
  QApplication::processEvents();

  view.refreshVisibleValues([](const QString& key) { return key.isEmpty() ? QString() : u"42 "_s; });

  QTreeWidgetItem* group = view.topLevelItem(0);
  ASSERT_NE(group, nullptr);
  ASSERT_EQ(group->childCount(), 1);
  QTreeWidgetItem* leaf = group->child(0);
  EXPECT_EQ(leaf->text(1), QString()) << "collapsed leaf is not filled yet";

  view.expandAll();
  QApplication::processEvents();  // flush the deferred re-apply scheduled by itemExpanded
  EXPECT_EQ(leaf->text(1), u"42 "_s) << "expanding must fill the revealed leaf";
}

// Regression: scrolling down must fill the rows that scroll into view at the
// bottom of the viewport. A walk that stops at the first off-screen row (rather
// than culling each row independently) leaves the freshly-revealed rows blank.
TEST(CurveTreeViewTest, RefreshVisibleValuesFillsRowsRevealedByScrolling) {
  PJ::CurveTreeView view;
  for (int i = 0; i < 60; ++i) {
    view.addCurve(u"grp/c%1"_s.arg(i, 2, 10, QChar('0')));
  }
  view.setValuesColumnHidden(false);
  view.expandAll();
  view.resize(300, 120);  // small viewport so the 60 rows overflow and can scroll
  view.show();
  QApplication::processEvents();

  view.refreshVisibleValues([](const QString& key) { return key.isEmpty() ? QString() : u"v "_s; });

  QScrollBar* scroll = view.verticalScrollBar();
  ASSERT_GT(scroll->maximum(), 0) << "content must overflow for the scroll case to be meaningful";
  scroll->setValue(scroll->maximum());  // scroll to the bottom
  QApplication::processEvents();        // flush the deferred refresh from valueChanged

  QTreeWidgetItem* group = view.topLevelItem(0);
  ASSERT_NE(group, nullptr);
  QTreeWidgetItem* last_leaf = group->child(group->childCount() - 1);
  ASSERT_NE(last_leaf, nullptr);
  EXPECT_EQ(last_leaf->text(1), u"v "_s) << "row scrolled into view must be filled";
}

// A value-only leaf (draggable=false, e.g. a string field) is shown and
// selectable but is NOT a drag source and never enters the drag payload.
TEST(CurveTreeViewTest, ValueOnlyLeafIsNotDraggableAndExcludedFromDragPayload) {
  PJ::CurveTreeView view;
  view.addCatalogItem(
      PJ::CurveTreeView::CurvePath{
          .key = u"curve:num"_s,
          .dataset = u"drive.mcap"_s,
          .topic = u"/diag"_s,
          .field = u"value"_s,
          .selectable = true,
          .draggable = true,
      });
  view.addCatalogItem(
      PJ::CurveTreeView::CurvePath{
          .key = u"curve:str"_s,
          .dataset = u"drive.mcap"_s,
          .topic = u"/diag"_s,
          .field = u"frame_id"_s,
          .selectable = true,
          .draggable = false,
      });

  QTreeWidgetItem* dataset = view.topLevelItem(0);
  ASSERT_NE(dataset, nullptr);
  QTreeWidgetItem* diag = findChild(dataset, u"diag"_s);
  ASSERT_NE(diag, nullptr);
  QTreeWidgetItem* num = findChild(diag, u"value"_s);
  QTreeWidgetItem* str = findChild(diag, u"frame_id"_s);
  ASSERT_NE(num, nullptr);
  ASSERT_NE(str, nullptr);

  // The string field is selectable (highlightable) but not a drag source.
  EXPECT_TRUE(str->flags().testFlag(Qt::ItemIsSelectable));
  EXPECT_FALSE(str->flags().testFlag(Qt::ItemIsDragEnabled));
  EXPECT_TRUE(num->flags().testFlag(Qt::ItemIsDragEnabled));

  // Selecting both yields a drag payload with only the numeric curve.
  num->setSelected(true);
  str->setSelected(true);
  EXPECT_EQ(toStdStrings(view.selectedCurveNamesRecursive()), (std::vector<std::string>{"curve:num"}));
  EXPECT_EQ(toStdStrings(view.selectedCatalogKeysRecursive()), (std::vector<std::string>{"curve:num"}));

  // A string-only selection produces no draggable payload at all.
  num->setSelected(false);
  std::unique_ptr<QMimeData> mime(view.createDragMimeData(Qt::LeftButton));
  EXPECT_EQ(mime, nullptr) << "a string-only selection must not start a drag";
}

TEST(CurveTreeViewTest, ExpandedGroupPathsSurviveRebuild) {
  // A full rebuild (clearCurves + re-add) runs on every catalog removal —
  // routine under demand-driven streaming (placeholder supersede). The
  // expanded-state snapshot must restore what still exists, skip what
  // vanished, and leave everything else collapsed as it was.
  PJ::CurveTreeView view;
  const auto add_all = [&view]() {
    view.addCatalogItems({
        PJ::CurveTreeView::CurvePath{
            .key = u"k1"_s,
            .dataset = u"robot"_s,
            .topic = u"imu"_s,
            .field = u"x"_s,
        },
        PJ::CurveTreeView::CurvePath{
            .key = u"k2"_s,
            .dataset = u"robot"_s,
            .topic = u"odom"_s,
            .field = u"x"_s,
        },
    });
  };
  add_all();

  QTreeWidgetItem* robot = view.topLevelItem(0);
  ASSERT_NE(robot, nullptr);
  robot->setExpanded(true);
  QTreeWidgetItem* imu = findChild(robot, u"imu"_s);
  ASSERT_NE(imu, nullptr);
  imu->setExpanded(true);
  QTreeWidgetItem* odom = findChild(robot, u"odom"_s);
  ASSERT_NE(odom, nullptr);
  ASSERT_FALSE(odom->isExpanded());

  const QStringList expanded = view.expandedGroupPaths();

  view.clearCurves();
  add_all();
  QTreeWidgetItem* rebuilt_robot = view.topLevelItem(0);
  ASSERT_NE(rebuilt_robot, nullptr);
  ASSERT_FALSE(rebuilt_robot->isExpanded());

  view.restoreExpandedGroupPaths(expanded);

  EXPECT_TRUE(rebuilt_robot->isExpanded());
  QTreeWidgetItem* rebuilt_imu = findChild(rebuilt_robot, u"imu"_s);
  ASSERT_NE(rebuilt_imu, nullptr);
  EXPECT_TRUE(rebuilt_imu->isExpanded());
  QTreeWidgetItem* rebuilt_odom = findChild(rebuilt_robot, u"odom"_s);
  ASSERT_NE(rebuilt_odom, nullptr);
  EXPECT_FALSE(rebuilt_odom->isExpanded());
}

TEST(CurveTreeViewTest, SetUnsubscribedKeysReplacesTheFullSet) {
  PJ::CurveTreeView view;
  view.addCatalogItems({
      PJ::CurveTreeView::CurvePath{
          .key = u"k1"_s,
          .dataset = u"robot"_s,
          .topic = u"imu"_s,
          .field = u"x"_s,
      },
      PJ::CurveTreeView::CurvePath{
          .key = u"k2"_s,
          .dataset = u"robot"_s,
          .topic = u"odom"_s,
          .field = u"x"_s,
      },
  });

  EXPECT_FALSE(view.isKeyUnsubscribed(u"k1"_s));
  EXPECT_FALSE(view.isKeyUnsubscribed(u"k2"_s));

  view.setUnsubscribedKeys({u"k1"_s});
  EXPECT_TRUE(view.isKeyUnsubscribed(u"k1"_s));
  EXPECT_FALSE(view.isKeyUnsubscribed(u"k2"_s));

  // Full-set replace: re-subscribing k1 and unsubscribing k2 in one call
  // flips both, not just adds k2.
  view.setUnsubscribedKeys({u"k2"_s});
  EXPECT_FALSE(view.isKeyUnsubscribed(u"k1"_s));
  EXPECT_TRUE(view.isKeyUnsubscribed(u"k2"_s));

  EXPECT_FALSE(view.isKeyUnsubscribed(u"no-such-key"_s));
}

TEST(CurveTreeViewTest, IsUnsubscribedSeedsFromCurvePathAtConstruction) {
  PJ::CurveTreeView view;
  view.addCatalogItem(
      PJ::CurveTreeView::CurvePath{
          .key = u"k1"_s,
          .dataset = u"robot"_s,
          .topic = u"imu"_s,
          .field = u"x"_s,
          .is_unsubscribed = true,
      });
  EXPECT_TRUE(view.isKeyUnsubscribed(u"k1"_s));
}

namespace {
// First direct child of `parent` whose Name-column text is `text`, or nullptr.
QTreeWidgetItem* peekChildByText(QTreeWidgetItem* parent, const QString& text) {
  for (int i = 0; i < parent->childCount(); ++i) {
    if (parent->child(i)->text(0) == text) {
      return parent->child(i);
    }
  }
  return nullptr;
}
}  // namespace

TEST(CurveTreeViewTest, DoubleClickEmitsPeekOnlyForScalarPlaceholderLeaf) {
  PJ::CurveTreeView view;  // default hierarchical view
  // Scalar-shaped placeholder: a draggable leaf with no field breakdown yet.
  view.addCatalogItem(
      PJ::CurveTreeView::CurvePath{
          .key = u"scalar-key"_s,
          .dataset = u"robot"_s,
          .topic = u"/speed"_s,
          .field = QString(),
          .selectable = true,
          .is_placeholder = true,
      });
  // Object-shaped placeholder: a non-selectable 3D-object terminal (also a
  // childless node, but NOT peek-eligible).
  view.addCatalogItem(
      PJ::CurveTreeView::CurvePath{
          .key = u"object-key"_s,
          .dataset = u"robot"_s,
          .topic = u"/points"_s,
          .field = QString(),
          .selectable = false,
          .is_3d_object_topic = true,
          .is_placeholder = true,
      });
  // Real (subscribed) scalar leaf: not a placeholder.
  view.addCatalogItem(
      PJ::CurveTreeView::CurvePath{
          .key = u"real-key"_s,
          .dataset = u"robot"_s,
          .topic = u"/temp"_s,
          .field = QString(),
          .selectable = true,
          .is_placeholder = false,
      });

  QStringList captured;
  QObject::connect(
      &view, &PJ::CurveTreeView::placeholderPeekRequested, [&](const QString& key) { captured.push_back(key); });

  QTreeWidgetItem* root = view.topLevelItem(0);
  ASSERT_NE(root, nullptr);
  QTreeWidgetItem* scalar_leaf = peekChildByText(root, u"speed"_s);
  QTreeWidgetItem* object_terminal = peekChildByText(root, u"points"_s);
  QTreeWidgetItem* real_leaf = peekChildByText(root, u"temp"_s);
  ASSERT_NE(scalar_leaf, nullptr);
  ASSERT_NE(object_terminal, nullptr);
  ASSERT_NE(real_leaf, nullptr);

  Q_EMIT view.itemDoubleClicked(object_terminal, 0);
  Q_EMIT view.itemDoubleClicked(real_leaf, 0);
  EXPECT_TRUE(captured.isEmpty()) << "object placeholder and real leaf must not emit a peek";

  Q_EMIT view.itemDoubleClicked(scalar_leaf, 0);
  EXPECT_EQ(captured, QStringList{u"scalar-key"_s});
}

TEST(CurveTreeViewTest, RequestExpansionWhenPromotedFiresOnceThenRespectsManualCollapse) {
  PJ::CurveTreeView view;  // default hierarchical view

  // Arm the intent for the topic's tree-path while it is still a placeholder leaf.
  view.requestExpansionWhenPromoted(u"robot/imu/data"_s);

  // Promotion: the placeholder leaf is replaced by real field leaves under the
  // topic, so "robot/imu/data" becomes a group node.
  const auto promote = [&view]() {
    view.clearCurves();
    view.addCatalogItems(
        {PJ::CurveTreeView::CurvePath{
             .key = u"f1"_s,
             .dataset = u"robot"_s,
             .topic = u"/imu/data"_s,
             .field = u"angular_velocity.z"_s,
         },
         PJ::CurveTreeView::CurvePath{
             .key = u"f2"_s,
             .dataset = u"robot"_s,
             .topic = u"/imu/data"_s,
             .field = u"orientation.w"_s,
         }});
  };

  promote();
  QTreeWidgetItem* imu = peekChildByText(view.topLevelItem(0), u"imu"_s);
  ASSERT_NE(imu, nullptr);
  QTreeWidgetItem* data_group = peekChildByText(imu, u"data"_s);
  ASSERT_NE(data_group, nullptr);
  EXPECT_TRUE(data_group->isExpanded()) << "the promoted topic group auto-expands once";

  // One-shot: a manual collapse must survive the next rebuild (the intent was
  // already consumed).
  data_group->setExpanded(false);
  promote();
  QTreeWidgetItem* imu2 = peekChildByText(view.topLevelItem(0), u"imu"_s);
  ASSERT_NE(imu2, nullptr);
  QTreeWidgetItem* data_group2 = peekChildByText(imu2, u"data"_s);
  ASSERT_NE(data_group2, nullptr);
  EXPECT_FALSE(data_group2->isExpanded()) << "auto-expand must not re-fire after the one-shot intent is consumed";
}

TEST(CurveTreeViewTest, RequestExpansionWhenPromotedFiresInShowTopicsView) {
  PJ::CurveTreeView view;
  view.setViewMode(PJ::CurveTreeView::ViewMode::kShowTopics);  // the app's default view
  view.requestExpansionWhenPromoted(u"robot/imu/data"_s);

  view.addCatalogItems(
      {PJ::CurveTreeView::CurvePath{
           .key = u"f1"_s,
           .dataset = u"robot"_s,
           .topic = u"/imu/data"_s,
           .field = u"angular_velocity.z"_s,
       },
       PJ::CurveTreeView::CurvePath{
           .key = u"f2"_s,
           .dataset = u"robot"_s,
           .topic = u"/imu/data"_s,
           .field = u"orientation.w"_s,
       }});

  // In show-topics view the topic is a single verbatim node "/imu/data" whose
  // text diverges from the normalized search path — the search-role keying still
  // resolves and expands it.
  QTreeWidgetItem* root = view.topLevelItem(0);
  ASSERT_NE(root, nullptr);
  QTreeWidgetItem* topic = peekChildByText(root, u"/imu/data"_s);
  ASSERT_NE(topic, nullptr);
  EXPECT_TRUE(topic->isExpanded());
}

TEST(CurveTreeViewTest, CatalogKeysUnderCollectsSelfAndDescendants) {
  PJ::CurveTreeView view;
  view.setViewMode(PJ::CurveTreeView::ViewMode::kShowTopics);
  view.addCatalogItems({
      PJ::CurveTreeView::CurvePath{
          .key = u"k_x"_s,
          .dataset = u"robot"_s,
          .topic = u"/imu/data"_s,
          .field = u"x"_s,
      },
      PJ::CurveTreeView::CurvePath{
          .key = u"k_y"_s,
          .dataset = u"robot"_s,
          .topic = u"/imu/data"_s,
          .field = u"y"_s,
      },
  });

  QTreeWidgetItem* robot = view.topLevelItem(0);
  ASSERT_NE(robot, nullptr);
  QTreeWidgetItem* topic = findChild(robot, u"/imu/data"_s);
  ASSERT_NE(topic, nullptr);
  ASSERT_TRUE(PJ::CurveTreeView::catalogKeyOf(topic).isEmpty()) << "a promoted topic group carries no key itself";

  QStringList keys = PJ::CurveTreeView::catalogKeysUnder(topic);
  keys.sort();
  EXPECT_EQ(keys, (QStringList{u"k_x"_s, u"k_y"_s}));

  // A keyed leaf reports just itself.
  QTreeWidgetItem* leaf_x = findChild(topic, u"x"_s);
  ASSERT_NE(leaf_x, nullptr);
  EXPECT_EQ(PJ::CurveTreeView::catalogKeysUnder(leaf_x), (QStringList{u"k_x"_s}));
}

TEST(CurveTreeViewTest, ForcedTopicMarksLandOnTheTopicNodeAndSurviveRebuild) {
  PJ::CurveTreeView view;
  view.setViewMode(PJ::CurveTreeView::ViewMode::kShowTopics);
  const auto add_all = [&view]() {
    view.addCatalogItems({
        PJ::CurveTreeView::CurvePath{
            .key = u"k_x"_s,
            .dataset = u"robot"_s,
            .topic = u"/imu/data"_s,
            .field = u"x"_s,
        },
        PJ::CurveTreeView::CurvePath{
            .key = u"k_pc"_s,
            .dataset = u"robot"_s,
            .topic = u"/points"_s,
            .field = {},
            .selectable = false,  // object-topic terminal — IS the topic row
        },
    });
  };
  add_all();

  const QString imu_path = PJ::CurveTreeView::treePathFromCurvePath(
      PJ::CurveTreeView::CurvePath{.key = {}, .dataset = u"robot"_s, .topic = u"/imu/data"_s, .field = {}});
  const QString pc_path = PJ::CurveTreeView::treePathFromCurvePath(
      PJ::CurveTreeView::CurvePath{.key = {}, .dataset = u"robot"_s, .topic = u"/points"_s, .field = {}});

  view.setForcedTopicPaths({imu_path, pc_path});
  EXPECT_TRUE(view.isTopicPathForced(imu_path)) << "promoted scalar topic: mark on the keyless GROUP node";
  EXPECT_TRUE(view.isTopicPathForced(pc_path)) << "object terminal: mark on the topic row itself";

  // The set is retained: a rebuild (clear + re-add) re-stamps the marks.
  view.clearCurves();
  add_all();
  EXPECT_TRUE(view.isTopicPathForced(imu_path));

  // Full-set replace: unforcing clears the mark.
  view.setForcedTopicPaths({});
  EXPECT_FALSE(view.isTopicPathForced(imu_path));
  EXPECT_FALSE(view.isTopicPathForced(pc_path));
}

TEST(CurveTreeViewTest, DatasetProgressSurvivesClearCurvesAndReAdd) {
  PJ::CurveTreeView view;
  const PJ::CurveTreeView::CurvePath path{
      .key = u"drive/imu/x"_s,
      .dataset = u"drive.mcap"_s,
      .topic = u"/imu/data"_s,
      .field = u"acceleration.x"_s,
  };
  constexpr quint64 kRowKey = 41;
  view.setDatasetRowKey(PJ::CurveTreeView::treePathFromCurvePath(path), kRowKey);
  const auto add_all = [&view, &path]() { view.addCatalogItems({path}); };
  add_all();

  const PJ::CurveTreeView::DatasetProgress expected{
      .display_name = u"drive.mcap"_s,
      .fraction = 0.375,
      .cancellable = true,
      .state = PJ::CurveTreeView::DatasetProgress::State::kLoading,
      .flash_on = false,
  };
  view.setDatasetProgress(progressSet(kRowKey, expected));

  ASSERT_EQ(view.topLevelItemCount(), 1);
  expectDatasetProgress(view.topLevelItem(0), expected);

  // Retained progress becomes a ghost while the tree is empty, then moves back
  // to the real dataset row after the rebuild. Matching names must not make the
  // insertion reuse (and later delete) the ghost.
  view.clearCurves();
  ASSERT_EQ(view.topLevelItemCount(), 1);
  EXPECT_TRUE(view.topLevelItem(0)->data(0, kDatasetGhostRoleForTest).toBool());
  expectDatasetProgress(view.topLevelItem(0), expected);

  add_all();
  ASSERT_EQ(view.topLevelItemCount(), 1);
  QTreeWidgetItem* dataset = view.topLevelItem(0);
  EXPECT_FALSE(dataset->data(0, kDatasetGhostRoleForTest).toBool());
  EXPECT_FALSE(PJ::CurveTreeView::catalogKeysUnder(dataset).isEmpty());
  expectDatasetProgress(dataset, expected);
}

TEST(CurveTreeViewTest, EmptyDatasetProgressClearsAllProgress) {
  PJ::CurveTreeView view;
  const PJ::CurveTreeView::CurvePath path{
      .key = u"robot/imu/x"_s,
      .dataset = u"robot"_s,
      .topic = u"imu"_s,
      .field = u"x"_s,
  };
  constexpr quint64 kRowKey = 42;
  view.setDatasetRowKey(PJ::CurveTreeView::treePathFromCurvePath(path), kRowKey);
  view.addCatalogItem(path);
  view.setDatasetProgress(progressSet(
      kRowKey, PJ::CurveTreeView::DatasetProgress{
                   .display_name = u"robot"_s,
                   .fraction = 0.5,
               }));

  ASSERT_EQ(view.topLevelItemCount(), 1);
  QTreeWidgetItem* dataset = view.topLevelItem(0);
  ASSERT_TRUE(dataset->data(0, kDatasetProgressRoleForTest).isValid());

  view.setDatasetProgress({});

  EXPECT_EQ(view.topLevelItemCount(), 1);
  EXPECT_FALSE(dataset->data(0, kDatasetProgressRoleForTest).isValid());
}

TEST(CurveTreeViewTest, DatasetProgressNeverMarksScalarLeaves) {
  PJ::CurveTreeView view;
  const PJ::CurveTreeView::CurvePath path{
      .key = u"robot/imu/x"_s,
      .dataset = u"robot"_s,
      .topic = u"imu"_s,
      .field = u"x"_s,
  };
  constexpr quint64 kRowKey = 43;
  view.setDatasetRowKey(PJ::CurveTreeView::treePathFromCurvePath(path), kRowKey);
  view.setDatasetProgress(progressSet(
      kRowKey, PJ::CurveTreeView::DatasetProgress{
                   .display_name = u"robot"_s,
                   .indeterminate = true,
               }));
  view.addCatalogItem(path);

  ASSERT_EQ(view.topLevelItemCount(), 1);
  QTreeWidgetItem* dataset = view.topLevelItem(0);
  QTreeWidgetItem* topic = findChild(dataset, u"imu"_s);
  ASSERT_NE(topic, nullptr);
  QTreeWidgetItem* leaf = findChild(topic, u"x"_s);
  ASSERT_NE(leaf, nullptr);

  EXPECT_TRUE(dataset->data(0, kDatasetProgressRoleForTest).isValid());
  EXPECT_FALSE(topic->data(0, kDatasetProgressRoleForTest).isValid());
  EXPECT_FALSE(leaf->data(0, kDatasetProgressRoleForTest).isValid());
}

TEST(CurveTreeViewTest, DatasetProgressCreatesAndRemovesGhost) {
  PJ::CurveTreeView view;
  constexpr quint64 kRowKey = 44;
  view.setDatasetRowKey(u"missing/topic/value"_s, kRowKey);
  const PJ::CurveTreeView::DatasetProgress expected{
      .display_name = u"Missing dataset"_s,
      .fraction = 1.0,
      .state = PJ::CurveTreeView::DatasetProgress::State::kFailed,
      .flash_on = false,
  };

  view.setDatasetProgress(progressSet(kRowKey, expected));

  ASSERT_EQ(view.topLevelItemCount(), 1);
  QTreeWidgetItem* ghost = view.topLevelItem(0);
  EXPECT_EQ(ghost->text(0), expected.display_name);
  EXPECT_EQ(ghost->childCount(), 0);
  EXPECT_TRUE(ghost->data(0, kDatasetGhostRoleForTest).toBool());
  EXPECT_FALSE(ghost->flags().testFlag(Qt::ItemIsSelectable));
  EXPECT_FALSE(ghost->flags().testFlag(Qt::ItemIsDragEnabled));
  EXPECT_TRUE(PJ::CurveTreeView::catalogKeyOf(ghost).isEmpty());
  expectDatasetProgress(ghost, expected);

  view.setDatasetProgress({});

  EXPECT_EQ(view.topLevelItemCount(), 0);
}

// A row showing load progress must survive the filter. It reports work in
// flight and carries the only affordances that can stop it, so filtering it away
// takes the stop button with it. A ghost matches no filter text at all.
TEST(CurveTreeViewTest, ProgressRowsSurviveAFilterThatMatchesNothing) {
  PJ::CurveTreeView view;
  view.addCurves({u"veh/imu/x"_s});
  constexpr quint64 kGhostKey = 71;
  constexpr quint64 kRealKey = 72;
  view.setDatasetRowKey(u"veh"_s, kRealKey);

  QHash<quint64, PJ::CurveTreeView::DatasetProgress> progress;
  progress[kRealKey] = PJ::CurveTreeView::DatasetProgress{
      .display_name = u"veh"_s,
      .fraction = 0.5,
      .cancellable = true,
      .state = PJ::CurveTreeView::DatasetProgress::State::kLoading,
      .flash_on = false,
  };
  progress[kGhostKey] = PJ::CurveTreeView::DatasetProgress{
      .display_name = u"Importing MCAP"_s,
      .fraction = 0.25,
      .cancellable = true,
      .state = PJ::CurveTreeView::DatasetProgress::State::kLoading,
      .flash_on = false,
  };
  view.setDatasetProgress(progress);

  view.applyFilter(u"zzz-matches-nothing"_s);

  QTreeWidgetItem* real_row = findTopLevel(view, u"veh"_s);
  QTreeWidgetItem* ghost_row = findTopLevel(view, u"Importing MCAP"_s);
  ASSERT_NE(real_row, nullptr);
  ASSERT_NE(ghost_row, nullptr);
  EXPECT_FALSE(real_row->isHidden()) << "a loading dataset row must not be filtered away";
  EXPECT_FALSE(ghost_row->isHidden()) << "a ghost row matches no text and would always vanish";

  // The exemption lasts exactly as long as the decoration does.
  view.setDatasetProgress({});
  QTreeWidgetItem* after = findTopLevel(view, u"veh"_s);
  ASSERT_NE(after, nullptr);
  EXPECT_TRUE(after->isHidden()) << "once the progress is gone the filter applies normally";
}

// A ghost's label is its display name, and that name changes under a stable row
// key when the row adopts its dataset's name. The fraction-only fast path must
// repaint the text AND re-sort, or the row keeps the producer's title — in the
// position that title sorted to — until the row set happens to change.
TEST(CurveTreeViewTest, GhostLabelAndOrderFollowItsDisplayNameOnAFractionOnlyTick) {
  PJ::CurveTreeView view;
  constexpr quint64 kRenamedKey = 73;
  constexpr quint64 kSiblingKey = 74;
  PJ::CurveTreeView::DatasetProgress renamed{
      .display_name = u"b-importing"_s,
      .fraction = 0.1,
      .state = PJ::CurveTreeView::DatasetProgress::State::kLoading,
      .flash_on = false,
  };
  PJ::CurveTreeView::DatasetProgress sibling = renamed;
  sibling.display_name = u"c-importing"_s;

  QHash<quint64, PJ::CurveTreeView::DatasetProgress> progress;
  progress[kRenamedKey] = renamed;
  progress[kSiblingKey] = sibling;
  view.setDatasetProgress(progress);
  ASSERT_EQ(topLevelNames(view), (std::vector<std::string>{"b-importing", "c-importing"}));

  // Same key set: this is the fast path, not a rebuild.
  renamed.display_name = u"d-recording.mcap"_s;
  renamed.fraction = 0.6;
  progress[kRenamedKey] = renamed;
  view.setDatasetProgress(progress);

  EXPECT_EQ(topLevelNames(view), (std::vector<std::string>{"c-importing", "d-recording.mcap"}))
      << "the ghost kept a stale label or a stale sort position";
}

// progress_row_cache_ holds raw item pointers. QTreeWidget::clear() is inherited,
// non-virtual and public, so any caller can delete those items without the view
// hearing about it — and the next same-membership tick is the pass that trusts
// the cache without re-resolving.
TEST(CurveTreeViewTest, InheritedClearCannotLeaveTheProgressCacheDangling) {
  PJ::CurveTreeView view;
  view.addCurves({u"veh/imu/x"_s});
  constexpr quint64 kRowKey = 75;
  view.setDatasetRowKey(u"veh"_s, kRowKey);
  PJ::CurveTreeView::DatasetProgress progress{
      .display_name = u"veh"_s,
      .fraction = 0.3,
      .cancellable = true,
      .state = PJ::CurveTreeView::DatasetProgress::State::kLoading,
      .flash_on = false,
  };
  view.setDatasetProgress(progressSet(kRowKey, progress));
  ASSERT_NE(findTopLevel(view, u"veh"_s), nullptr);

  static_cast<QTreeWidget&>(view).clear();  // deletes the very row the cache points at

  progress.fraction = 0.9;  // unchanged membership: the fast path
  view.setDatasetProgress(progressSet(kRowKey, progress));

  QTreeWidgetItem* row = findTopLevel(view, u"veh"_s);
  ASSERT_NE(row, nullptr) << "the cleared row must come back as a ghost";
  EXPECT_TRUE(row->data(0, kDatasetGhostRoleForTest).toBool());
  expectDatasetProgress(row, progress);
}

// The path->key map is only useful while its key names a live ingest. Pruning it
// on retirement is what keeps it from growing for the whole session — but a
// mapping registered ahead of its key's first publication must survive, because
// that ordering is the normal one (the path resolves, then progress is emitted).
TEST(CurveTreeViewTest, RetiredRowKeysDropTheirTreePathMappings) {
  PJ::CurveTreeView view;
  view.addCurves({u"veh/imu/x"_s});
  constexpr quint64 kFirstKey = 81;
  constexpr quint64 kSecondKey = 82;
  constexpr quint64 kUnpublishedKey = 83;
  const PJ::CurveTreeView::DatasetProgress progress{
      .display_name = u"veh"_s,
      .fraction = 0.5,
      .cancellable = true,
      .state = PJ::CurveTreeView::DatasetProgress::State::kLoading,
      .flash_on = false,
  };

  view.setDatasetRowKey(u"veh"_s, kFirstKey);
  view.setDatasetProgress(progressSet(kFirstKey, progress));
  ASSERT_EQ(PJ::CurveTreeViewTestPeer::datasetPathMappingCount(view), 1);

  view.setDatasetProgress({});  // the load ended
  EXPECT_EQ(PJ::CurveTreeViewTestPeer::datasetPathMappingCount(view), 0) << "a retired key kept its path mapping";

  // A reload of the same dataset files a fresh key under the same path, and a
  // mapping for a key nobody has published yet must not be pruned by someone
  // else's membership change.
  view.setDatasetRowKey(u"veh"_s, kSecondKey);
  view.setDatasetRowKey(u"other"_s, kUnpublishedKey);
  view.setDatasetProgress(progressSet(kSecondKey, progress));
  EXPECT_EQ(PJ::CurveTreeViewTestPeer::datasetPathMappingCount(view), 2)
      << "a mapping whose key has not been published yet must survive someone else's membership change";
}

// A key has ONE live path. A dataset's tree path changes under a stable key (a
// sibling's removal relabels "foo (2)" back to "foo" and cascades), and the full
// pass resolves a key through whichever of its mapped paths it meets first in an
// unordered hash — so a path left behind can decorate another dataset's row and
// hand that row's stop click to this ingest.
TEST(CurveTreeViewTest, RemappingARowKeyRetiresItsPreviousPath) {
  TestCurveTreeView view;
  view.resize(360, 180);
  view.addCurves({u"alpha/imu/x"_s, u"beta/imu/x"_s});
  constexpr quint64 kRowKey = 91;
  const PJ::CurveTreeView::DatasetProgress progress{
      .display_name = u"beta"_s,
      .fraction = 0.5,
      .cancellable = true,
      .state = PJ::CurveTreeView::DatasetProgress::State::kLoading,
      .flash_on = false,
  };

  view.setDatasetRowKey(u"alpha"_s, kRowKey);
  view.setDatasetProgress(progressSet(kRowKey, progress));
  ASSERT_NE(findTopLevel(view, u"alpha"_s), nullptr);

  view.setDatasetRowKey(u"beta"_s, kRowKey);  // the row this key names moved
  view.show();
  QApplication::processEvents();

  EXPECT_EQ(PJ::CurveTreeViewTestPeer::datasetPathMappingCount(view), 1) << "the key kept its retired path";

  QTreeWidgetItem* alpha = findTopLevel(view, u"alpha"_s);
  QTreeWidgetItem* beta = findTopLevel(view, u"beta"_s);
  ASSERT_NE(alpha, nullptr);
  ASSERT_NE(beta, nullptr);
  EXPECT_FALSE(alpha->data(0, kDatasetProgressRoleForTest).isValid()) << "the decoration stayed on the retired path";
  EXPECT_FALSE(alpha->data(0, kDatasetRowKeyRoleForTest).isValid());
  EXPECT_TRUE(beta->data(0, kDatasetProgressRoleForTest).isValid());
  EXPECT_EQ(beta->data(0, kDatasetRowKeyRoleForTest).value<quint64>(), kRowKey);

  // The click follows the decoration: the stop on the row this key now names
  // reports that key, and the row it left behind offers nothing to click.
  std::vector<std::pair<quint64, bool>> requests;
  QObject::connect(&view, &PJ::CurveTreeView::cancelRequested, &view, [&requests](quint64 row_key, bool keep_partial) {
    requests.emplace_back(row_key, keep_partial);
  });
  const QPoint stop_pos = cancelButtonPosition(view, beta);
  ASSERT_TRUE(view.viewport()->rect().contains(stop_pos));
  EXPECT_TRUE(sendMousePress(view, stop_pos));
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0], std::make_pair(kRowKey, true));

  // The same gesture on the row the key left behind stops nothing: that row
  // carries no progress, so the position holds no affordance at all.
  const QPoint retired_pos = cancelButtonPosition(view, alpha);
  ASSERT_TRUE(view.viewport()->rect().contains(retired_pos));
  sendMousePress(view, retired_pos);
  EXPECT_EQ(requests.size(), 1u) << "a stop click landed on a row that shows no load";
}

// Two live keys can name one dataset path: a reload's ingest resolves to the
// dataset while the finished one is still lingering on it. A row carries ONE
// decoration and routes its stop to ONE ingest, so the LAST key registered for a
// path owns the real row and the key it displaces falls back to a ghost —
// deterministic, where sharing the row would leave the winner to hash order.
TEST(CurveTreeViewTest, TheLatestKeyForAPathOwnsTheRowAndDisplacesTheOlderToAGhost) {
  PJ::CurveTreeView view;
  view.addCurves({u"foo/imu/x"_s});
  constexpr quint64 kFinishingKey = 95;
  constexpr quint64 kReloadKey = 96;

  QHash<quint64, PJ::CurveTreeView::DatasetProgress> progress;
  progress[kFinishingKey] = PJ::CurveTreeView::DatasetProgress{
      .display_name = u"foo (finishing)"_s,
      .fraction = 1.0,
      .state = PJ::CurveTreeView::DatasetProgress::State::kCompleted,
      .flash_on = false,
  };
  view.setDatasetRowKey(u"foo"_s, kFinishingKey);
  view.setDatasetProgress(progress);
  ASSERT_EQ(findTopLevel(view, u"foo"_s)->data(0, kDatasetRowKeyRoleForTest).value<quint64>(), kFinishingKey);

  // The reload's ingest resolves to the same dataset row.
  progress[kReloadKey] = PJ::CurveTreeView::DatasetProgress{
      .display_name = u"foo"_s,
      .fraction = 0.2,
      .cancellable = true,
      .state = PJ::CurveTreeView::DatasetProgress::State::kLoading,
      .flash_on = false,
  };
  view.setDatasetRowKey(u"foo"_s, kReloadKey);
  view.setDatasetProgress(progress);

  QTreeWidgetItem* real_row = findTopLevel(view, u"foo"_s);
  ASSERT_NE(real_row, nullptr);
  EXPECT_FALSE(real_row->data(0, kDatasetGhostRoleForTest).toBool());
  EXPECT_EQ(real_row->data(0, kDatasetRowKeyRoleForTest).value<quint64>(), kReloadKey)
      << "the newcomer must own the row, and its stop click with it";

  QTreeWidgetItem* ghost = findTopLevel(view, u"foo (finishing)"_s);
  ASSERT_NE(ghost, nullptr) << "the displaced key must keep a row of its own";
  EXPECT_TRUE(ghost->data(0, kDatasetGhostRoleForTest).toBool());
  EXPECT_EQ(ghost->data(0, kDatasetRowKeyRoleForTest).value<quint64>(), kFinishingKey);
  EXPECT_EQ(PJ::CurveTreeViewTestPeer::datasetPathMappingCount(view), 1) << "a path names exactly one live key";
}

// The two affordances must be distinguishable by position alone: the row IS the
// keep-or-discard choice, so a host that got the wrong flag would silently
// discard data the user asked to keep.
TEST(CurveTreeViewTest, KeepAndDiscardAffordancesReportOppositeKeepPartial) {
  TestCurveTreeView view;
  view.resize(360, 180);
  view.addCurves({u"loading"_s});
  constexpr quint64 kRowKey = 77;
  view.setDatasetRowKey(u"loading"_s, kRowKey);
  view.setDatasetProgress(progressSet(
      kRowKey, PJ::CurveTreeView::DatasetProgress{
                   .display_name = u"loading"_s,
                   .fraction = 0.4,
                   .cancellable = true,
                   .state = PJ::CurveTreeView::DatasetProgress::State::kLoading,
                   .flash_on = false,
               }));
  view.show();
  QApplication::processEvents();

  QTreeWidgetItem* loading = findTopLevel(view, u"loading"_s);
  ASSERT_NE(loading, nullptr);

  std::vector<std::pair<quint64, bool>> requests;
  QObject::connect(&view, &PJ::CurveTreeView::cancelRequested, &view, [&requests](quint64 row_key, bool keep_partial) {
    requests.emplace_back(row_key, keep_partial);
  });

  // The two rects must not overlap, or a click could not name one of them.
  const QRect keep_rect = PJ::CurveTreeViewTestPeer::cancelButtonRect(view, loading);
  const QRect discard_rect = PJ::CurveTreeViewTestPeer::discardButtonRect(view, loading);
  ASSERT_FALSE(keep_rect.isEmpty());
  ASSERT_FALSE(discard_rect.isEmpty());
  EXPECT_FALSE(keep_rect.intersects(discard_rect));
  // Both must sit inside the row: a glyph taller than the row spills over its
  // neighbours and cannot be hit reliably.
  const QRect row_rect = view.visualItemRect(loading);
  EXPECT_LE(keep_rect.height(), row_rect.height());
  EXPECT_LE(discard_rect.height(), row_rect.height());

  const QPoint keep_pos = cancelButtonPosition(view, loading);
  ASSERT_TRUE(view.viewport()->rect().contains(keep_pos));
  EXPECT_TRUE(sendMousePress(view, keep_pos));

  const QPoint discard_pos = discardButtonPosition(view, loading);
  ASSERT_TRUE(view.viewport()->rect().contains(discard_pos));
  EXPECT_TRUE(sendMousePress(view, discard_pos));

  ASSERT_EQ(requests.size(), 2u);
  EXPECT_EQ(requests[0], std::make_pair(kRowKey, true));   // ✕ keeps
  EXPECT_EQ(requests[1], std::make_pair(kRowKey, false));  // bin discards
}

// Hover must be driven by pointer movement, not sampled from the global cursor
// when the row happens to repaint for another reason — that made the highlight
// wait for the next progress tick, which reads as lag, and never arrive at all
// on a stalled row.
TEST(CurveTreeViewTest, StopButtonHoverTracksPointerMovement) {
  TestCurveTreeView view;
  view.resize(360, 180);
  view.addCurves({u"loading"_s});
  constexpr quint64 kRowKey = 63;
  view.setDatasetRowKey(u"loading"_s, kRowKey);
  view.setDatasetProgress(progressSet(
      kRowKey, PJ::CurveTreeView::DatasetProgress{
                   .display_name = u"loading"_s,
                   .fraction = 0.4,
                   .cancellable = true,
                   .state = PJ::CurveTreeView::DatasetProgress::State::kLoading,
                   .flash_on = false,
               }));
  view.show();
  QApplication::processEvents();

  QTreeWidgetItem* loading = findTopLevel(view, u"loading"_s);
  ASSERT_NE(loading, nullptr);
  EXPECT_EQ(PJ::CurveTreeViewTestPeer::hoveredStopButton(view), 0);

  sendHoverMove(view, cancelButtonPosition(view, loading));
  EXPECT_EQ(PJ::CurveTreeViewTestPeer::hoveredStopButton(view), 1);

  sendHoverMove(view, discardButtonPosition(view, loading));
  EXPECT_EQ(PJ::CurveTreeViewTestPeer::hoveredStopButton(view), 2);

  // Off the affordances entirely: the highlight must not stick.
  sendHoverMove(view, QPoint(4, view.visualItemRect(loading).center().y()));
  EXPECT_EQ(PJ::CurveTreeViewTestPeer::hoveredStopButton(view), 0);
}

// A stop is acknowledged the moment it is accepted, not when the producer
// finishes — a cooperative stop can take seconds. The row must then be inert:
// clicking again would ask a second time for something already in flight.
TEST(CurveTreeViewTest, StoppingRowIsInertForBothAffordances) {
  TestCurveTreeView view;
  view.resize(360, 180);
  view.addCurves({u"stopping"_s});
  constexpr quint64 kRowKey = 97;
  view.setDatasetRowKey(u"stopping"_s, kRowKey);
  view.setDatasetProgress(progressSet(
      kRowKey, PJ::CurveTreeView::DatasetProgress{
                   .display_name = u"stopping"_s,
                   .fraction = 0.27,
                   .cancellable = true,
                   .state = PJ::CurveTreeView::DatasetProgress::State::kStopping,
                   .flash_on = false,
               }));
  view.show();
  QApplication::processEvents();

  QTreeWidgetItem* row = findTopLevel(view, u"stopping"_s);
  ASSERT_NE(row, nullptr);

  int requests = 0;
  QObject::connect(&view, &PJ::CurveTreeView::cancelRequested, &view, [&requests](quint64, bool) { ++requests; });

  // Both cells stay in place, so the cluster does not change shape mid-stop.
  EXPECT_FALSE(PJ::CurveTreeViewTestPeer::cancelButtonRect(view, row).isEmpty());
  EXPECT_FALSE(PJ::CurveTreeViewTestPeer::discardButtonRect(view, row).isEmpty());

  sendHoverMove(view, cancelButtonPosition(view, row));
  EXPECT_EQ(PJ::CurveTreeViewTestPeer::hoveredStopButton(view), 0);
  sendMousePress(view, cancelButtonPosition(view, row));
  sendMousePress(view, discardButtonPosition(view, row));
  EXPECT_EQ(requests, 0);
}

// The indeterminate band is phased from when the animation STARTED, so a load
// always opens with the band entering from the left. Phasing it off absolute
// wall-clock time instead made the sweep begin wherever the clock happened to
// land — a fresh load could show the band already near the right edge.
TEST(CurveTreeViewTest, MarqueeBandEntersFromTheLeftAtStart) {
  const QRect track(0, 0, 400, 20);
  constexpr qint64 kPeriod = 1200;

  // At the very start the band is only just entering: flush to the left edge and
  // barely visible, never already mid-track.
  const QRect at_start = PJ::CurveTreeView::marqueeBandRect(track, 0, kPeriod);
  EXPECT_EQ(at_start.left(), track.left()) << "the sweep must begin AT the left edge, not mid-track";
  EXPECT_LE(at_start.width(), 2) << "the band must enter, not appear already grown";

  // Shortly after, it has entered from the left and is anchored there.
  const QRect early = PJ::CurveTreeView::marqueeBandRect(track, kPeriod / 10, kPeriod);
  ASSERT_FALSE(early.isEmpty());
  EXPECT_EQ(early.left(), track.left()) << "the entering band must be flush to the left edge";
  EXPECT_LT(early.width(), track.width() / 2);

  // Mid-cycle it sits mid-track, and later it is further right: a real sweep.
  const QRect middle = PJ::CurveTreeView::marqueeBandRect(track, kPeriod / 2, kPeriod);
  const QRect late = PJ::CurveTreeView::marqueeBandRect(track, (kPeriod * 4) / 5, kPeriod);
  ASSERT_FALSE(middle.isEmpty());
  ASSERT_FALSE(late.isEmpty());
  EXPECT_GT(middle.left(), track.left());
  EXPECT_GT(late.left(), middle.left());

  // The cycle repeats: one full period later is the same position again.
  EXPECT_EQ(PJ::CurveTreeView::marqueeBandRect(track, kPeriod / 2 + kPeriod, kPeriod), middle);

  // It never escapes the track.
  for (qint64 t = 0; t < kPeriod; t += 37) {
    const QRect band = PJ::CurveTreeView::marqueeBandRect(track, t, kPeriod);
    EXPECT_TRUE(band.isEmpty() || track.contains(band)) << "band escaped the track at t=" << t;
  }
}

// A producer that can stop but not discard (the toolbox ingest ABI has no
// rollback) still gets the bin DRAWN — losing a cell mid-load reads as a glitch
// — but it must be inert, or the click would promise a discard the producer
// would silently turn into a keep.
TEST(CurveTreeViewTest, NonDiscardableRowKeepsTheBinButIgnoresIt) {
  TestCurveTreeView view;
  view.resize(360, 180);
  view.addCurves({u"stoponly"_s});
  constexpr quint64 kRowKey = 84;
  view.setDatasetRowKey(u"stoponly"_s, kRowKey);
  view.setDatasetProgress(progressSet(
      kRowKey, PJ::CurveTreeView::DatasetProgress{
                   .display_name = u"stoponly"_s,
                   .fraction = 0.3,
                   .cancellable = true,
                   .discardable = false,
                   .state = PJ::CurveTreeView::DatasetProgress::State::kLoading,
                   .flash_on = false,
               }));
  view.show();
  QApplication::processEvents();

  QTreeWidgetItem* row = findTopLevel(view, u"stoponly"_s);
  ASSERT_NE(row, nullptr);

  std::vector<std::pair<quint64, bool>> requests;
  QObject::connect(&view, &PJ::CurveTreeView::cancelRequested, &view, [&requests](quint64 row_key, bool keep_partial) {
    requests.emplace_back(row_key, keep_partial);
  });

  // The bin still occupies its cell, so the cluster keeps its shape.
  EXPECT_FALSE(PJ::CurveTreeViewTestPeer::discardButtonRect(view, row).isEmpty());

  // ...but neither hover nor click reaches it.
  sendHoverMove(view, discardButtonPosition(view, row));
  EXPECT_EQ(PJ::CurveTreeViewTestPeer::hoveredStopButton(view), 0);
  sendMousePress(view, discardButtonPosition(view, row));
  EXPECT_TRUE(requests.empty());

  // Stopping-and-keeping is still offered, which is the whole point.
  sendMousePress(view, cancelButtonPosition(view, row));
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0], std::make_pair(kRowKey, true));
}

// The affordances stay painted through the terminal linger so the cluster does
// not lose two of its three cells as the row retires — but they are inert
// there: the ingest has already ended, so a click must not reach the host.
TEST(CurveTreeViewTest, TerminalRowAffordancesAreInert) {
  TestCurveTreeView view;
  view.resize(360, 180);
  view.addCurves({u"done"_s});
  constexpr quint64 kRowKey = 91;
  view.setDatasetRowKey(u"done"_s, kRowKey);
  view.setDatasetProgress(progressSet(
      kRowKey, PJ::CurveTreeView::DatasetProgress{
                   .display_name = u"done"_s,
                   .fraction = 1.0,
                   .cancellable = true,
                   .state = PJ::CurveTreeView::DatasetProgress::State::kCompleted,
                   .flash_on = false,
               }));
  view.show();
  QApplication::processEvents();

  QTreeWidgetItem* done = findTopLevel(view, u"done"_s);
  ASSERT_NE(done, nullptr);

  int requests = 0;
  QObject::connect(&view, &PJ::CurveTreeView::cancelRequested, &view, [&requests](quint64, bool) { ++requests; });

  // The rects still exist (they are geometry, not state) — that is what keeps
  // the cluster stable — but pressing them emits nothing.
  EXPECT_FALSE(PJ::CurveTreeViewTestPeer::cancelButtonRect(view, done).isEmpty());
  EXPECT_FALSE(PJ::CurveTreeViewTestPeer::discardButtonRect(view, done).isEmpty());
  sendMousePress(view, cancelButtonPosition(view, done));
  sendMousePress(view, discardButtonPosition(view, done));
  EXPECT_EQ(requests, 0);
}

// The ✕ and the bin are painted glyphs with no label, so nothing tells the user
// which one keeps the partial data and which one throws it away until they
// hover. The tooltips resolve through the same hit-test as the click, so a
// tooltip can never describe a glyph other than the one that would be pressed.
TEST(CurveTreeViewTest, StopAffordanceTooltipsDistinguishKeepFromDiscard) {
  TestCurveTreeView view;
  view.resize(360, 180);
  view.addCurves({u"loading"_s});
  constexpr quint64 kRowKey = 101;
  view.setDatasetRowKey(u"loading"_s, kRowKey);
  view.setDatasetProgress(progressSet(
      kRowKey, PJ::CurveTreeView::DatasetProgress{
                   .display_name = u"loading"_s,
                   .fraction = 0.4,
                   .cancellable = true,
                   .state = PJ::CurveTreeView::DatasetProgress::State::kLoading,
                   .flash_on = false,
               }));
  view.show();
  QApplication::processEvents();

  QTreeWidgetItem* loading = findTopLevel(view, u"loading"_s);
  ASSERT_NE(loading, nullptr);

  EXPECT_EQ(
      PJ::CurveTreeViewTestPeer::stopButtonTooltip(view, cancelButtonPosition(view, loading)),
      u"Stop loading and keep the data received so far"_s);
  EXPECT_EQ(
      PJ::CurveTreeViewTestPeer::stopButtonTooltip(view, discardButtonPosition(view, loading)),
      u"Stop loading and discard the partial data"_s);

  // The cluster's tooltips must not leak across the rest of the row: the name
  // is not an affordance, and a row-wide "stop loading" tooltip would be a lie
  // about where to click.
  const QRect row_rect = view.visualItemRect(loading);
  const QPoint name_pos(row_rect.left() + 4, row_rect.center().y());
  EXPECT_TRUE(PJ::CurveTreeViewTestPeer::stopButtonTooltip(view, name_pos).isEmpty());
}

// A producer that can stop but cannot roll back paints the bin greyed. That is
// the affordance that most needs a tooltip: it still looks like a choice, so it
// has to say why it is not one.
TEST(CurveTreeViewTest, InertBinTooltipExplainsThatTheSourceCannotDiscard) {
  TestCurveTreeView view;
  view.resize(360, 180);
  view.addCurves({u"stoponly"_s});
  constexpr quint64 kRowKey = 102;
  view.setDatasetRowKey(u"stoponly"_s, kRowKey);
  view.setDatasetProgress(progressSet(
      kRowKey, PJ::CurveTreeView::DatasetProgress{
                   .display_name = u"stoponly"_s,
                   .fraction = 0.3,
                   .cancellable = true,
                   .discardable = false,
                   .state = PJ::CurveTreeView::DatasetProgress::State::kLoading,
                   .flash_on = false,
               }));
  view.show();
  QApplication::processEvents();

  QTreeWidgetItem* row = findTopLevel(view, u"stoponly"_s);
  ASSERT_NE(row, nullptr);

  EXPECT_EQ(
      PJ::CurveTreeViewTestPeer::stopButtonTooltip(view, discardButtonPosition(view, row)),
      u"This source can stop but cannot discard partial data"_s);
  // Stopping-and-keeping is still on offer, and still says so.
  EXPECT_EQ(
      PJ::CurveTreeViewTestPeer::stopButtonTooltip(view, cancelButtonPosition(view, row)),
      u"Stop loading and keep the data received so far"_s);
}

// Both glyphs stay painted through the terminal linger so the cluster keeps its
// shape as the row retires. They answer to nothing there, so they must not
// offer a stop that can no longer happen either.
TEST(CurveTreeViewTest, FinishedRowStopAffordancesOfferNoTooltip) {
  TestCurveTreeView view;
  view.resize(360, 180);
  view.addCurves({u"done"_s});
  constexpr quint64 kRowKey = 103;
  view.setDatasetRowKey(u"done"_s, kRowKey);
  view.setDatasetProgress(progressSet(
      kRowKey, PJ::CurveTreeView::DatasetProgress{
                   .display_name = u"done"_s,
                   .fraction = 1.0,
                   .cancellable = true,
                   .state = PJ::CurveTreeView::DatasetProgress::State::kCompleted,
                   .flash_on = false,
               }));
  view.show();
  QApplication::processEvents();

  QTreeWidgetItem* done = findTopLevel(view, u"done"_s);
  ASSERT_NE(done, nullptr);

  // The rects still exist — that is what keeps the cluster stable — so this is
  // a real hit on each glyph, not a miss that trivially returns nothing.
  ASSERT_FALSE(PJ::CurveTreeViewTestPeer::cancelButtonRect(view, done).isEmpty());
  ASSERT_FALSE(PJ::CurveTreeViewTestPeer::discardButtonRect(view, done).isEmpty());
  EXPECT_TRUE(PJ::CurveTreeViewTestPeer::stopButtonTooltip(view, cancelButtonPosition(view, done)).isEmpty());
  EXPECT_TRUE(PJ::CurveTreeViewTestPeer::stopButtonTooltip(view, discardButtonPosition(view, done)).isEmpty());

  // The event is still consumed: the pointer is over a glyph, so a tooltip left
  // showing from the live phase is hidden rather than replaced by the row's own.
  const QPoint keep_pos = cancelButtonPosition(view, done);
  QHelpEvent over_inert_glyph(QEvent::ToolTip, keep_pos, view.viewport()->mapToGlobal(keep_pos));
  EXPECT_TRUE(view.viewportEvent(&over_inert_glyph));
}

// In production the tooltip is only ever reached through QEvent::ToolTip, so
// drive a real one: over a glyph the view answers it, and everywhere else it
// falls through to the normal per-item tooltip path.
TEST(CurveTreeViewTest, ToolTipEventOverAStopAffordanceIsAnsweredByTheView) {
  TestCurveTreeView view;
  view.resize(360, 180);
  view.addCurves({u"loading"_s});
  constexpr quint64 kRowKey = 104;
  view.setDatasetRowKey(u"loading"_s, kRowKey);
  view.setDatasetProgress(progressSet(
      kRowKey, PJ::CurveTreeView::DatasetProgress{
                   .display_name = u"loading"_s,
                   .fraction = 0.5,
                   .cancellable = true,
                   .state = PJ::CurveTreeView::DatasetProgress::State::kLoading,
                   .flash_on = false,
               }));
  view.show();
  QApplication::processEvents();

  QTreeWidgetItem* loading = findTopLevel(view, u"loading"_s);
  ASSERT_NE(loading, nullptr);

  const QPoint keep_pos = cancelButtonPosition(view, loading);
  QHelpEvent over_glyph(QEvent::ToolTip, keep_pos, view.viewport()->mapToGlobal(keep_pos));
  EXPECT_TRUE(view.viewportEvent(&over_glyph)) << "the ✕ must answer its own tooltip event";

  const QRect row_rect = view.visualItemRect(loading);
  const QPoint name_pos(row_rect.left() + 4, row_rect.center().y());
  QHelpEvent over_name(QEvent::ToolTip, name_pos, view.viewport()->mapToGlobal(name_pos));
  EXPECT_FALSE(view.viewportEvent(&over_name)) << "outside the cluster the row keeps its own tooltip handling";
}

// The hover provider sees the row's key for a leaf and its rebuilt tree path
// for a keyless group; a row with a static tooltip is left to Qt.
TEST(CurveTreeViewTest, ToolTipProviderIsAskedForKeylessGroupsAndLeaves) {
  TestCurveTreeView view;
  view.resize(360, 240);
  view.setViewMode(PJ::CurveTreeView::ViewMode::kShowTopics);
  view.addCatalogItems({
      {.key = u"k1"_s, .dataset = u"drive.mcap"_s, .topic = u"/imu/data"_s, .field = u"x"_s},
      {.key = u"k2"_s,
       .dataset = u"drive.mcap"_s,
       .topic = u"/cam"_s,
       .field = {},
       .selectable = false,
       .draggable = true,
       .tooltip = u"static"_s},
  });
  view.expandAll();
  view.show();
  QApplication::processEvents();

  std::vector<std::pair<QString, QString>> calls;
  view.setTooltipProvider([&](const QString& key, const QString& tree_path) {
    calls.emplace_back(key, tree_path);
    return u"tip"_s;
  });
  const auto hover = [&](QTreeWidgetItem* item) {
    const QRect rect = view.visualItemRect(item);
    const QPoint pos(rect.left() + 4, rect.center().y());
    QHelpEvent event(QEvent::ToolTip, pos, view.viewport()->mapToGlobal(pos));
    return view.viewportEvent(&event);
  };

  QTreeWidgetItem* dataset = findTopLevel(view, u"drive.mcap"_s);
  ASSERT_NE(dataset, nullptr);
  QTreeWidgetItem* topic = nullptr;
  QTreeWidgetItem* cam = nullptr;
  for (int i = 0; i < dataset->childCount(); ++i) {
    (dataset->child(i)->text(0) == u"/imu/data"_s ? topic : cam) = dataset->child(i);
  }
  ASSERT_NE(topic, nullptr);
  ASSERT_NE(cam, nullptr);
  ASSERT_EQ(topic->childCount(), 1);

  EXPECT_TRUE(hover(dataset));
  EXPECT_TRUE(hover(topic));
  EXPECT_TRUE(hover(topic->child(0)));
  hover(cam);  // Qt shows the static tooltip itself; the provider must not be asked
  ASSERT_EQ(calls.size(), 3U);
  EXPECT_EQ(calls[0], std::make_pair(QString{}, u"drive.mcap"_s));
  EXPECT_EQ(calls[1], std::make_pair(QString{}, u"drive.mcap/imu/data"_s));
  EXPECT_EQ(calls[2], std::make_pair(u"k1"_s, u"drive.mcap/imu/data/x"_s));
}

TEST(CurveTreeViewTest, LeftPressOnCancellableLoadingRowRequestsCancelWithoutSelecting) {
  TestCurveTreeView view;
  view.resize(360, 180);
  view.addCurves({u"loading"_s, u"selected"_s});
  constexpr quint64 kRowKey = 51;
  view.setDatasetRowKey(u"loading"_s, kRowKey);
  view.setDatasetProgress(progressSet(
      kRowKey, PJ::CurveTreeView::DatasetProgress{
                   .display_name = u"loading"_s,
                   .fraction = 0.4,
                   .cancellable = true,
                   .state = PJ::CurveTreeView::DatasetProgress::State::kLoading,
                   .flash_on = false,
               }));
  view.show();
  QApplication::processEvents();

  QTreeWidgetItem* loading = findTopLevel(view, u"loading"_s);
  QTreeWidgetItem* selected = findTopLevel(view, u"selected"_s);
  ASSERT_NE(loading, nullptr);
  ASSERT_NE(selected, nullptr);
  selected->setSelected(true);

  std::vector<quint64> requested_keys;
  QObject::connect(&view, &PJ::CurveTreeView::cancelRequested, &view, [&requested_keys](quint64 row_key, bool) {
    requested_keys.push_back(row_key);
  });

  const QPoint cancel_pos = cancelButtonPosition(view, loading);
  ASSERT_TRUE(view.viewport()->rect().contains(cancel_pos));
  EXPECT_TRUE(sendMousePress(view, cancel_pos));

  EXPECT_EQ(requested_keys, (std::vector<quint64>{kRowKey}));
  EXPECT_FALSE(loading->isSelected());
  EXPECT_TRUE(selected->isSelected());
  EXPECT_FALSE(PJ::CurveTreeViewTestPeer::suppressNextRelease(view));
  EXPECT_EQ(PJ::CurveTreeViewTestPeer::dragButton(view), Qt::NoButton);
  EXPECT_TRUE(PJ::CurveTreeViewTestPeer::dragPayloadIsEmpty(view));

  sendMouseRelease(view, cancel_pos);
  EXPECT_EQ(requested_keys, (std::vector<quint64>{kRowKey}));
  EXPECT_FALSE(loading->isSelected());
  EXPECT_TRUE(selected->isSelected());
}

TEST(CurveTreeViewTest, RightPressOnCancellableLoadingRowDoesNotRequestCancel) {
  TestCurveTreeView view;
  view.resize(360, 180);
  view.addCurve(u"loading"_s);
  constexpr quint64 kRowKey = 52;
  view.setDatasetRowKey(u"loading"_s, kRowKey);
  view.setDatasetProgress(progressSet(
      kRowKey, PJ::CurveTreeView::DatasetProgress{
                   .display_name = u"loading"_s,
                   .indeterminate = true,
                   .cancellable = true,
                   .state = PJ::CurveTreeView::DatasetProgress::State::kLoading,
                   .flash_on = false,
               }));
  view.show();
  QApplication::processEvents();

  QTreeWidgetItem* loading = findTopLevel(view, u"loading"_s);
  ASSERT_NE(loading, nullptr);
  std::vector<quint64> requested_keys;
  QObject::connect(&view, &PJ::CurveTreeView::cancelRequested, &view, [&requested_keys](quint64 row_key, bool) {
    requested_keys.push_back(row_key);
  });

  const QPoint cancel_pos = cancelButtonPosition(view, loading);
  sendMousePress(view, cancel_pos, Qt::RightButton);
  EXPECT_TRUE(requested_keys.empty());
  sendMouseRelease(view, cancel_pos, Qt::RightButton);
}

TEST(CurveTreeViewTest, PressOnNonCancellableLoadingRowDoesNotRequestCancel) {
  TestCurveTreeView view;
  view.resize(360, 180);
  view.addCurve(u"loading"_s);
  constexpr quint64 kRowKey = 53;
  view.setDatasetRowKey(u"loading"_s, kRowKey);
  view.setDatasetProgress(progressSet(
      kRowKey, PJ::CurveTreeView::DatasetProgress{
                   .display_name = u"loading"_s,
                   .fraction = 0.4,
                   .cancellable = false,
                   .state = PJ::CurveTreeView::DatasetProgress::State::kLoading,
                   .flash_on = false,
               }));
  view.show();
  QApplication::processEvents();

  QTreeWidgetItem* loading = findTopLevel(view, u"loading"_s);
  ASSERT_NE(loading, nullptr);
  std::vector<quint64> requested_keys;
  QObject::connect(&view, &PJ::CurveTreeView::cancelRequested, &view, [&requested_keys](quint64 row_key, bool) {
    requested_keys.push_back(row_key);
  });

  const QPoint cancel_pos = cancelButtonPosition(view, loading);
  sendMousePress(view, cancel_pos);
  EXPECT_TRUE(requested_keys.empty());
  sendMouseRelease(view, cancel_pos);
}

TEST(CurveTreeViewTest, AnimationTimerRunsOnlyForRowsThatNeedAnimation) {
  PJ::CurveTreeView view;
  view.addCurves({u"first"_s, u"second"_s});
  constexpr quint64 kFirstRowKey = 54;
  constexpr quint64 kSecondRowKey = 55;
  view.setDatasetRowKey(u"first"_s, kFirstRowKey);
  view.setDatasetRowKey(u"second"_s, kSecondRowKey);

  QTimer* animation_timer = view.findChild<QTimer*>(u"datasetProgressAnimationTimer"_s);
  ASSERT_NE(animation_timer, nullptr);
  EXPECT_EQ(animation_timer->parent(), &view);
  EXPECT_EQ(animation_timer->interval(), 33);
  EXPECT_FALSE(animation_timer->isActive());

  QHash<quint64, PJ::CurveTreeView::DatasetProgress> progress_by_key;
  progress_by_key.insert(
      kFirstRowKey, PJ::CurveTreeView::DatasetProgress{
                        .display_name = u"first"_s,
                        .fraction = 0.25,
                        .state = PJ::CurveTreeView::DatasetProgress::State::kLoading,
                    });
  progress_by_key.insert(
      kSecondRowKey, PJ::CurveTreeView::DatasetProgress{
                         .display_name = u"second"_s,
                         .fraction = 0.75,
                         .state = PJ::CurveTreeView::DatasetProgress::State::kLoading,
                     });
  view.setDatasetProgress(progress_by_key);
  EXPECT_FALSE(animation_timer->isActive());

  progress_by_key[kSecondRowKey].indeterminate = true;
  view.setDatasetProgress(progress_by_key);
  EXPECT_TRUE(animation_timer->isActive());

  progress_by_key[kSecondRowKey].indeterminate = false;
  progress_by_key[kSecondRowKey].fraction = 0.9;
  view.setDatasetProgress(progress_by_key);
  EXPECT_FALSE(animation_timer->isActive());
}

TEST(CurveTreeViewTest, AnimationTimerTracksFailedFlashPhase) {
  PJ::CurveTreeView view;
  constexpr quint64 kRowKey = 56;
  PJ::CurveTreeView::DatasetProgress failed{
      .display_name = u"failed"_s,
      .state = PJ::CurveTreeView::DatasetProgress::State::kFailed,
      .flash_on = true,
  };
  QTimer* animation_timer = view.findChild<QTimer*>(u"datasetProgressAnimationTimer"_s);
  ASSERT_NE(animation_timer, nullptr);

  view.setDatasetProgress(progressSet(kRowKey, failed));
  EXPECT_TRUE(animation_timer->isActive());

  failed.flash_on = false;
  view.setDatasetProgress(progressSet(kRowKey, failed));
  EXPECT_FALSE(animation_timer->isActive());
}

TEST(CurveTreeViewTest, TypeFilterClassifiesByTopicAndGovernsWholeSubtree) {
  PJ::CurveTreeView view;
  view.addCatalogItems({
      // Plain numeric topic (Plot bucket): veh/imu/x
      PJ::CurveTreeView::CurvePath{
          .key = QStringLiteral("veh/imu/x"),
          .dataset = QStringLiteral("veh"),
          .topic = QStringLiteral("imu"),
          .field = QStringLiteral("x"),
      },
      // 3D object topic veh/odom (non-selectable terminal) ...
      PJ::CurveTreeView::CurvePath{
          .key = QStringLiteral("veh/odom"),
          .dataset = QStringLiteral("veh"),
          .topic = QStringLiteral("odom"),
          .field = {},
          .selectable = false,
          .is_3d_object_topic = true,
      },
      // ... that ALSO exposes a scalar field veh/odom/px
      PJ::CurveTreeView::CurvePath{
          .key = QStringLiteral("veh/odom/px"),
          .dataset = QStringLiteral("veh"),
          .topic = QStringLiteral("odom"),
          .field = QStringLiteral("px"),
      },
      // 2D image topic veh/cam
      PJ::CurveTreeView::CurvePath{
          .key = QStringLiteral("veh/cam"),
          .dataset = QStringLiteral("veh"),
          .topic = QStringLiteral("cam"),
          .field = {},
          .selectable = false,
          .is_image_topic = true,
      },
  });

  QTreeWidgetItem* veh = view.topLevelItem(0);
  ASSERT_NE(veh, nullptr);
  QTreeWidgetItem* imu = findChild(veh, QStringLiteral("imu"));
  QTreeWidgetItem* odom = findChild(veh, QStringLiteral("odom"));
  QTreeWidgetItem* cam = findChild(veh, QStringLiteral("cam"));
  ASSERT_NE(imu, nullptr);
  ASSERT_NE(odom, nullptr);
  ASSERT_NE(cam, nullptr);
  QTreeWidgetItem* imu_x = findChild(imu, QStringLiteral("x"));
  QTreeWidgetItem* odom_px = findChild(odom, QStringLiteral("px"));
  ASSERT_NE(imu_x, nullptr);
  ASSERT_NE(odom_px, nullptr);

  // Hiding 3D collapses the odom topic AND its timeseries child together; the
  // plain-numeric imu topic and the 2D cam stay.
  view.setVisibleCurveKinds(/*plot=*/true, /*scene2d=*/true, /*scene3d=*/false);
  EXPECT_TRUE(odom->isHidden());
  EXPECT_TRUE(odom_px->isHidden());
  EXPECT_FALSE(imu->isHidden());
  EXPECT_FALSE(imu_x->isHidden());
  EXPECT_FALSE(cam->isHidden());
  EXPECT_FALSE(veh->isHidden());

  // Hiding Plot collapses the numeric imu topic; the 3D odom topic keeps its
  // field (it belongs to a 3D topic, not the Plot bucket); the 2D cam stays.
  view.setVisibleCurveKinds(/*plot=*/false, /*scene2d=*/true, /*scene3d=*/true);
  EXPECT_TRUE(imu->isHidden());
  EXPECT_TRUE(imu_x->isHidden());
  EXPECT_FALSE(odom->isHidden());
  EXPECT_FALSE(odom_px->isHidden());
  EXPECT_FALSE(cam->isHidden());

  // Everything back on restores every row.
  view.setVisibleCurveKinds(true, true, true);
  EXPECT_FALSE(odom->isHidden());
  EXPECT_FALSE(odom_px->isHidden());
  EXPECT_FALSE(imu->isHidden());
  EXPECT_FALSE(cam->isHidden());
}

TEST(CurveTreeViewTest, EmptyTypeFilterShowsMessageChildAndKeepsDatasetVisible) {
  PJ::CurveTreeView view;
  view.setEmptyFilterMessage(QStringLiteral("No series match"));
  view.addCatalogItem(
      PJ::CurveTreeView::CurvePath{
          .key = QStringLiteral("veh/pc"),
          .dataset = QStringLiteral("veh"),
          .topic = QStringLiteral("pc"),
          .field = {},
          .selectable = false,
          .is_3d_object_topic = true,
      });

  QTreeWidgetItem* veh = view.topLevelItem(0);
  ASSERT_NE(veh, nullptr);

  // Hide the only (3D) topic: the dataset name stays and a message child appears.
  view.setVisibleCurveKinds(/*plot=*/true, /*scene2d=*/true, /*scene3d=*/false);
  EXPECT_FALSE(veh->isHidden());
  QTreeWidgetItem* message = findChild(veh, QStringLiteral("No series match"));
  ASSERT_NE(message, nullptr);
  EXPECT_FALSE(message->isHidden());
  EXPECT_FALSE(message->flags().testFlag(Qt::ItemIsSelectable));

  // Restore 3D: the message hides and the real topic returns.
  view.setVisibleCurveKinds(true, true, true);
  EXPECT_TRUE(message->isHidden());
  QTreeWidgetItem* pc = findChild(veh, QStringLiteral("pc"));
  ASSERT_NE(pc, nullptr);
  EXPECT_FALSE(pc->isHidden());
}

int main(int argc, char** argv) {
  if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  testing::InitGoogleTest(&argc, argv);
  QApplication app(argc, argv);
  return RUN_ALL_TESTS();
}
