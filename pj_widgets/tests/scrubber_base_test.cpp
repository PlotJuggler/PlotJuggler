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
#include <cmath>
#include <limits>

#include "pj_widgets/DoubleScrubber.h"
#include "pj_widgets/IntScrubber.h"
#include "pj_widgets/ScrubberBase.h"
using namespace Qt::StringLiterals;

namespace {

// Fixed widget size, so the arrow and body hit-zones are deterministic without showing the widget: centerRect() is x in
// [16, 64), arrows are the 16-px edges.
constexpr int kWidth = 80;
constexpr int kHeight = 20;

// Sends a synchronous mouse event with explicit local and global positions (the scrubber's drag math reads
// globalPosition(); zone detection reads pos()). Global coords stay well inside the offscreen screen, so the
// screen-edge cursor-wrap path never triggers during the test drag.
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

// If the platform swallowed the release outside the app, the first move with no pressed left button settles the drag
// instead of leaving the global wasm event filter active until another click.
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

// Return (and keypad Enter) in the inline editor commits and closes it. QLineEdit ignores the key after emitting
// returnPressed, so it must not bubble to the scrubber, whose keyPressEvent would reopen the editor. Focus stays on the
// scrubber, and a Return aimed at the scrubber itself still opens the editor.
TEST(ScrubberBase, ReturnCommitsAndClosesEditorWithoutReopening) {
  for (const int key : {Qt::Key_Return, Qt::Key_Enter}) {
    PJ::IntScrubber scrubber;
    configure(scrubber);
    scrubber.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&scrubber));
    scrubber.activateWindow();
    scrubber.setFocus();

    QTest::keyClick(&scrubber, Qt::Key_Return);  // keyboard route into the editor
    auto* editor = scrubber.findChild<QLineEdit*>();
    ASSERT_NE(editor, nullptr);
    ASSERT_TRUE(editor->isVisible());

    editor->setText(u"57"_s);
    QTest::keyClick(editor, static_cast<Qt::Key>(key));

    EXPECT_EQ(scrubber.value(), 57);
    EXPECT_FALSE(editor->isVisible()) << "key " << key << " reopened the editor";
    EXPECT_TRUE(scrubber.hasFocus());

    QTest::keyClick(&scrubber, static_cast<Qt::Key>(key));  // a second Return opens it again
    EXPECT_TRUE(editor->isVisible());
  }
}

// Closing the editor hides its focused QLineEdit, which makes Qt hand focus to the next widget in the window's focus
// chain. Committing with Return or Enter, or reverting with Escape, puts focus back on the scrubber (so Space and
// Return keep reaching it, not a sibling text field); clicking or tabbing to another widget closes the editor too but
// leaves focus where the user sent it.
TEST(ScrubberBase, EditorExitFocusGoesBackToScrubberOnlyForKeyboardExits) {
  QWidget window;
  QLineEdit sibling(&window);  // created first: earlier in the focus chain than the scrubber
  PJ::IntScrubber scrubber(&window);
  configure(scrubber);
  sibling.setGeometry(0, 30, kWidth, kHeight);
  scrubber.move(0, 0);
  window.resize(kWidth, 60);
  window.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
  window.activateWindow();
  ASSERT_TRUE(QTest::qWaitForWindowActive(&window));

  const auto open_editor = [&]() {
    scrubber.setFocus();
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
    auto* editor = scrubber.findChild<QLineEdit*>();
    EXPECT_NE(editor, nullptr);
    EXPECT_TRUE(editor && editor->isVisible());
    EXPECT_EQ(QApplication::focusWidget(), editor);
    return editor;
  };

  for (const int key : {Qt::Key_Return, Qt::Key_Enter, Qt::Key_Escape}) {
    QLineEdit* editor = open_editor();
    QTest::keyClick(QApplication::focusWidget(), static_cast<Qt::Key>(key));
    EXPECT_FALSE(editor->isVisible()) << "key " << key;
    EXPECT_EQ(QApplication::focusWidget(), &scrubber) << "key " << key << " left focus off the scrubber";
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_X);  // a later key must not land in the sibling
    EXPECT_TRUE(sibling.text().isEmpty()) << "key " << key;
  }

  // Focus-out revert: the user moved on, so the scrubber must not take focus back.
  open_editor();
  sibling.setFocus();
  EXPECT_FALSE(scrubber.findChild<QLineEdit*>()->isVisible());
  EXPECT_EQ(QApplication::focusWidget(), &sibling);
}

// "nan" and "inf" parse as doubles, but a scrubber holding one poisons whatever it drives (the speed scrubber made the
// playback time NaN). A typed non-finite value is refused like "abc": the value stays, nothing is emitted.
TEST(ScrubberBase, DoubleScrubberRejectsTypedNonFiniteText) {
  PJ::DoubleScrubber scrubber;
  scrubber.setRange(0.1, 10.0);
  scrubber.setValue(2.5);
  scrubber.resize(kWidth, kHeight);
  QSignalSpy changed_spy(&scrubber, &PJ::DoubleScrubber::valueChanged);
  QSignalSpy finished_spy(&scrubber, &PJ::ScrubberBase::editingFinished);

  for (const QString& text : {u"nan"_s, u"NaN"_s, u"inf"_s, u"-inf"_s, u"abc"_s}) {
    const QPointF body_local(40, kHeight / 2);
    sendMouse(&scrubber, QEvent::MouseButtonPress, body_local, QPointF(100, 100), Qt::LeftButton, Qt::LeftButton);
    sendMouse(&scrubber, QEvent::MouseButtonRelease, body_local, QPointF(100, 100), Qt::LeftButton, Qt::NoButton);
    auto* editor = scrubber.findChild<QLineEdit*>();
    ASSERT_NE(editor, nullptr);
    editor->setText(text);
    QTest::keyClick(editor, Qt::Key_Return);

    EXPECT_DOUBLE_EQ(scrubber.value(), 2.5) << text.toStdString();
  }
  EXPECT_EQ(changed_spy.count(), 0);
  EXPECT_EQ(finished_spy.count(), 0);

  // The refusals must not have broken the editor: a finite value still commits.
  const QPointF body_local(40, kHeight / 2);
  sendMouse(&scrubber, QEvent::MouseButtonPress, body_local, QPointF(100, 100), Qt::LeftButton, Qt::LeftButton);
  sendMouse(&scrubber, QEvent::MouseButtonRelease, body_local, QPointF(100, 100), Qt::LeftButton, Qt::NoButton);
  auto* editor = scrubber.findChild<QLineEdit*>();
  ASSERT_NE(editor, nullptr);
  editor->setText(u"4.5"_s);
  QTest::keyClick(editor, Qt::Key_Return);
  EXPECT_DOUBLE_EQ(scrubber.value(), 4.5);
  EXPECT_EQ(changed_spy.count(), 1);
  EXPECT_EQ(finished_spy.count(), 1);
}

TEST(ScrubberBase, DoubleScrubberSetValueIgnoresNonFinite) {
  PJ::DoubleScrubber scrubber;
  scrubber.setRange(0.1, 10.0);
  scrubber.setValue(2.5);
  QSignalSpy changed_spy(&scrubber, &PJ::DoubleScrubber::valueChanged);

  for (const double v :
       {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()}) {
    scrubber.setValue(v);
    EXPECT_DOUBLE_EQ(scrubber.value(), 2.5);
  }
  EXPECT_EQ(changed_spy.count(), 0);

  scrubber.setValue(4.0);  // a finite value still goes through
  EXPECT_DOUBLE_EQ(scrubber.value(), 4.0);
  EXPECT_EQ(changed_spy.count(), 1);
}

// A non-finite bound would clamp the value to infinity: the value must stay the finite one it had.
TEST(ScrubberBase, DoubleScrubberValueStaysFiniteWhenABoundIsNot) {
  PJ::DoubleScrubber scrubber;
  const double inf = std::numeric_limits<double>::infinity();
  const auto reset = [&]() {
    scrubber.setRange(0.1, 10.0);
    scrubber.setValue(2.5);
  };

  reset();
  scrubber.setMinimum(inf);
  EXPECT_TRUE(std::isfinite(scrubber.value()));
  reset();
  scrubber.setMaximum(-inf);
  EXPECT_TRUE(std::isfinite(scrubber.value()));
  reset();
  scrubber.setRange(inf, inf);
  EXPECT_TRUE(std::isfinite(scrubber.value()));
}
