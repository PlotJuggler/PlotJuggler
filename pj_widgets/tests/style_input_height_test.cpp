// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// PJ::Style pins every input-chrome control (line edit / combo / spin box) to
// EXACTLY one compact height (kInputHeight) so they line up with each other and
// with PJ::CheckButton (a fixed-height pill). The bug this guards: the old clamp
// was a *maximum* of kInputHeight floored at the font height, so Fusion-narrow
// controls (scrubbers) rendered SHORTER than 20 while combos capped AT 20 — three
// different heights in one panel. Height must be kInputHeight for any font.

#include <gtest/gtest.h>

#include <QApplication>
#include <QFont>
#include <QFontMetrics>
#include <QStyleOptionFrame>

#include "pj_widgets/Style.h"
using namespace Qt::StringLiterals;

namespace {

TEST(StyleInputHeight, PinsInputsToKInputHeightForAnyFont) {
  PJ::Style style(u"Fusion"_s);

  // A small font (Fusion would size the control below 20) and a tall one (above
  // 20): both must come out exactly kInputHeight.
  for (const int pixel_size : {10, 13, 24}) {
    QFont font;
    font.setPixelSize(pixel_size);
    QStyleOptionFrame opt;
    opt.fontMetrics = QFontMetrics(font);
    for (const QStyle::ContentsType type : {QStyle::CT_LineEdit, QStyle::CT_ComboBox, QStyle::CT_SpinBox}) {
      const QSize out = style.sizeFromContents(type, &opt, QSize(80, opt.fontMetrics.height()), nullptr);
      EXPECT_EQ(out.height(), PJ::Style::kInputHeight)
          << "pixel_size=" << pixel_size << " ContentsType=" << static_cast<int>(type);
    }
  }
}

}  // namespace
