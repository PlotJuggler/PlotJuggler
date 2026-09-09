// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QApplication>
#include <QGuiApplication>
#include <QHeaderView>
#include <QImage>
#include <QMouseEvent>
#include <QTableWidget>

#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/HeaderDividerHighlight.h"
#include "pj_widgets/HeaderResizePolicy.h"

namespace PJ {
namespace {

// One QApplication for the whole test binary; QWidget construction requires it.
constexpr int kColumnCount = 3;
constexpr int kColumnWidth = 80;
constexpr int kTableWidth = 300;
constexpr int kTableHeight = 200;
// Comfortably inside a section, far from any grip.
constexpr int kSectionInteriorOffset = 30;
// Beyond any plausible grip margin.
constexpr int kFarFromGrip = 25;

theme::Theme frameworkTheme() {
  const QColor window = QGuiApplication::palette().color(QPalette::Window);
  return theme::themeFor(window.lightness() >= 128);
}

// A table wired exactly the way the app wires one, plus handles onto the pieces
// the assertions need.
struct Fixture {
  QTableWidget table;
  QHeaderView* header;
  QWidget* viewport;
  QWidget* overlay;

  Fixture() : table(0, kColumnCount), header(table.horizontalHeader()) {
    table.resize(kTableWidth, kTableHeight);
    HeaderResizePolicy::install(header, 0, 20);
    HeaderDividerHighlight::install(header);
    for (int i = 0; i < kColumnCount; ++i) {
      header->resizeSection(i, kColumnWidth);
    }
    viewport = header->viewport();
    // The highlighter's line is the only widget it parents to the viewport.
    const auto children = viewport->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
    overlay = children.isEmpty() ? nullptr : children.first();
  }

  /// Right edge of @p section in viewport coordinates — where its grip sits.
  [[nodiscard]] int edgeOf(int section) const {
    return header->sectionViewportPosition(section) + header->sectionSize(section);
  }

  void sendMouse(QEvent::Type type, int x, Qt::MouseButton button = Qt::NoButton) {
    const QPointF pos(x, header->height() / 2.0);
    QMouseEvent event(type, pos, pos, button, button, Qt::NoModifier);
    QApplication::sendEvent(viewport, &event);
  }

  /// The colour the overlay actually paints, read back off a render of it.
  [[nodiscard]] QColor paintedColor() const {
    return overlay->grab().toImage().pixelColor(0, 0);
  }
};

TEST(HeaderDividerHighlight, InstallsExactlyOneOverlayAndIsIdempotent) {
  Fixture f;
  ASSERT_NE(f.overlay, nullptr);
  EXPECT_EQ(HeaderDividerHighlight::install(f.header), HeaderDividerHighlight::install(f.header));
  EXPECT_EQ(f.viewport->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly).size(), 1);
}

TEST(HeaderDividerHighlight, HiddenUntilThePointerReachesAGrip) {
  Fixture f;
  EXPECT_FALSE(f.overlay->isVisibleTo(f.viewport));

  f.sendMouse(QEvent::MouseMove, f.header->sectionViewportPosition(0) + kSectionInteriorOffset);
  EXPECT_FALSE(f.overlay->isVisibleTo(f.viewport)) << "hovering a section body must not light the divider";
}

TEST(HeaderDividerHighlight, HoverLightsTheGrippedBoundaryInHighlightHovered) {
  Fixture f;
  f.sendMouse(QEvent::MouseMove, f.edgeOf(0));

  ASSERT_TRUE(f.overlay->isVisibleTo(f.viewport));
  // A hairline sitting on the section's own border-right pixel, starting one row
  // down so the section's border-top rule stays unbroken across the band.
  EXPECT_EQ(f.overlay->geometry(), QRect(f.edgeOf(0) - 1, 1, 1, f.viewport->height() - 1));
  EXPECT_EQ(f.paintedColor(), theme::interaction(theme::Variant::Highlight, theme::State::Hovered, frameworkTheme()));
}

TEST(HeaderDividerHighlight, PressDeepensToHighlightPressed) {
  Fixture f;
  f.sendMouse(QEvent::MouseMove, f.edgeOf(0));
  const QColor hovered = f.paintedColor();

  f.sendMouse(QEvent::MouseButtonPress, f.edgeOf(0), Qt::LeftButton);
  ASSERT_TRUE(f.overlay->isVisibleTo(f.viewport));
  const QColor pressed = f.paintedColor();

  EXPECT_EQ(pressed, theme::interaction(theme::Variant::Highlight, theme::State::Pressed, frameworkTheme()));
  EXPECT_NE(pressed, hovered) << "pressed must be visually distinct from hovered";
}

TEST(HeaderDividerHighlight, HighlightSurvivesThePointerLeavingDuringADrag) {
  Fixture f;
  f.sendMouse(QEvent::MouseMove, f.edgeOf(0));
  f.sendMouse(QEvent::MouseButtonPress, f.edgeOf(0), Qt::LeftButton);

  // A resize drag routinely runs the pointer off the grip, and out of the
  // header entirely; the divider must stay lit until the button comes up.
  f.sendMouse(QEvent::MouseMove, f.edgeOf(0) + kFarFromGrip);
  EXPECT_TRUE(f.overlay->isVisibleTo(f.viewport));
  QEvent leave(QEvent::Leave);
  QApplication::sendEvent(f.viewport, &leave);
  EXPECT_TRUE(f.overlay->isVisibleTo(f.viewport));
}

TEST(HeaderDividerHighlight, ReleaseAwayFromAGripClearsTheHighlight) {
  Fixture f;
  f.sendMouse(QEvent::MouseMove, f.edgeOf(0));
  f.sendMouse(QEvent::MouseButtonPress, f.edgeOf(0), Qt::LeftButton);
  f.sendMouse(QEvent::MouseButtonRelease, f.edgeOf(0) + kFarFromGrip, Qt::LeftButton);

  EXPECT_FALSE(f.overlay->isVisibleTo(f.viewport));
}

TEST(HeaderDividerHighlight, TheLastSectionsEdgeIsNotAdvertisedAsResizable) {
  Fixture f;
  // HeaderResizePolicy snaps the last section back — there is no column after it
  // to trade width with — so lighting its edge would promise a drag that no-ops.
  f.sendMouse(QEvent::MouseMove, f.edgeOf(kColumnCount - 1));
  EXPECT_FALSE(f.overlay->isVisibleTo(f.viewport));
}

TEST(HeaderDividerHighlight, HiddenSectionsAreSkippedWhenLocatingTheGrip) {
  Fixture f;
  f.header->hideSection(1);
  // With column 1 hidden, column 0's right edge now abuts column 2, so it stays
  // a real boundary; column 2 is last, so its edge stays inert.
  f.sendMouse(QEvent::MouseMove, f.edgeOf(0));
  EXPECT_TRUE(f.overlay->isVisibleTo(f.viewport));

  f.sendMouse(QEvent::MouseMove, f.edgeOf(2));
  EXPECT_FALSE(f.overlay->isVisibleTo(f.viewport));
}

}  // namespace
}  // namespace PJ
