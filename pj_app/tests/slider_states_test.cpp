// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Every slider in the app must answer the pointer: a distinct colour under the
// cursor, and a further distinct colour while its handle is grabbed. These are
// stylesheet-driven, so the only honest check is to apply the real expanded QSS
// and read the handle's pixels back out of a render in each state.

#include <gtest/gtest.h>

#include <QApplication>
#include <QHoverEvent>
#include <QImage>
#include <QMouseEvent>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <array>

#include "Theme.h"
#include "pj_widgets/RealSlider.h"
#include "support/gui_test_env.h"

namespace PJ {
namespace {

// Sliders need a Theme that outlives their stylesheet-driven renders.
struct QtEnvironment : ::testing::Environment {
  void SetUp() override {
    if (pj_app_test::isTestSuiteSelected("SliderStates")) {
      theme_ = new Theme;
    }
  }
  void TearDown() override {
    delete theme_;
    theme_ = nullptr;
  }
  Theme* theme_ = nullptr;
};

// The environment owns the Theme so tests can re-point the whole application at
// either palette; a slider must be built *after* the switch so it polishes
// against the stylesheet under test.
QtEnvironment* environment = nullptr;

void applyTheme(const QString& name) {
  environment->theme_->setTheme(name);
  qApp->setStyleSheet(environment->theme_->expandedQss());
}

// Both palettes ship independent hex values, so a state that reads distinctly in
// one can silently collapse in the other.
constexpr std::array<const char*, 2> kThemes = {"dark", "light"};

static auto* const kEnv = ::testing::AddGlobalTestEnvironment(environment = new QtEnvironment);

constexpr int kSliderLength = 400;
// The playback chrome row the timeSlider is designed to fill.
constexpr int kPlaybackThickness = 24;
// Roomy enough that a plain slider's handle is never clipped by the widget.
constexpr int kPlainThickness = 20;

// The handle's rectangle as the style will lay it out. QSlider::initStyleOption
// is protected, so the option is filled the same way QSlider fills it.
QStyleOptionSlider sliderOption(QSlider* slider) {
  QStyleOptionSlider opt;
  opt.initFrom(slider);
  opt.orientation = slider->orientation();
  opt.minimum = slider->minimum();
  opt.maximum = slider->maximum();
  opt.sliderPosition = slider->sliderPosition();
  opt.sliderValue = slider->value();
  opt.upsideDown = slider->invertedAppearance();
  opt.subControls = QStyle::SC_All;
  return opt;
}

// The handle's dominant colour in the slider's *current* state, read off a
// render of the widget.
//
// Rendering goes through QWidget::grab rather than QStyle::drawComplexControl:
// the stylesheet style resolves its rules against the live widget, so painting
// a hand-built option into a detached image produces nothing at all.
QColor handleColor(QSlider* slider) {
  // RGB32 drops alpha: a QSS-styled widget grabs with transparent pixels that
  // would otherwise all read back as #000000 and hide every difference.
  const QImage image = slider->grab().toImage().convertToFormat(QImage::Format_RGB32);

  QStyleOptionSlider opt = sliderOption(slider);
  const QRect handle = slider->style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, slider);
  QHash<QRgb, int> counts;
  for (int y = handle.top(); y <= handle.bottom(); ++y) {
    for (int x = handle.left(); x <= handle.right(); ++x) {
      if (image.valid(x, y)) {
        counts[image.pixel(x, y)]++;
      }
    }
  }
  QRgb best = 0;
  int best_count = 0;
  for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
    if (it.value() > best_count) {
      best_count = it.value();
      best = it.key();
    }
  }
  return QColor::fromRgb(best);
}

// Reports the three colours for one slider so a failure names what it saw.
struct HandleStates {
  QColor nominal;
  QColor hovered;
  QColor pressed;
};

HandleStates sampleStates(QSlider* slider, int width, int height) {
  slider->setRange(0, 100);
  slider->setValue(50);  // mid-track: the handle is clear of both end caps
  slider->resize(width, height);
  slider->show();
  // The stylesheet is applied on polish; rendering before it has run would
  // measure the unstyled fallback.
  slider->ensurePolished();
  QApplication::processEvents();

  HandleStates states;
  states.nominal = handleColor(slider);

  // Two things have to be true for the stylesheet to see a hover: the widget
  // must report underMouse() (that is what becomes State_MouseOver) and the
  // slider must have resolved the handle as its hovered subcontrol. Neither
  // happens without a real pointer, so both are set explicitly.
  QStyleOptionSlider opt = sliderOption(slider);
  const QPointF centre =
      slider->style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, slider).center();
  slider->setAttribute(Qt::WA_UnderMouse, true);
  QHoverEvent hover(QEvent::HoverMove, centre, centre, centre);
  QApplication::sendEvent(slider, &hover);
  states.hovered = handleColor(slider);

  QMouseEvent press(QEvent::MouseButtonPress, centre, centre, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(slider, &press);
  states.pressed = handleColor(slider);
  return states;
}

// Both assertions in one place so every slider is held to the same contract.
void expectAnswersPointer(const HandleStates& states, const char* what) {
  EXPECT_NE(states.hovered, states.nominal)
      << what << ": no hover response — nominal=" << states.nominal.name().toStdString()
      << " hovered=" << states.hovered.name().toStdString();
  EXPECT_NE(states.pressed, states.hovered)
      << what << ": no grabbed response — hovered=" << states.hovered.name().toStdString()
      << " pressed=" << states.pressed.name().toStdString();
}

TEST(SliderStates, PlaybackSliderAnswersHoverAndGrab) {
  for (const char* theme : kThemes) {
    applyTheme(QString::fromLatin1(theme));
    RealSlider slider;
    slider.setObjectName(QStringLiteral("timeSlider"));
    slider.setOrientation(Qt::Horizontal);
    expectAnswersPointer(
        sampleStates(&slider, kSliderLength, kPlaybackThickness),
        (QStringLiteral("timeSlider [") + theme + "]").toStdString().c_str());
  }
}

TEST(SliderStates, PlainHorizontalSliderAnswersHoverAndGrab) {
  for (const char* theme : kThemes) {
    applyTheme(QString::fromLatin1(theme));
    QSlider slider(Qt::Horizontal);
    expectAnswersPointer(
        sampleStates(&slider, kSliderLength, kPlainThickness),
        (QStringLiteral("horizontal QSlider [") + theme + "]").toStdString().c_str());
  }
}

TEST(SliderStates, PlainVerticalSliderAnswersHoverAndGrab) {
  for (const char* theme : kThemes) {
    applyTheme(QString::fromLatin1(theme));
    QSlider slider(Qt::Vertical);
    expectAnswersPointer(
        sampleStates(&slider, kPlainThickness, kSliderLength),
        (QStringLiteral("vertical QSlider [") + theme + "]").toStdString().c_str());
  }
}

}  // namespace
}  // namespace PJ
