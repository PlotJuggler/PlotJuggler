// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Regression test for a stuck camera gesture: right-clicking the 3D view shows a
// context menu (Split Horizontally/Vertically, Clear) via DockWidget's event
// filter reacting to a QContextMenuEvent — SceneViewWidget itself has no
// view-side context-menu code (see pj_scene3D/CLAUDE.md). The menu's
// QMenu::exec() runs a nested event loop that can swallow the right button's
// mouseReleaseEvent before SceneViewWidget ever sees it, so the widget's own
// press/release-latched gesture state must not stay latched forever — the next
// mouse move (with no button held) has to self-heal instead of continuing to
// zoom/pan/rotate on every hover.

#include <gtest/gtest.h>

#include <QApplication>
#include <QMouseEvent>
#include <QPointF>

#include "pj_scene3d_widgets/scene_view_widget.h"

using pj::scene3d::SceneViewWidget;

TEST(SceneViewWidgetMouseGesture, LostReleaseSelfHealsOnNextMove) {
  SceneViewWidget view;
  view.resize(400, 300);

  int presentation_changes = 0;
  QObject::connect(&view, &SceneViewWidget::presentationChanged, [&] { ++presentation_changes; });

  const QPointF press_pos(200.0, 100.0);
  QMouseEvent press(
      QEvent::MouseButtonPress, press_pos, press_pos, press_pos, Qt::RightButton, Qt::RightButton, Qt::NoModifier);
  QApplication::sendEvent(&view, &press);

  // A real right-drag: what would normally zoom the camera.
  const QPointF drag_pos(200.0, 150.0);
  QMouseEvent drag(QEvent::MouseMove, drag_pos, drag_pos, drag_pos, Qt::NoButton, Qt::RightButton, Qt::NoModifier);
  QApplication::sendEvent(&view, &drag);
  const float radius_after_drag = view.camera().state().radius;
  ASSERT_NE(radius_after_drag, 5.0f) << "precondition: the drag actually zoomed the default camera";
  EXPECT_EQ(presentation_changes, 0) << "a camera drag must not snapshot every mouse-move event";

  // The button's mouseReleaseEvent never arrives (modelling it being swallowed by
  // the context menu's nested exec()); the next move already reports the button
  // physically up, exactly like Qt would once the real release actually happened.
  const QPointF phantom_pos(200.0, 250.0);
  QMouseEvent phantom_move(
      QEvent::MouseMove, phantom_pos, phantom_pos, phantom_pos, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(&view, &phantom_move);

  EXPECT_FLOAT_EQ(radius_after_drag, view.camera().state().radius)
      << "camera must not keep zooming once its button is no longer physically held";
  EXPECT_EQ(presentation_changes, 1)
      << "the interrupted gesture must still commit exactly once, like a normal release would";
}
