// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// A link in a label open during a live theme switch (Preferences' "what is sent", next to the theme toggle) takes the
// new theme's link colour: the light theme's dark blue is unreadable on the dark dialog.

#include <gtest/gtest.h>

#include <QApplication>
#include <QImage>
#include <QLabel>

#include "Theme.h"
#include "WidgetTuner.h"

using namespace Qt::StringLiterals;

namespace {

// How many pixels of the rendered label are close to `ink`.
int pixelsNear(QLabel& label, const QColor& ink) {
  const QImage image = label.grab().toImage();
  int count = 0;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      const QColor pixel = image.pixelColor(x, y);
      if (std::abs(pixel.red() - ink.red()) + std::abs(pixel.green() - ink.green()) +
              std::abs(pixel.blue() - ink.blue()) <
          30) {
        ++count;
      }
    }
  }
  return count;
}

TEST(LabelLinkThemeTest, ALinkTakesTheNewThemesColourOnALiveSwitch) {
  PJ::WidgetTuner tuner;
  qApp->installEventFilter(&tuner);
  PJ::Theme theme;
  theme.setTheme(u"light"_s);
  qApp->setStyleSheet(theme.expandedQss());
  QLabel label(u"Anonymous usage statistics (<a href=\"https://example.com\">what is sent</a>):"_s);
  label.resize(label.sizeHint());
  ASSERT_GT(pixelsNear(label, QApplication::palette().color(QPalette::Link)), 0) << "the light link is not drawn";

  theme.setTheme(u"dark"_s);
  qApp->setStyleSheet(theme.expandedQss());
  const QColor dark_link = QApplication::palette().color(QPalette::Link);
  EXPECT_GT(pixelsNear(label, dark_link), 0)
      << "the link kept the light theme's colour, not " << dark_link.name().toStdString();
  qApp->setStyleSheet(QString());
}

}  // namespace
