// SPDX-License-Identifier: MPL-2.0
#include "pj_widgets/Scrollbar.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QHeaderView>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTableWidget>
#include <QTest>
#include <QWheelEvent>

#include "pj_widgets/FrameworkTokens.h"
using namespace Qt::StringLiterals;

using PJ::scrollbar_detail::computeHandle;
using PJ::scrollbar_detail::defaultAccent;
using PJ::scrollbar_detail::Handle;
using PJ::scrollbar_detail::valueForDrag;

// ---------------------------------------------------------------------------
// Headless math tests — no QApplication needed.
// ---------------------------------------------------------------------------

TEST(ScrollbarHandle, FullRangeStartsAtZero) {
  // 0..100, page 100, value 0 over a 200px track -> full-length handle at 0.
  Handle h = computeHandle(0, 100, 0, 100, 200.0);
  EXPECT_NEAR(h.pos, 0.0, 1e-6);
  EXPECT_NEAR(h.len, 100.0, 1e-6);  // page/(range+page)=100/200 * 200
}

TEST(ScrollbarHandle, MidValueOffsetsHandle) {
  Handle h = computeHandle(0, 100, 50, 100, 200.0);
  EXPECT_NEAR(h.len, 100.0, 1e-6);
  EXPECT_NEAR(h.pos, 50.0, 1e-6);  // frac 50/200 * 200, clamped within track
}

TEST(ScrollbarHandle, TinyPageFloorsToMinLen) {
  Handle h = computeHandle(0, 10000, 0, 1, 200.0);
  EXPECT_GE(h.len, 28.0);  // kMinPillLenPx floor keeps it grabbable
}

TEST(ScrollbarHandle, NothingToScrollIsEmpty) {
  Handle h = computeHandle(0, 0, 0, 100, 200.0);
  EXPECT_NEAR(h.len, 0.0, 1e-6);
}

TEST(ScrollbarHandle, ZeroPageStepIsEmpty) {
  // Degenerate: max>min but pageStep 0 (e.g. a collapsed viewport) → no real
  // handle, so paint nothing instead of flooring a phantom kMinPillLenPx pill.
  Handle h = computeHandle(0, 100, 0, 0, 200.0);
  EXPECT_NEAR(h.len, 0.0, 1e-6);
}

// ---------------------------------------------------------------------------
// defaultAccent — theme-aware pill color keyed off QPalette::Window lightness
// (NOT the system QPalette::Highlight, which the app never sets). No QApplication
// needed.
// ---------------------------------------------------------------------------

TEST(ScrollbarAccent, DarkWindowYieldsFrameworkScrollHandle) {
  // A dark chrome (low lightness) resolves to the framework Scroll Handle color
  // for the dark theme, keyed off window lightness — never the OS highlight.
  EXPECT_EQ(
      defaultAccent(QColor(0x2B, 0x2B, 0x33)),
      PJ::theme::surface(PJ::theme::Surface::ScrollHandle, PJ::theme::Theme::Dark));
}

TEST(ScrollbarAccent, LightWindowYieldsFrameworkScrollHandle) {
  EXPECT_EQ(
      defaultAccent(QColor(0xF5, 0xF5, 0xF5)),
      PJ::theme::surface(PJ::theme::Surface::ScrollHandle, PJ::theme::Theme::Light));
}

// ---------------------------------------------------------------------------
// valueForDrag math tests — no QApplication needed.
// ---------------------------------------------------------------------------

TEST(ScrollbarDrag, HalfTrackDeltaMapsToHalfTotal) {
  // delta of half the track (100px) over total=200, start=0 → value moves by 100.
  EXPECT_EQ(valueForDrag(0, 100.0, 200.0, 200), 100);
}

TEST(ScrollbarDrag, ZeroTrackLenReturnsStart) {
  EXPECT_EQ(valueForDrag(50, 10.0, 0.0, 200), 50);
}

TEST(ScrollbarDrag, NegativeDeltaScrollsBack) {
  EXPECT_EQ(valueForDrag(100, -50.0, 200.0, 200), 50);
}

TEST(ScrollbarDrag, NonZeroStartValueOffsets) {
  // start=10, delta 1/4 track, total=100 → start + 25 = 35.
  EXPECT_EQ(valueForDrag(10, 50.0, 200.0, 100), 35);
}

// ---------------------------------------------------------------------------
// Widget tests — require QApplication.
// ---------------------------------------------------------------------------

// One QApplication for the whole binary; QWidget construction requires it.

// ---------------------------------------------------------------------------
// Config tests — auto-hide + fade duration.
// ---------------------------------------------------------------------------

TEST(ScrollbarConfig, AutoHideOffStaysVisible) {
  QScrollArea area;
  area.setWidget(new QWidget);
  area.widget()->setFixedSize(2000, 2000);
  area.resize(200, 200);
  area.show();

  PJ::Scrollbar bar(Qt::Vertical);
  bar.attach(&area);
  bar.setAutoHide(false);
  QTest::qWait(50);
  EXPECT_DOUBLE_EQ(bar.currentOpacity(), 1.0);
}

TEST(ScrollbarConfig, InstantFadeWhenDurationZero) {
  QScrollArea area;
  area.setWidget(new QWidget);
  area.widget()->setFixedSize(2000, 2000);
  area.resize(200, 200);
  area.show();

  PJ::Scrollbar bar(Qt::Vertical);
  bar.setFadeDurationMs(0);
  bar.attach(&area);

  // (1) The configured duration must propagate to the underlying animation.
  EXPECT_EQ(bar.fadeDurationMs(), 0);

  // (2) With duration=0 a show must complete synchronously — opacity reaches
  // 1.0 within the setAutoHide(false) call frame, with NO qWait() in between.
  // This exercises the instant-snap code path in setShown(): with fade_ms_==0
  // the opacity_effect_ is set directly rather than scheduling an animation
  // timer tick (QPropertyAnimation defers even duration=0 to the event loop).
  bar.setAutoHide(false);
  EXPECT_DOUBLE_EQ(bar.currentOpacity(), 1.0);
}

TEST(ScrollbarWidget, AttachEnablesViewportMouseTracking) {
  // Without mouse tracking the viewport only delivers MouseMove while a button
  // is held, so the pill never fades in on a plain hover (it only appears on
  // click and never re-hides on intra-viewport motion). attach() must enable
  // tracking itself rather than relying on the host having done so (the Timeline
  // happens to; a generic plugin scroll area does not).
  QScrollArea area;
  area.setWidget(new QWidget);
  area.widget()->setFixedSize(2000, 2000);
  area.resize(200, 200);
  area.show();

  ASSERT_FALSE(area.viewport()->hasMouseTracking()) << "precondition: viewport starts without tracking";

  PJ::Scrollbar bar(Qt::Vertical);
  bar.attach(&area);

  EXPECT_TRUE(area.viewport()->hasMouseTracking())
      << "attach() must enable viewport mouse tracking so hover-reveal works";
}

TEST(ScrollbarWidget, HoverOverCoveringChildRevealsPill) {
  // When a child widget fully covers the viewport (e.g. a QScrollArea packed with
  // marketplace cards), hover MouseMoves are delivered to that child, not the
  // viewport — so attach() must observe the covering subtree, otherwise the pill
  // only ever reveals on scroll/click. Here the content widget is the cover.
  QScrollArea area;
  auto* content = new QWidget;
  content->setFixedSize(2000, 2000);  // both axes overflow; content covers the viewport
  area.setWidget(content);
  area.resize(200, 200);
  area.show();

  PJ::Scrollbar v(Qt::Vertical);
  v.attach(&area);
  ASSERT_FALSE(v.isShown());

  // Content sits at the scroll origin, so a content-local point equals its
  // viewport point; pick one inside the right-edge strip.
  const int vp_w = area.viewport()->width();
  const QPoint local(vp_w - 5, 100);
  QMouseEvent move(
      QEvent::MouseMove, QPointF(local), content->mapToGlobal(local), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
  QCoreApplication::sendEvent(content, &move);

  EXPECT_TRUE(v.isShown()) << "hover over a covering child, inside the strip, must reveal the pill";
}

TEST(ScrollbarWidget, CoveringChildAddedAfterAttachIsObserved) {
  // Cards arrive asynchronously (the marketplace fetches its registry after the
  // window is built), so a child added AFTER attach() must be picked up via
  // ChildAdded and observed for hover too.
  QScrollArea area;
  auto* content = new QWidget;
  content->setFixedSize(2000, 2000);
  area.setWidget(content);
  area.resize(200, 200);
  area.show();

  PJ::Scrollbar v(Qt::Vertical);
  v.attach(&area);

  // A card added under the content widget after attach.
  auto* card = new QWidget(content);
  card->setFixedSize(2000, 100);
  card->move(0, 0);
  QCoreApplication::processEvents();  // let the ChildAdded reach the observer

  ASSERT_FALSE(v.isShown());
  const int vp_w = area.viewport()->width();
  const QPoint local(vp_w - 5, 40);  // on the card, inside the strip
  QMouseEvent move(
      QEvent::MouseMove, QPointF(local), card->mapToGlobal(local), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
  QCoreApplication::sendEvent(card, &move);

  EXPECT_TRUE(v.isShown()) << "a card added after attach must still reveal the pill on hover";
}

TEST(ScrollbarWidget, WheelScrollRevealsMatchingAxisOnly) {
  // A scroll (wheel / trackpad gesture) reveals the pill even when the cursor is
  // nowhere near the strip — and only the axis that actually scrolled reveals.
  QScrollArea area;
  area.setWidget(new QWidget);
  area.widget()->setFixedSize(2000, 2000);  // both axes have something to scroll
  area.resize(200, 200);
  area.show();

  PJ::Scrollbar h(Qt::Horizontal);
  h.attach(&area);
  PJ::Scrollbar v(Qt::Vertical);
  v.attach(&area);
  ASSERT_FALSE(h.isShown());
  ASSERT_FALSE(v.isShown());

  // A purely-vertical wheel over the middle of the viewport (not the strip).
  const QPointF pos(100, 100);
  const QPointF global = area.viewport()->mapToGlobal(pos);
  QWheelEvent wheel(
      pos, global, QPoint(0, 0), QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, /*inverted=*/false);
  QCoreApplication::sendEvent(area.viewport(), &wheel);

  EXPECT_TRUE(v.isShown()) << "vertical wheel must reveal the vertical pill";
  EXPECT_FALSE(h.isShown()) << "vertical wheel must NOT reveal the horizontal pill";
}

TEST(ScrollbarWidget, ScrollRevealSurvivesHoverOut) {
  // Regression: a scroll reveals the pill, then a hover-out (e.g. the synthetic
  // MouseMove Qt delivers as scrolled content slides under a stationary cursor,
  // now that attach() enables mouse tracking) must NOT cancel the reveal — the
  // hold timer owns the hide. Previously this hid the pill instantly on scroll.
  QScrollArea area;
  area.setWidget(new QWidget);
  area.widget()->setFixedSize(2000, 2000);
  area.resize(200, 200);
  area.show();

  PJ::Scrollbar v(Qt::Vertical);
  v.attach(&area);

  const QPointF mid(100, 100);  // in the content, NOT the right-edge strip
  const QPointF global = area.viewport()->mapToGlobal(mid);
  QWheelEvent wheel(
      mid, global, QPoint(0, 0), QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, /*inverted=*/false);
  QCoreApplication::sendEvent(area.viewport(), &wheel);
  ASSERT_TRUE(v.isShown()) << "scroll must reveal the pill";

  // A hover move in the middle (outside the strip) while the reveal hold is active.
  QMouseEvent move(QEvent::MouseMove, mid, global, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
  QCoreApplication::sendEvent(area.viewport(), &move);

  EXPECT_TRUE(v.isShown()) << "a hover-out must not cancel an active scroll reveal";
}

TEST(ScrollbarWidget, PressOffHandleDoesNotScrollOrGrab) {
  // Regression: only the visible handle is a drag target. A press in the edge
  // strip but off the handle must fall through to the view — it must NOT be
  // consumed as a click-to-center jump (which would both steal the content click
  // and move the scroll position).
  QScrollArea area;
  area.setWidget(new QWidget);
  area.widget()->setFixedSize(2000, 2000);
  area.resize(200, 200);
  area.show();

  PJ::Scrollbar v(Qt::Vertical);
  v.attach(&area);
  QScrollBar* bar = area.verticalScrollBar();
  bar->setValue(bar->minimum());  // handle sits at the top of the track

  // Press mid-strip — in the right-edge strip, off the handle (which is at the
  // top), and clear of the bottom-right corner (yielded to the horizontal pill).
  const QPointF pos(area.viewport()->width() - 3, area.viewport()->height() / 2);
  const QPointF global = area.viewport()->mapToGlobal(pos);
  QMouseEvent press(QEvent::MouseButtonPress, pos, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QCoreApplication::sendEvent(area.viewport(), &press);

  EXPECT_EQ(bar->value(), bar->minimum()) << "an off-handle press must not jump the scroll value";
}

TEST(ScrollbarWidget, ClickToScrollJumpsOnOffHandlePress) {
  // With click-to-scroll opted in (the Timeline's mode), an off-handle press in
  // the strip centers the handle under the cursor, jumping the scroll value.
  QScrollArea area;
  area.setWidget(new QWidget);
  area.widget()->setFixedSize(2000, 2000);
  area.resize(200, 200);
  area.show();

  PJ::Scrollbar v(Qt::Vertical);
  v.setClickToScroll(true);
  v.attach(&area);
  QScrollBar* bar = area.verticalScrollBar();
  bar->setValue(bar->minimum());  // handle at the top

  // Mid-strip, off the handle, clear of the yielded bottom-right corner.
  const QPointF pos(area.viewport()->width() - 3, area.viewport()->height() / 2);
  const QPointF global = area.viewport()->mapToGlobal(pos);
  QMouseEvent press(QEvent::MouseButtonPress, pos, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QCoreApplication::sendEvent(area.viewport(), &press);

  EXPECT_GT(bar->value(), bar->minimum()) << "click-to-scroll must jump toward an off-handle press";
}

TEST(ScrollbarWidget, WheelDoesNotRevealWhenLocked) {
  // A locked (non-interactive) scrollbar must not reveal on scroll — the view is
  // frozen, so a flashing pill would be a misleading affordance (finding #6).
  QScrollArea area;
  area.setWidget(new QWidget);
  area.widget()->setFixedSize(2000, 2000);
  area.resize(200, 200);
  area.show();

  PJ::Scrollbar v(Qt::Vertical);
  v.attach(&area);
  v.setInteractive(false);

  const QPointF pos(100, 100);
  QWheelEvent wheel(
      pos, area.viewport()->mapToGlobal(pos), QPoint(0, 0), QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
      Qt::NoScrollPhase, /*inverted=*/false);
  QCoreApplication::sendEvent(area.viewport(), &wheel);

  EXPECT_FALSE(v.isShown()) << "a locked scrollbar must not reveal on scroll";
}

TEST(ScrollbarWidget, LockRetreatsShownPill) {
  // Locking mid-reveal must retreat the pill, not leave it stuck visible over the
  // frozen view (finding #8).
  QScrollArea area;
  area.setWidget(new QWidget);
  area.widget()->setFixedSize(2000, 2000);
  area.resize(200, 200);
  area.show();

  PJ::Scrollbar v(Qt::Vertical);
  v.attach(&area);

  const QPointF pos(100, 100);
  QWheelEvent wheel(
      pos, area.viewport()->mapToGlobal(pos), QPoint(0, 0), QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
      Qt::NoScrollPhase, /*inverted=*/false);
  QCoreApplication::sendEvent(area.viewport(), &wheel);
  ASSERT_TRUE(v.isShown());

  v.setInteractive(false);
  EXPECT_FALSE(v.isShown()) << "locking must retreat a shown pill";
}

TEST(ScrollbarWidget, VerticalStripYieldsBottomRightCornerToHorizontal) {
  // The H and V strips overlap in the bottom-right corner; the vertical pill must
  // yield it so the horizontal pill's right end stays reachable (finding #4).
  QScrollArea area;
  area.setWidget(new QWidget);
  area.widget()->setFixedSize(2000, 2000);  // both axes scroll
  area.resize(200, 200);
  area.show();

  PJ::Scrollbar h(Qt::Horizontal);
  h.attach(&area);
  PJ::Scrollbar v(Qt::Vertical);
  v.attach(&area);

  const int w = area.viewport()->width();
  const int ht = area.viewport()->height();
  const QPointF corner(w - 3, ht - 3);
  QMouseEvent move(
      QEvent::MouseMove, corner, area.viewport()->mapToGlobal(corner), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
  QCoreApplication::sendEvent(area.viewport(), &move);

  EXPECT_TRUE(h.isShown()) << "the bottom-right corner reveals the horizontal pill";
  EXPECT_FALSE(v.isShown()) << "the vertical pill yields the bottom-right corner";
}

// ---------------------------------------------------------------------------
// Reserved-gutter placement — attachPillScrollbars' mode: the native bar keeps
// its policy and layout slot (content never sits under the pill) and owns all
// interaction; the pill is a paint-only mirror.
// ---------------------------------------------------------------------------

namespace {

// A 200x200 scroll area over 2000x2000 content with a trailing-edge button —
// the "trash button at the right edge of a row" shape. The button hugs the
// viewport's right edge (placed AFTER attach, since gutter mode narrows the
// viewport) with its center inside the hover strip and, with the scroll at
// minimum, within the pill handle's span.
// `pill` is declared after `area` on purpose: attach() reparents the pill into
// the area, so the pill member must be destroyed first (reverse declaration
// order) or the area's child cleanup would delete it a second time.
struct EdgeButtonArea {
  QScrollArea area;
  PJ::Scrollbar pill;
  QPushButton* button = nullptr;
  int clicks = 0;

  explicit EdgeButtonArea(PJ::Scrollbar::Placement placement) : pill(Qt::Vertical) {
    auto* content = new QWidget;
    content->setFixedSize(2000, 2000);
    area.setWidget(content);
    area.resize(200, 200);
    area.show();
    pill.attach(&area, placement);
    QTest::qWait(50);  // let the AsNeeded bar appear and the layout settle

    const int vp_w = area.viewport()->width();
    button = new QPushButton(content);
    button->setGeometry(vp_w - 14, 0, 14, 20);
    button->show();
    QObject::connect(button, &QPushButton::clicked, button, [this] { ++clicks; });
    QCoreApplication::processEvents();  // deliver ChildAdded to the hover observer

    // Hover the button so the pill is revealed first — the overlay consume path
    // only triggers on a shown pill, matching a user who can see it.
    const QPoint center = button->rect().center();
    QMouseEvent move(
        QEvent::MouseMove, QPointF(center), button->mapToGlobal(center), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(button, &move);
  }
};

}  // namespace

TEST(ScrollbarGutter, KeepsPolicyAndReservesGutter) {
  QScrollArea area;
  area.setWidget(new QWidget);
  area.widget()->setFixedSize(2000, 2000);
  area.resize(200, 200);
  area.show();

  PJ::Scrollbar pill(Qt::Vertical);
  pill.attach(&area, PJ::Scrollbar::Placement::kReservedGutter);
  QTest::qWait(50);  // let the AsNeeded bar appear and the layout settle

  EXPECT_EQ(area.verticalScrollBarPolicy(), Qt::ScrollBarAsNeeded) << "gutter mode must not touch the axis policy";
  QScrollBar* native = area.verticalScrollBar();
  EXPECT_TRUE(native->isVisible()) << "the native bar keeps reserving its gutter";
  EXPECT_LE(area.viewport()->width() + native->width(), area.width())
      << "viewport and gutter must partition the width — content never under the pill";
}

TEST(ScrollbarGutter, EdgeContentClickReachesChild) {
  EdgeButtonArea fixture(PJ::Scrollbar::Placement::kReservedGutter);

  QTest::mouseClick(fixture.button, Qt::LeftButton);

  EXPECT_EQ(fixture.clicks, 1) << "gutter mode must never consume a content click";
}

TEST(ScrollbarGutter, OverlayPlacementStealsSameClickOnHandle) {
  // The contrast pin: in overlay placement the same edge button sits under the
  // revealed pill handle, whose press-grab wins. This is the documented
  // trade-off that reserves overlay placement for viewport-owning hosts (the
  // Timeline) — content views get the gutter.
  EdgeButtonArea fixture(PJ::Scrollbar::Placement::kOverlayViewport);
  ASSERT_TRUE(fixture.pill.isShown()) << "precondition: the hover revealed the pill";

  QTest::mouseClick(fixture.button, Qt::LeftButton);

  EXPECT_EQ(fixture.clicks, 0);
}

TEST(ScrollbarGutter, NativeBarStillPagesOnTrackPress) {
  QScrollArea area;
  area.setWidget(new QWidget);
  area.widget()->setFixedSize(2000, 2000);
  area.resize(200, 200);
  area.show();

  PJ::Scrollbar pill(Qt::Vertical);
  pill.attach(&area, PJ::Scrollbar::Placement::kReservedGutter);
  QTest::qWait(50);  // let the AsNeeded bar appear and the layout settle

  QScrollBar* native = area.verticalScrollBar();
  native->setValue(native->minimum());

  // Press the (blank) track well below the handle: the native bar pages down —
  // proof the bar still owns interaction; the pill only observes.
  const QPoint below_handle(native->width() / 2, native->height() - 5);
  QTest::mouseClick(native, Qt::LeftButton, Qt::NoModifier, below_handle);

  EXPECT_GT(native->value(), native->minimum()) << "a track press must still page-scroll via the native bar";
}

TEST(ScrollbarGutter, PillCoversBarAndMirrorsHandle) {
  QScrollArea area;
  area.setWidget(new QWidget);
  area.widget()->setFixedSize(2000, 2000);
  area.resize(200, 200);
  area.show();

  PJ::Scrollbar pill(Qt::Vertical);
  pill.attach(&area, PJ::Scrollbar::Placement::kReservedGutter);
  QTest::qWait(50);  // let the AsNeeded bar appear and the layout settle

  QScrollBar* native = area.verticalScrollBar();
  native->setValue(native->maximum());
  QCoreApplication::processEvents();

  EXPECT_EQ(pill.geometry(), QRect(native->mapTo(&area, QPoint(0, 0)), native->size()))
      << "the pill must cover exactly the native bar's gutter";
  EXPECT_GT(pill.handleLenPx(), 0.0);
  EXPECT_GT(pill.handlePosPx(), 0.0) << "at max scroll the mirrored handle sits away from the origin";
  EXPECT_LE(pill.handlePosPx() + pill.handleLenPx(), static_cast<double>(native->height()) + 0.5)
      << "the mirrored handle stays inside the bar";
}

TEST(ScrollbarGutter, NoGutterOrRevealWhenNothingToScroll) {
  QScrollArea area;
  area.setWidget(new QWidget);
  area.widget()->setFixedSize(100, 100);  // fits: nothing to scroll
  area.resize(200, 200);
  area.show();

  PJ::Scrollbar pill(Qt::Vertical);
  pill.attach(&area, PJ::Scrollbar::Placement::kReservedGutter);
  QTest::qWait(50);  // let the AsNeeded bar appear and the layout settle

  EXPECT_FALSE(area.verticalScrollBar()->isVisible()) << "AsNeeded reserves no gutter when content fits";
  EXPECT_DOUBLE_EQ(pill.handleLenPx(), 0.0);

  const QPointF mid(50, 50);
  QWheelEvent wheel(
      mid, area.viewport()->mapToGlobal(mid), QPoint(0, 0), QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
      Qt::NoScrollPhase, /*inverted=*/false);
  QCoreApplication::sendEvent(area.viewport(), &wheel);
  EXPECT_FALSE(pill.isShown()) << "no reveal when there is nothing to scroll";
}

TEST(ScrollbarGutter, HoverOnGutterRevealsAndLeaveHides) {
  QScrollArea area;
  area.setWidget(new QWidget);
  area.widget()->setFixedSize(2000, 2000);
  area.resize(200, 200);
  area.show();

  PJ::Scrollbar pill(Qt::Vertical);
  pill.attach(&area, PJ::Scrollbar::Placement::kReservedGutter);
  QTest::qWait(50);  // let the AsNeeded bar appear and the layout settle
  ASSERT_FALSE(pill.isShown());

  // A plain hover: Qt discards buttonless MouseMoves for a non-tracking widget
  // before object filters run, so what the pill's filter actually receives is
  // the HoverMove synthesized via the bar's WA_Hover — send the move through
  // sendEvent and rely on that synthesis, exactly like a real cursor.
  QScrollBar* native = area.verticalScrollBar();
  const QPoint on_bar(native->width() / 2, native->height() / 2);
  QMouseEvent move(
      QEvent::MouseMove, QPointF(on_bar), native->mapToGlobal(on_bar), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
  QCoreApplication::sendEvent(native, &move);
  EXPECT_TRUE(pill.isShown()) << "hovering the gutter must reveal the pill";

  QEvent leave(QEvent::Leave);
  QCoreApplication::sendEvent(native, &leave);
  EXPECT_FALSE(pill.isShown()) << "leaving the gutter must hide the pill";
}

TEST(ScrollbarGutter, HeaderBandPaintedAndTrackInsetBelowHeader) {
  // Item views: the gutter spans the full edge, so its top segment runs beside
  // the horizontal header. The groove must be inset below the header (so the
  // handle never climbs into the band) and the blanked bar must paint that band
  // as the header's continuation — otherwise it reads as a hole in the header.
  QTableWidget table(60, 2);
  table.setHorizontalHeaderLabels({u"Name"_s, u"Value"_s});
  table.resize(250, 200);
  table.show();

  PJ::Scrollbar pill(Qt::Vertical);
  pill.attach(&table, PJ::Scrollbar::Placement::kReservedGutter);
  QTest::qWait(50);  // let the AsNeeded bar appear and the layout settle

  QScrollBar* native = table.verticalScrollBar();
  ASSERT_TRUE(native->isVisible());
  const int header_h = table.horizontalHeader()->height();
  ASSERT_GT(header_h, 0);

  native->setValue(native->minimum());
  QCoreApplication::processEvents();
  EXPECT_GE(pill.handlePosPx(), static_cast<double>(header_h))
      << "at scroll-top the mirrored handle must start below the header band";

  // Render the bar without the window-background pre-fill: only what the Paint
  // interception paints lands on the sentinel canvas — the header band must be
  // the framework Backdrop, everything below it must stay blank (sentinel).
  QImage canvas(native->size(), QImage::Format_ARGB32);
  const QColor sentinel(255, 0, 0);
  canvas.fill(sentinel);
  QPainter canvas_painter(&canvas);
  native->render(&canvas_painter, QPoint(), QRegion(), QWidget::DrawChildren);
  canvas_painter.end();
  const PJ::theme::Theme band_theme =
      PJ::theme::themeFor(QGuiApplication::palette().window().color().lightness() >= 128);
  EXPECT_EQ(
      canvas.pixelColor(native->width() / 2, header_h / 2),
      PJ::theme::surface(PJ::theme::Surface::Backdrop, band_theme))
      << "the band beside the header must paint the framework Backdrop";
  EXPECT_EQ(canvas.pixelColor(native->width() / 2, header_h + 20), sentinel)
      << "below the band the blanked bar must paint nothing";
}

TEST(ScrollbarGutter, NoHeaderInsetForPlainScrollArea) {
  QScrollArea area;
  area.setWidget(new QWidget);
  area.widget()->setFixedSize(2000, 2000);
  area.resize(200, 200);
  area.show();

  PJ::Scrollbar pill(Qt::Vertical);
  pill.attach(&area, PJ::Scrollbar::Placement::kReservedGutter);
  QTest::qWait(50);  // let the AsNeeded bar appear and the layout settle

  QScrollBar* native = area.verticalScrollBar();
  native->setValue(native->minimum());
  QCoreApplication::processEvents();
  EXPECT_LE(pill.handlePosPx(), 1.0) << "no header — the groove must start at the gutter top";

  QImage canvas(native->size(), QImage::Format_ARGB32);
  const QColor sentinel(255, 0, 0);
  canvas.fill(sentinel);
  QPainter canvas_painter(&canvas);
  native->render(&canvas_painter, QPoint(), QRegion(), QWidget::DrawChildren);
  canvas_painter.end();
  EXPECT_EQ(canvas.pixelColor(native->width() / 2, 5), sentinel)
      << "no header — the blanked bar must paint nothing at all";
}

TEST(ScrollbarGutter, WheelOverViewportRevealsPill) {
  QScrollArea area;
  area.setWidget(new QWidget);
  area.widget()->setFixedSize(2000, 2000);
  area.resize(200, 200);
  area.show();

  PJ::Scrollbar pill(Qt::Vertical);
  pill.attach(&area, PJ::Scrollbar::Placement::kReservedGutter);
  QTest::qWait(50);  // let the AsNeeded bar appear and the layout settle
  ASSERT_FALSE(pill.isShown());

  const QPointF mid(100, 100);
  QWheelEvent wheel(
      mid, area.viewport()->mapToGlobal(mid), QPoint(0, 0), QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
      Qt::NoScrollPhase, /*inverted=*/false);
  QCoreApplication::sendEvent(area.viewport(), &wheel);

  EXPECT_TRUE(pill.isShown()) << "a wheel scroll must reveal the gutter pill";
}

TEST(ScrollbarWidget, AttachHidesNativeAndTracksRange) {
  QScrollArea area;
  area.setWidget(new QWidget);
  area.widget()->setFixedSize(2000, 2000);
  area.resize(200, 200);
  area.show();

  PJ::Scrollbar bar(Qt::Vertical);
  bar.attach(&area);

  EXPECT_EQ(area.verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);

  // Scroll to the bottom so the pill should be positioned toward the end.
  QScrollBar* native = area.verticalScrollBar();
  native->setValue(native->maximum());

  // computeHandle with the live bar state must produce a non-empty handle
  // positioned away from the origin — confirming the overlay tracks the bar.
  const double track_len = static_cast<double>(area.viewport()->height());
  const Handle h = computeHandle(native->minimum(), native->maximum(), native->value(), native->pageStep(), track_len);

  EXPECT_GT(h.len, 0.0) << "handle must be non-empty when there is content to scroll";
  EXPECT_GT(h.pos, 0.0) << "handle must be positioned toward the end after scrolling to max";
}
