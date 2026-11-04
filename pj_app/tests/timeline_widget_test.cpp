// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QLineEdit>
#include <QTest>
#include <cmath>

#include "pj_runtime/PlaybackEngine.h"
#include "pj_widgets/DoubleScrubber.h"
#include "pj_widgets/RealSlider.h"
#include "ui/TimelineWidget.h"

namespace PJ {
namespace {

TEST(TimelineWidgetTest, SpaceTogglesPlaybackButRemainsTextInAnEditor) {
  PlaybackEngine engine;
  TimelineWidget widget;
  widget.setPlaybackEngine(&engine);
  engine.setRange({displaySeconds(0), displaySeconds(100)});
  widget.show();
  widget.activateWindow();
  QApplication::processEvents();
  QTest::keyClick(&widget, Qt::Key_Space);
  EXPECT_TRUE(engine.isPlaying());
  QTest::keyClick(&widget, Qt::Key_Space);
  EXPECT_FALSE(engine.isPlaying());

  QLineEdit editor(&widget);
  editor.show();
  editor.setFocus();
  QTest::keyClick(&editor, Qt::Key_Space);
  EXPECT_EQ(editor.text(), QStringLiteral(" "));
  EXPECT_FALSE(engine.isPlaying());
}

TEST(TimelineWidgetTest, ClickingTrackSeeksToClickAndCanContinueDragging) {
  PlaybackEngine engine;
  TimelineWidget widget;
  widget.resize(800, widget.sizeHint().height());
  widget.setPlaybackEngine(&engine);
  engine.setRange({displaySeconds(0), displaySeconds(100)});
  widget.show();
  QApplication::processEvents();
  auto* slider = widget.findChild<RealSlider*>("timeSlider");
  ASSERT_NE(slider, nullptr);
  const QPoint target(slider->width() * 3 / 4, slider->height() / 2);
  QTest::mousePress(slider, Qt::LeftButton, Qt::NoModifier, target);
  EXPECT_NEAR(toAxisDouble(engine.currentTime()), 75.0, 3.0);
  EXPECT_TRUE(slider->isSliderDown());
  QTest::mouseMove(slider, QPoint(slider->width() / 2, slider->height() / 2));
  QTest::mouseRelease(slider, Qt::LeftButton, Qt::NoModifier, QPoint(slider->width() / 2, slider->height() / 2));
  EXPECT_NEAR(toAxisDouble(engine.currentTime()), 50.0, 3.0);

  widget.setSeekLocked(true);
  QTest::mouseClick(slider, Qt::LeftButton, Qt::NoModifier, target);
  EXPECT_NEAR(toAxisDouble(engine.currentTime()), 50.0, 3.0);
}

TEST(TimelineWidgetTest, StepControlsArrowKeysBeforeDataAndAfterRangeChanges) {
  PlaybackEngine engine;
  TimelineWidget widget;
  widget.setPlaybackEngine(&engine);
  auto* step = widget.findChild<DoubleScrubber*>("playbackStep");
  auto* slider = widget.findChild<RealSlider*>("timeSlider");
  ASSERT_NE(step, nullptr);
  ASSERT_NE(slider, nullptr);

  engine.setRange({displaySeconds(0), displaySeconds(0)});
  step->setValue(0.02);
  engine.setRange({displaySeconds(0), displaySeconds(1)});
  engine.setCurrentTime(displaySeconds(0.4));
  QTest::keyClick(slider, Qt::Key_Right);
  ASSERT_TRUE(QTest::qWaitFor([&] { return std::abs(toAxisDouble(engine.currentTime()) - 0.42) < 1e-9; }, 1000));

  engine.setRange({displaySeconds(0), displaySeconds(20000)});
  engine.setCurrentTime(displaySeconds(1));
  QTest::keyClick(slider, Qt::Key_Right);
  ASSERT_TRUE(QTest::qWaitFor([&] { return std::abs(toAxisDouble(engine.currentTime()) - 1.02) < 1e-9; }, 1000));

  bool parsed = false;
  step->setValue(QStringLiteral("nan").toDouble(&parsed));
  ASSERT_TRUE(parsed);
  EXPECT_EQ(slider->singleStep(), 10);

  step->setValue(0);
  engine.setCurrentTime(displaySeconds(1));
  QTest::keyClick(slider, Qt::Key_Right);
  ASSERT_TRUE(QTest::qWaitFor([&] { return std::abs(toAxisDouble(engine.currentTime()) - 1.002) < 1e-9; }, 1000));

  engine.setRange({displaySeconds(0), displaySeconds(1e-9)});
  engine.setCurrentTime(displaySeconds(0));
  step->setValue(10);
  QTest::keyClick(slider, Qt::Key_Right);
  ASSERT_TRUE(QTest::qWaitFor([&] { return toAxisDouble(engine.currentTime()) == 1e-9; }, 1000));
}

}  // namespace
}  // namespace PJ
