// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Offscreen unit coverage for RasterTextEngine, the GL-safe text decorator:
// it must delegate layout metrics to the wrapped engine, pass through
// untouched on raster paint devices, rasterize DPR-aware images carrying the
// painter's pen/font, and install exactly once into Qwt's engine dictionary.

#include <gtest/gtest.h>
#include <qwt_text.h>
#include <qwt_text_engine.h>

#include <QApplication>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <memory>

#include "pj_plotting/RasterTextEngine.h"
using namespace Qt::StringLiterals;

namespace {

constexpr int kFlags = Qt::AlignLeft | Qt::AlignVCenter;

int inkPixelCount(const QImage& image) {
  if (image.isNull()) {
    return 0;
  }
  const QImage rgba = image.convertToFormat(QImage::Format_ARGB32);
  int count = 0;
  for (int y = 0; y < rgba.height(); ++y) {
    const auto* row = reinterpret_cast<const QRgb*>(rgba.scanLine(y));
    for (int x = 0; x < rgba.width(); ++x) {
      if (qAlpha(row[x]) > 0) {
        ++count;
      }
    }
  }
  return count;
}

// Pixels visibly darker than a white background — "did text get drawn here".
int darkPixelCount(const QImage& image) {
  const QImage rgba = image.convertToFormat(QImage::Format_ARGB32);
  int count = 0;
  for (int y = 0; y < rgba.height(); ++y) {
    const auto* row = reinterpret_cast<const QRgb*>(rgba.scanLine(y));
    for (int x = 0; x < rgba.width(); ++x) {
      if (qGray(row[x]) < 128) {
        ++count;
      }
    }
  }
  return count;
}

// Average color of the solid glyph cores (alpha > 200), ignoring antialiased
// edges whose blending skews channel values.
QColor inkCoreColor(const QImage& image) {
  const QImage rgba = image.convertToFormat(QImage::Format_ARGB32);
  long r = 0;
  long g = 0;
  long b = 0;
  long n = 0;
  for (int y = 0; y < rgba.height(); ++y) {
    const auto* row = reinterpret_cast<const QRgb*>(rgba.scanLine(y));
    for (int x = 0; x < rgba.width(); ++x) {
      if (qAlpha(row[x]) > 200) {
        r += qRed(row[x]);
        g += qGreen(row[x]);
        b += qBlue(row[x]);
        ++n;
      }
    }
  }
  if (n == 0) {
    return {};
  }
  return QColor(static_cast<int>(r / n), static_cast<int>(g / n), static_cast<int>(b / n));
}

// A canvas painter carrying known pen/font state at the given DPR.
struct StatePainter {
  explicit StatePainter(qreal dpr, const QColor& pen_color = Qt::black)
      : canvas(400, 100, QImage::Format_ARGB32_Premultiplied) {
    canvas.setDevicePixelRatio(dpr);
    canvas.fill(Qt::white);
    painter.begin(&canvas);
    painter.setPen(pen_color);
    QFont font = painter.font();
    font.setPointSize(10);
    painter.setFont(font);
  }
  ~StatePainter() {
    painter.end();
  }
  QImage canvas;
  QPainter painter;
};

}  // namespace

TEST(RasterizeEngineText, ProducesInkAtDevicePixelRatio) {
  StatePainter state(2.0);
  const QwtPlainTextEngine engine;
  const QImage image = PJ::rasterizeEngineText(engine, state.painter, QSizeF(80, 16), kFlags, u"Hello"_s);

  ASSERT_FALSE(image.isNull());
  EXPECT_DOUBLE_EQ(image.devicePixelRatio(), 2.0);
  EXPECT_EQ(image.width(), 160);
  EXPECT_EQ(image.height(), 32);
  EXPECT_GT(inkPixelCount(image), 20);
}

TEST(RasterizeEngineText, HonorsPenColor) {
  StatePainter state(1.0, Qt::red);
  const QwtPlainTextEngine engine;
  const QImage image = PJ::rasterizeEngineText(engine, state.painter, QSizeF(80, 16), kFlags, u"Hello"_s);

  const QColor core = inkCoreColor(image);
  ASSERT_TRUE(core.isValid()) << "no solid ink pixels";
  EXPECT_GT(core.red(), 180);
  EXPECT_LT(core.green(), 80);
  EXPECT_LT(core.blue(), 80);
}

TEST(RasterizeEngineText, RichTextHonorsHtmlColor) {
  StatePainter state(1.0);
  const QwtRichTextEngine engine;
  const QImage image =
      PJ::rasterizeEngineText(engine, state.painter, QSizeF(80, 20), kFlags, u"<font color=\"#ff0000\">XX</font>"_s);

  const QColor core = inkCoreColor(image);
  ASSERT_TRUE(core.isValid()) << "no solid ink pixels";
  EXPECT_GT(core.red(), 180);
  EXPECT_LT(core.green(), 80);
  EXPECT_LT(core.blue(), 80);
}

TEST(RasterizeEngineText, HonorsPainterTransformScale) {
  // Qwt's GL backing-store path paints on a physical-size QOpenGLPaintDevice
  // (device DPR 1) and applies the display ratio as a painter scale — the
  // raster must fold that in, or HiDPI text rasters at 1x and blits up blurry.
  StatePainter state(1.0);
  state.painter.scale(2.0, 2.0);
  const QwtPlainTextEngine engine;
  const QImage image = PJ::rasterizeEngineText(engine, state.painter, QSizeF(80, 16), kFlags, u"Hello"_s);

  ASSERT_FALSE(image.isNull());
  EXPECT_DOUBLE_EQ(image.devicePixelRatio(), 2.0);
  EXPECT_EQ(image.width(), 160);
  EXPECT_EQ(image.height(), 32);
}

TEST(RasterizeEngineText, CombinesDeviceDprWithTransformScale) {
  StatePainter state(2.0);
  state.painter.scale(1.5, 1.5);
  const QwtPlainTextEngine engine;
  const QImage image = PJ::rasterizeEngineText(engine, state.painter, QSizeF(80, 16), kFlags, u"Hello"_s);

  ASSERT_FALSE(image.isNull());
  EXPECT_DOUBLE_EQ(image.devicePixelRatio(), 3.0);
  EXPECT_EQ(image.width(), 240);
  EXPECT_EQ(image.height(), 48);
}

TEST(RasterizeEngineText, EmptyTextGivesNullImage) {
  StatePainter state(1.0);
  const QwtPlainTextEngine engine;
  EXPECT_TRUE(PJ::rasterizeEngineText(engine, state.painter, QSizeF(80, 16), kFlags, QString()).isNull());
  EXPECT_TRUE(PJ::rasterizeEngineText(engine, state.painter, QSizeF(0, 0), kFlags, u"x"_s).isNull());
}

TEST(RasterTextEngine, PassthroughOnRasterDeviceMatchesStockEngine) {
  const QRectF rect(4, 4, 112, 22);

  QImage stock_image(120, 30, QImage::Format_ARGB32_Premultiplied);
  stock_image.fill(Qt::white);
  {
    QPainter painter(&stock_image);
    painter.setPen(Qt::black);
    const QwtPlainTextEngine stock;
    stock.draw(&painter, rect, kFlags, u"Hello"_s);
  }

  QImage wrapped_image(120, 30, QImage::Format_ARGB32_Premultiplied);
  wrapped_image.fill(Qt::white);
  {
    QPainter painter(&wrapped_image);
    painter.setPen(Qt::black);
    const PJ::RasterTextEngine wrapper(std::make_unique<QwtPlainTextEngine>());
    wrapper.draw(&painter, rect, kFlags, u"Hello"_s);
  }

  // Byte-identical: on a raster device the wrapper must delegate, not
  // raster+blit (which would alter antialiased compositing).
  EXPECT_EQ(stock_image, wrapped_image);
  EXPECT_GT(darkPixelCount(stock_image), 0) << "stock engine drew nothing — the comparison is vacuous";
}

TEST(RasterTextEngine, DelegatesLayoutMetrics) {
  const PJ::RasterTextEngine wrapper(std::make_unique<QwtPlainTextEngine>());
  const QwtPlainTextEngine stock;
  QFont font;
  font.setPointSize(11);
  const QString text = u"Hello world"_s;

  EXPECT_EQ(wrapper.textSize(font, kFlags, text), stock.textSize(font, kFlags, text));
  EXPECT_DOUBLE_EQ(wrapper.heightForWidth(font, kFlags, text, 60.0), stock.heightForWidth(font, kFlags, text, 60.0));
  EXPECT_TRUE(wrapper.mightRender(text));

  double wl = -1;
  double wr = -1;
  double wt = -1;
  double wb = -1;
  double sl = -2;
  double sr = -2;
  double st = -2;
  double sb = -2;
  wrapper.textMargins(font, text, wl, wr, wt, wb);
  stock.textMargins(font, text, sl, sr, st, sb);
  EXPECT_DOUBLE_EQ(wl, sl);
  EXPECT_DOUBLE_EQ(wr, sr);
  EXPECT_DOUBLE_EQ(wt, st);
  EXPECT_DOUBLE_EQ(wb, sb);

  // Rich wrapper must keep the stock rich-text detection, or AutoText
  // resolution in Qwt's engine dictionary breaks.
  const PJ::RasterTextEngine rich_wrapper(std::make_unique<QwtRichTextEngine>());
  EXPECT_TRUE(rich_wrapper.mightRender(u"<b>x</b>"_s));
}

TEST(PainterUsesGlTextPath, FalseForImagePainter) {
  QImage image(32, 32, QImage::Format_ARGB32_Premultiplied);
  QPainter painter(&image);
  EXPECT_FALSE(PJ::painterUsesGlTextPath(&painter));
  EXPECT_FALSE(PJ::painterUsesGlTextPath(nullptr));
}

// Keep last in this file: installation deletes Qwt's stock engines, so any
// QwtText constructed before this point must not be drawn afterwards.
TEST(InstallRasterTextEngines, ReplacesStockEnginesExactlyOnce) {
  EXPECT_TRUE(PJ::installRasterTextEngines());
  EXPECT_NE(dynamic_cast<const PJ::RasterTextEngine*>(QwtText::textEngine(QwtText::PlainText)), nullptr);
  EXPECT_NE(dynamic_cast<const PJ::RasterTextEngine*>(QwtText::textEngine(QwtText::RichText)), nullptr);
  EXPECT_FALSE(PJ::installRasterTextEngines()) << "second install must be a no-op";
}
