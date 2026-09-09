// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QLabel>
#include <QList>
#include <QListWidget>
#include <QMetaObject>
#include <QPointer>
#include <QSignalSpy>
#include <QString>
#include <QToolButton>
#include <QVariant>
#include <QWidget>
#include <Qt>
#include <QtGlobal>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <vector>

#include "pj_widgets/ConfigPanelHost.h"
#include "pj_widgets/LayerListView.h"
using namespace Qt::StringLiterals;

namespace {

std::vector<qint64> ids(std::initializer_list<qint64> values) {
  return {values.begin(), values.end()};
}

int directChildWidgetCount(const QWidget& widget) {
  return static_cast<int>(widget.findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly).size());
}

}  // namespace

TEST(LayerListViewTest, SetRowsReturnsIdsInOrder) {
  PJ::LayerListView view;

  view.setRows({
      {.id = 10, .name = u"first"_s, .visible = true},
      {.id = 20, .name = u"second"_s, .visible = false},
      {.id = 30, .name = u"third"_s, .visible = true},
  });

  EXPECT_EQ(view.order(), ids({10, 20, 30}));
}

TEST(LayerListViewTest, AddAndRemoveRowsUpdateOrder) {
  PJ::LayerListView view;

  view.addRow({.id = 1, .name = u"one"_s, .visible = true});
  view.addRow({.id = 2, .name = u"two"_s, .visible = true});
  view.addRow({.id = 3, .name = u"three"_s, .visible = true});
  EXPECT_EQ(view.order(), ids({1, 2, 3}));

  view.removeRow(2);
  EXPECT_EQ(view.order(), ids({1, 3}));
}

TEST(LayerListViewTest, CurrentIdRoundTrips) {
  PJ::LayerListView view;
  view.setRows({
      {.id = 101, .name = u"first"_s, .visible = true},
      {.id = 202, .name = u"second"_s, .visible = true},
  });

  view.setCurrentId(202);

  ASSERT_TRUE(view.currentId().has_value());
  EXPECT_EQ(*view.currentId(), 202);
}

TEST(LayerListViewTest, VisibilityToggleSignalFiresFromEyeButton) {
  PJ::LayerListView view;
  view.setRows({{.id = 7, .name = u"row"_s, .visible = true}});
  QSignalSpy spy(&view, &PJ::LayerListView::visibilityToggled);
  ASSERT_TRUE(spy.isValid());

  auto* eye = view.findChild<QToolButton*>(u"curveVisibilityToggle"_s);
  ASSERT_NE(eye, nullptr);
  eye->setChecked(false);

  ASSERT_EQ(spy.count(), 1);
  const QList<QVariant> args = spy.takeFirst();
  ASSERT_EQ(args.size(), 2);
  EXPECT_EQ(args[0].toLongLong(), 7);
  EXPECT_FALSE(args[1].toBool());
}

TEST(LayerListViewTest, RemoveRequestedSignalFiresFromTrashButton) {
  PJ::LayerListView view;
  view.setRows({{.id = 9, .name = u"row"_s, .visible = true}});
  QSignalSpy spy(&view, &PJ::LayerListView::removeRequested);
  ASSERT_TRUE(spy.isValid());

  auto* trash = view.findChild<QAbstractButton*>(u"curveTrashToggle"_s);
  ASSERT_NE(trash, nullptr);
  trash->click();

  ASSERT_EQ(spy.count(), 1);
  const QList<QVariant> args = spy.takeFirst();
  ASSERT_EQ(args.size(), 1);
  EXPECT_EQ(args[0].toLongLong(), 9);
}

TEST(LayerListViewTest, SetRowWarningUpdatesTooltip) {
  PJ::LayerListView view;
  view.setRows({{.id = 1, .name = u"row"_s, .visible = true}});
  auto* list = view.findChild<QListWidget*>();
  ASSERT_NE(list, nullptr);
  ASSERT_EQ(list->count(), 1);

  auto* label = view.findChild<QLabel*>();  // the row's ElidingLabel (a QLabel)
  ASSERT_NE(label, nullptr);

  view.setRowWarning(1, true, u"missing source"_s);
  EXPECT_EQ(list->item(0)->toolTip(), u"missing source"_s);
  EXPECT_FALSE(label->styleSheet().isEmpty());  // warning color applied

  view.setRowWarning(1, false);
  EXPECT_EQ(list->item(0)->toolTip(), u"row"_s);
  EXPECT_TRUE(label->styleSheet().isEmpty());  // styling cleared
  EXPECT_EQ(view.order(), ids({1}));
}

TEST(LayerListViewTest, SetRowsAppliesAndDedupesWarningRows) {
  PJ::LayerListView view;
  view.setRows({
      {.id = 1, .name = u"a"_s, .visible = true},
      {.id = 2, .name = u"b"_s, .visible = true, .warn = true, .warning_reason = u"bad"_s},
      {.id = 2, .name = u"dup"_s, .visible = true},  // duplicate id: ignored
  });

  // Duplicate id dropped, and the warning carried by the LayerRow is applied.
  EXPECT_EQ(view.order(), ids({1, 2}));
  auto* list = view.findChild<QListWidget*>();
  ASSERT_NE(list, nullptr);
  EXPECT_EQ(list->item(1)->toolTip(), u"bad"_s);
}

TEST(LayerListViewTest, ReorderPreservesWarningAndEmitsReordered) {
  PJ::LayerListView view;
  view.setRows({
      {.id = 1, .name = u"a"_s, .visible = true},
      {.id = 2, .name = u"b"_s, .visible = true, .warn = true, .warning_reason = u"bad"_s},
      {.id = 3, .name = u"c"_s, .visible = true},
  });
  QSignalSpy spy(&view, &PJ::LayerListView::reordered);
  ASSERT_TRUE(spy.isValid());

  auto* list = view.findChild<QListWidget*>();
  ASSERT_NE(list, nullptr);

  // Drive the internal drag-reorder signal (move row 0 to the end). onRowMoved
  // is a QueuedConnection, so let the event loop deliver it.
  ASSERT_TRUE(QMetaObject::invokeMethod(list, "rowMoved", Qt::DirectConnection, Q_ARG(int, 0), Q_ARG(int, 3)));
  QCoreApplication::processEvents();

  EXPECT_EQ(view.order(), ids({2, 3, 1}));
  ASSERT_EQ(spy.count(), 1);

  // The warning on id 2 survived the full rebuild (now at index 0).
  EXPECT_EQ(list->item(0)->toolTip(), u"bad"_s);
}

TEST(LayerListViewTest, AddRowIgnoresDuplicateId) {
  PJ::LayerListView view;
  view.addRow({.id = 5, .name = u"first"_s, .visible = true});
  view.addRow({.id = 5, .name = u"second"_s, .visible = true});

  // The duplicate is rejected, not appended — exactly one row with id 5.
  EXPECT_EQ(view.order(), ids({5}));

  auto* list = view.findChild<QListWidget*>();
  ASSERT_NE(list, nullptr);
  EXPECT_EQ(list->count(), 1);
}

// ReorderIds is the pure arithmetic behind drag-reorder (onRowMoved). `to` is a
// drop position in [0, size], as produced by the drop indicator.
TEST(ReorderIdsTest, MoveToEnd) {
  EXPECT_EQ(PJ::detail::reorderIds(ids({10, 20, 30, 40}), 0, 4), ids({20, 30, 40, 10}));
}

TEST(ReorderIdsTest, MoveToFront) {
  EXPECT_EQ(PJ::detail::reorderIds(ids({10, 20, 30, 40}), 3, 0), ids({40, 10, 20, 30}));
}

TEST(ReorderIdsTest, MoveDownPastOneNeighbour) {
  EXPECT_EQ(PJ::detail::reorderIds(ids({10, 20, 30, 40}), 1, 3), ids({10, 30, 20, 40}));
}

TEST(ReorderIdsTest, MoveUp) {
  EXPECT_EQ(PJ::detail::reorderIds(ids({10, 20, 30, 40}), 2, 1), ids({10, 30, 20, 40}));
}

TEST(ReorderIdsTest, DropOntoSelfIsNoOp) {
  EXPECT_EQ(PJ::detail::reorderIds(ids({10, 20, 30}), 1, 1), ids({10, 20, 30}));
}

TEST(ReorderIdsTest, DropJustBelowSelfIsNoOp) {
  // from=0, to=1 means "drop right below where it already is" — the dst-- guard
  // must collapse this to a no-op.
  EXPECT_EQ(PJ::detail::reorderIds(ids({10, 20, 30}), 0, 1), ids({10, 20, 30}));
}

TEST(ReorderIdsTest, ToBeyondSizeClampsToEnd) {
  EXPECT_EQ(PJ::detail::reorderIds(ids({10, 20, 30}), 0, 9), ids({20, 30, 10}));
}

TEST(ReorderIdsTest, OutOfRangeFromReturnsUnchanged) {
  EXPECT_EQ(PJ::detail::reorderIds(ids({10, 20, 30}), -1, 0), ids({10, 20, 30}));
  EXPECT_EQ(PJ::detail::reorderIds(ids({10, 20, 30}), 3, 0), ids({10, 20, 30}));
}

TEST(ConfigPanelHostTest, ReplacesConfigWidgetAndDeletesPreviousLater) {
  PJ::ConfigPanelHost host;
  auto* first = new QLabel(u"first"_s);
  auto* second = new QLabel(u"second"_s);
  QPointer<QWidget> old = first;

  host.setConfigWidget(first);
  EXPECT_EQ(directChildWidgetCount(host), 1);

  host.setConfigWidget(second);
  EXPECT_EQ(directChildWidgetCount(host), 1);
  EXPECT_EQ(second->parentWidget(), &host);

  QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  EXPECT_TRUE(old.isNull());
}

TEST(ConfigPanelHostTest, SetNullClearsHostedWidget) {
  PJ::ConfigPanelHost host;
  auto* widget = new QLabel(u"only"_s);
  QPointer<QWidget> tracked = widget;

  host.setConfigWidget(widget);
  EXPECT_EQ(directChildWidgetCount(host), 1);

  host.setConfigWidget(nullptr);  // equivalent to clear()
  EXPECT_EQ(directChildWidgetCount(host), 0);

  QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  EXPECT_TRUE(tracked.isNull());
}
