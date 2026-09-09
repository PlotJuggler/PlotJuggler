// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPointF>
#include <QSignalSpy>
#include <QTest>
#include <Qt>
#include <QtGlobal>

#include "pj_widgets/IntScrubber.h"
#include "pj_widgets/ScrubberBase.h"
using namespace Qt::StringLiterals;

namespace {

// Fixed widget size so the arrow / body hit-zones are deterministic without
// showing the widget: centerRect() is x ∈ [16, 64), arrows are the 16-px edges.
constexpr int kWidth = 80;
constexpr int kHeight = 20;

// Send a synchronous mouse event with explicit local + global positions (the
// scrubber's drag math reads globalPosition(); zone detection reads pos()).
// Global coords stay well inside the offscreen screen so the screen-edge
// cursor-wrap path never triggers during the test drag.
void sendMouse(
    QWidget* widget, QEvent::Type type, QPointF local, QPointF global, Qt::MouseButton button,
    Qt::MouseButtons buttons) {
  QMouseEvent event(type, local, global, button, buttons, Qt::NoModifier);
  QCoreApplication::sendEvent(widget, &event);
}

// Press in the body, drag right far enough to cross the click threshold and
// step the value, then release — one complete scrub gesture.
void performBodyDrag(PJ::IntScrubber& scrubber) {
  const QPointF body_local(40, kHeight / 2);
  sendMouse(&scrubber, QEvent::MouseButtonPress, body_local, QPointF(100, 100), Qt::LeftButton, Qt::LeftButton);
  sendMouse(&scrubber, QEvent::MouseMove, QPointF(70, kHeight / 2), QPointF(140, 100), Qt::NoButton, Qt::LeftButton);
  sendMouse(
      &scrubber, QEvent::MouseButtonRelease, QPointF(70, kHeight / 2), QPointF(140, 100), Qt::LeftButton, Qt::NoButton);
}

// Configure an in-place scrubber (QWidget is non-copyable, so callers own it).
void configure(PJ::IntScrubber& scrubber) {
  scrubber.setRange(0, 100);
  scrubber.resize(kWidth, kHeight);
}

}  // namespace

// A full scrub gesture (press → drag → release) fires editingFinished exactly
// once — the single "value settled" event the persistence layer cares about.
TEST(ScrubberBase, BodyDragFiresEditingFinishedOnceOnRelease) {
  PJ::IntScrubber scrubber;
  configure(scrubber);
  QSignalSpy finished_spy(&scrubber, &PJ::ScrubberBase::editingFinished);
  QSignalSpy changed_spy(&scrubber, &PJ::IntScrubber::valueChanged);

  performBodyDrag(scrubber);

  EXPECT_GT(changed_spy.count(), 1);   // value tracked live, every tick
  EXPECT_EQ(finished_spy.count(), 1);  // but settled exactly once
}

// The whole point of the pattern: nothing "finishes" mid-drag, only on release.
TEST(ScrubberBase, NoEditingFinishedWhileDragInProgress) {
  PJ::IntScrubber scrubber;
  configure(scrubber);
  QSignalSpy finished_spy(&scrubber, &PJ::ScrubberBase::editingFinished);

  const QPointF body_local(40, kHeight / 2);
  sendMouse(&scrubber, QEvent::MouseButtonPress, body_local, QPointF(100, 100), Qt::LeftButton, Qt::LeftButton);
  sendMouse(&scrubber, QEvent::MouseMove, QPointF(70, kHeight / 2), QPointF(140, 100), Qt::NoButton, Qt::LeftButton);

  EXPECT_EQ(finished_spy.count(), 0);  // still dragging — not settled yet
}

// If the platform swallowed the release outside the app, the first move with
// no pressed left button must settle the drag instead of leaving the global
// wasm event filter active until another click.
TEST(ScrubberBase, ButtonlessMoveSelfHealsLostDragRelease) {
  PJ::IntScrubber scrubber;
  configure(scrubber);
  QSignalSpy finished_spy(&scrubber, &PJ::ScrubberBase::editingFinished);

  const QPointF body_local(40, kHeight / 2);
  sendMouse(&scrubber, QEvent::MouseButtonPress, body_local, QPointF(100, 100), Qt::LeftButton, Qt::LeftButton);
  sendMouse(&scrubber, QEvent::MouseMove, QPointF(70, kHeight / 2), QPointF(140, 100), Qt::NoButton, Qt::LeftButton);
  sendMouse(&scrubber, QEvent::MouseMove, QPointF(72, kHeight / 2), QPointF(142, 100), Qt::NoButton, Qt::NoButton);

  EXPECT_EQ(finished_spy.count(), 1);

  // A later physical release must not emit a second settlement.
  sendMouse(
      &scrubber, QEvent::MouseButtonRelease, QPointF(72, kHeight / 2), QPointF(142, 100), Qt::LeftButton, Qt::NoButton);
  EXPECT_EQ(finished_spy.count(), 1);
}

// Clicking an arrow (press steps once, release ends the interaction) settles once.
TEST(ScrubberBase, ArrowClickFiresEditingFinished) {
  PJ::IntScrubber scrubber;
  configure(scrubber);
  QSignalSpy finished_spy(&scrubber, &PJ::ScrubberBase::editingFinished);

  const QPointF arrow_local(kWidth - 8, kHeight / 2);  // inside the right-arrow zone
  sendMouse(&scrubber, QEvent::MouseButtonPress, arrow_local, QPointF(100, 100), Qt::LeftButton, Qt::LeftButton);
  sendMouse(&scrubber, QEvent::MouseButtonRelease, arrow_local, QPointF(100, 100), Qt::LeftButton, Qt::NoButton);

  EXPECT_EQ(finished_spy.count(), 1);
}

// A keyboard edit committed with Return settles once.
TEST(ScrubberBase, KeyboardCommitFiresEditingFinished) {
  PJ::IntScrubber scrubber;
  configure(scrubber);
  QSignalSpy finished_spy(&scrubber, &PJ::ScrubberBase::editingFinished);

  // Click without dragging → enter inline edit mode.
  const QPointF body_local(40, kHeight / 2);
  sendMouse(&scrubber, QEvent::MouseButtonPress, body_local, QPointF(100, 100), Qt::LeftButton, Qt::LeftButton);
  sendMouse(&scrubber, QEvent::MouseButtonRelease, body_local, QPointF(100, 100), Qt::LeftButton, Qt::NoButton);

  auto* editor = scrubber.findChild<QLineEdit*>();
  ASSERT_NE(editor, nullptr);
  editor->setText(u"57"_s);
  QTest::keyClick(editor, Qt::Key_Return);

  EXPECT_EQ(scrubber.value(), 57);
  EXPECT_EQ(finished_spy.count(), 1);
}

// Escaping an inline edit reverts the value and must NOT report a finished edit.
TEST(ScrubberBase, EscapeRevertDoesNotFireEditingFinished) {
  PJ::IntScrubber scrubber;
  configure(scrubber);
  QSignalSpy finished_spy(&scrubber, &PJ::ScrubberBase::editingFinished);

  const QPointF body_local(40, kHeight / 2);
  sendMouse(&scrubber, QEvent::MouseButtonPress, body_local, QPointF(100, 100), Qt::LeftButton, Qt::LeftButton);
  sendMouse(&scrubber, QEvent::MouseButtonRelease, body_local, QPointF(100, 100), Qt::LeftButton, Qt::NoButton);

  auto* editor = scrubber.findChild<QLineEdit*>();
  ASSERT_NE(editor, nullptr);
  editor->setText(u"57"_s);
  QTest::keyClick(editor, Qt::Key_Escape);

  EXPECT_EQ(finished_spy.count(), 0);
}
