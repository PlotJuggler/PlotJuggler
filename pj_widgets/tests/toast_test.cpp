// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QApplication>
#include <QEventLoop>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/ToastManager.h"
#include "pj_widgets/ToastNotification.h"

namespace {

// QSS type selectors match on metaObject()->className(). The toast styling in
// stylesheet_{dark,light}.qss targets `PJ--ToastNotification`, so pin the class
// name: a namespace/rename regression would silently drop the styling instead
// of failing loudly.
TEST(ToastTest, ClassNameDrivesQssSelector) {
  PJ::ToastNotification toast("hello");
  EXPECT_STREQ(toast.metaObject()->className(), "PJ::ToastNotification");
}

// showToast() must create a ToastNotification under the manager's transparent
// container (objectName preserved from PJ3 for stylesheet selectors).
TEST(ToastTest, ShowToastCreatesNotification) {
  QWidget host;
  host.resize(800, 600);
  PJ::ToastManager manager(&host);

  // timeout_ms == 0 disables auto-dismiss so nothing tears down mid-test.
  manager.showToast("plain message", QPixmap(), 0);
  QCoreApplication::processEvents();

  auto* container = host.findChild<QWidget*>("toastManagerContainer");
  ASSERT_NE(container, nullptr);
  EXPECT_NE(host.findChild<PJ::ToastNotification*>(), nullptr);
}

// The message label is rich text with external links enabled — this is what
// makes an `<a href>` in the message open in the system browser with no extra
// wiring (the release-check "View on GitHub" link relies on it).
TEST(ToastTest, MessageLabelIsRichTextWithClickableLink) {
  QWidget host;
  host.resize(800, 600);
  PJ::ToastManager manager(&host);

  const QString message = R"(New release: <a href="https://github.com/PlotJuggler/PJ4">View</a>)";
  manager.showToast(message, QPixmap(), 0);
  QCoreApplication::processEvents();

  auto* toast = host.findChild<PJ::ToastNotification*>();
  ASSERT_NE(toast, nullptr);
  EXPECT_EQ(toast->message(), message);

  auto* label = toast->findChild<QLabel*>("toastMessage");
  ASSERT_NE(label, nullptr);
  EXPECT_EQ(label->textFormat(), Qt::RichText);
  EXPECT_TRUE(label->openExternalLinks());
  EXPECT_TRUE(label->text().contains("github.com/PlotJuggler/PJ4"));
}

// Spin the event loop for `ms` so QPropertyAnimation-driven toast slides can
// run to completion (the test target links no Qt6::Test, so no QTest::qWait).
void pumpEventLoopFor(int ms) {
  QEventLoop loop;
  QTimer::singleShot(ms, &loop, &QEventLoop::quit);
  loop.exec();
}

// The container spans the host's full height (a stable coordinate system for
// the slide/stack animations), so its input region must be masked down to the
// toasts themselves: hit-testing anywhere above the bottom-right stack has to
// pass through to the host, at any point of a toast's lifetime.
TEST(ToastTest, ContainerDoesNotBlockClicksOutsideToasts) {
  QWidget host;
  host.resize(800, 600);
  host.show();
  PJ::ToastManager manager(&host);

  manager.showToast("plain message", QPixmap(), 0);
  QCoreApplication::processEvents();

  auto* container = host.findChild<QWidget*>("toastManagerContainer");
  ASSERT_NE(container, nullptr);
  ASSERT_TRUE(container->isVisible());

  // Top-right corner and the middle of the right edge are both far above the
  // bottom-right toast stack: hit-testing there must not land on the toast
  // overlay or anything inside it, at any point of the toast's lifetime.
  const QPoint top_right(host.width() - 10, 10);
  const QPoint mid_right(host.width() - 10, host.height() / 2);
  for (const QPoint& point : {top_right, mid_right}) {
    QWidget* hit = host.childAt(point);
    EXPECT_TRUE(hit == nullptr || (hit != container && !container->isAncestorOf(hit)))
        << "childAt(" << point.x() << "," << point.y() << ") hit the toast overlay ('"
        << (hit ? hit->objectName().toStdString() : "null") << "') instead of passing through";
  }

  // Same check after the slide-in animation has landed.
  pumpEventLoopFor(500);
  for (const QPoint& point : {top_right, mid_right}) {
    QWidget* hit = host.childAt(point);
    EXPECT_TRUE(hit == nullptr || (hit != container && !container->isAncestorOf(hit)))
        << "childAt(" << point.x() << "," << point.y() << ") hit the toast overlay after the "
        << "slide-in finished";
  }
}

// Restricting the overlay's input region must not overshoot: once the slide-in
// lands, hit-testing over the toast body and its ✕ button still reaches them.
TEST(ToastTest, ToastRemainsClickableInsideInputRegion) {
  QWidget host;
  host.resize(800, 600);
  host.show();
  PJ::ToastManager manager(&host);

  manager.showToast("plain message", QPixmap(), 0);
  QCoreApplication::processEvents();

  auto* toast = host.findChild<PJ::ToastNotification*>();
  ASSERT_NE(toast, nullptr);

  pumpEventLoopFor(500);  // let the 300ms slide-in run to completion

  const QPoint toast_center = toast->mapTo(&host, toast->rect().center());
  QWidget* hit = host.childAt(toast_center);
  ASSERT_NE(hit, nullptr);
  EXPECT_TRUE(hit == toast || toast->isAncestorOf(hit))
      << "childAt over the toast body resolved to '" << (hit ? hit->objectName().toStdString() : "null") << "'";

  auto* close_button = toast->findChild<QPushButton*>("toastCloseButton");
  ASSERT_NE(close_button, nullptr);
  EXPECT_EQ(host.childAt(close_button->mapTo(&host, close_button->rect().center())), close_button);
}

// While a toast is still sliding in, the mask must track its CURRENT geometry:
// the toast is clickable where it is right now, and the final rect it has not
// yet reached stays click-through. This is what the per-toast event filter
// provides over the one-shot mask set in repositionToasts().
TEST(ToastTest, InputRegionTracksSlideAnimation) {
  QWidget host;
  host.resize(800, 600);
  host.show();
  PJ::ToastManager manager(&host);

  manager.showToast("plain message", QPixmap(), 0);
  QCoreApplication::processEvents();

  auto* container = host.findChild<QWidget*>("toastManagerContainer");
  ASSERT_NE(container, nullptr);
  auto* toast = host.findChild<PJ::ToastNotification*>();
  ASSERT_NE(toast, nullptr);

  // The slide only animates x; the landing x is the stack slot repositionToasts
  // computed (right-aligned inside the container with a Section margin).
  const int margin = PJ::theme::space(PJ::theme::Space::Section);
  const int final_x = container->width() - toast->width() - margin;
  const QPoint final_left_edge = container->mapTo(&host, QPoint(final_x + 4, toast->y() + toast->height() / 2));

  pumpEventLoopFor(100);  // mid-flight for the 300ms OutCubic slide

  if (toast->x() <= final_x + 20) {
    GTEST_SKIP() << "slide-in already landed; timing too coarse to probe mid-flight";
  }

  // The not-yet-reached landing area passes through...
  QWidget* hit = host.childAt(final_left_edge);
  EXPECT_TRUE(hit == nullptr || (hit != container && !container->isAncestorOf(hit)))
      << "landing area blocked before the toast arrived (stale one-shot mask)";

  // ...while the toast's currently visible part is already clickable.
  const QRect visible = toast->geometry().intersected(container->rect());
  if (!visible.isEmpty()) {
    QWidget* over_toast = host.childAt(container->mapTo(&host, visible.center()));
    EXPECT_TRUE(over_toast == toast || (over_toast && toast->isAncestorOf(over_toast)))
        << "toast not hit-testable at its in-flight position";
  }
}

// Two stacked toasts mask as the union of their rects; closing one shrinks the
// region (the survivor restacks to the bottom slot and its vacated rect stops
// blocking), and closing the last hides the overlay entirely.
TEST(ToastTest, InputRegionShrinksAsToastsClose) {
  QWidget host;
  host.resize(800, 600);
  host.show();
  PJ::ToastManager manager(&host);

  manager.showToast("first toast message", QPixmap(), 0);
  manager.showToast("second toast message", QPixmap(), 0);
  QCoreApplication::processEvents();
  pumpEventLoopFor(500);  // both slide-ins land

  auto* container = host.findChild<QWidget*>("toastManagerContainer");
  ASSERT_NE(container, nullptr);
  QList<PJ::ToastNotification*> toasts = host.findChildren<PJ::ToastNotification*>();
  ASSERT_EQ(toasts.size(), 2);

  for (PJ::ToastNotification* toast : toasts) {
    QWidget* hit = host.childAt(toast->mapTo(&host, toast->rect().center()));
    EXPECT_TRUE(hit == toast || (hit && toast->isAncestorOf(hit)))
        << "stacked toast not hit-testable through the union mask";
  }

  // Close the lower (newest) toast; the upper one restacks down into its slot.
  PJ::ToastNotification* lower = toasts[0]->y() > toasts[1]->y() ? toasts[0] : toasts[1];
  PJ::ToastNotification* upper = lower == toasts[0] ? toasts[1] : toasts[0];
  const QPoint upper_center_before = upper->mapTo(&host, upper->rect().center());

  lower->hideAnimated();
  pumpEventLoopFor(700);  // slide-out plus the survivor's restack animation

  QWidget* hit = host.childAt(upper_center_before);
  EXPECT_TRUE(hit == nullptr || (hit != container && !container->isAncestorOf(hit)))
      << "rect vacated by the restacked toast still blocks input";
  hit = host.childAt(upper->mapTo(&host, upper->rect().center()));
  EXPECT_TRUE(hit == upper || (hit && upper->isAncestorOf(hit))) << "surviving toast not hit-testable after restacking";

  // Close the last toast: the overlay hides and everything passes through.
  upper->hideAnimated();
  pumpEventLoopFor(700);
  EXPECT_FALSE(container->isVisible());
  EXPECT_EQ(host.childAt(upper_center_before), nullptr);
}

}  // namespace
