// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Real-GL coverage for RasterTextEngine: the GL paint engine must be detected
// (that path's glyph atlas is what dies on GPU reset / context recreation),
// text drawn through the wrapper must leave ink on a GL framebuffer, and a
// real QwtPlotOpenGLCanvas render must route legend text through the CPU
// raster path once the engines are installed. Self-skips without usable GL.

#include <gtest/gtest.h>
#include <qwt_plot.h>
#include <qwt_plot_opengl_canvas.h>
#include <qwt_text_engine.h>

#include <QApplication>
#include <QFont>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLPaintDevice>
#include <QPainter>
#include <QSettings>
#include <QVBoxLayout>
#include <QWidget>
#include <memory>
#include <string_view>
#include <tuple>

#include "pj_datastore/writer.hpp"
#include "pj_plotting/PlotWidget.h"
#include "pj_plotting/RasterTextEngine.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"
#include "pj_widgets/FrameworkTokens.h"
using namespace Qt::StringLiterals;

namespace {

class TestablePlotWidget : public PJ::PlotWidget {
 public:
  using PJ::PlotWidget::PlotWidget;
  using PJ::PlotWidgetBase::qwtPlot;
};

PJ::TopicId addScalarTopic(PJ::SessionManager& session, PJ::DatasetId dataset_id, std::string_view topic_name) {
  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto handle_or = writer.registerScalarSeries(dataset_id, topic_name, PJ::NumericType::kFloat64);
  if (!handle_or.has_value()) {
    return 0;
  }
  for (int i = 0; i < 20; ++i) {
    writer.appendScalar(*handle_or, 100 + i * 10, static_cast<double>(i % 5));
  }
  std::ignore = session.commitChunks(writer.flushAll());
  return handle_or->topic_id;
}

QString keyForTopic(PJ::CatalogModel& catalog, PJ::TopicId topic_id) {
  for (const auto& curve : catalog.curves()) {
    if (const auto d = catalog.curveDescriptor(curve.name); d && d->topic_id == topic_id) {
      return curve.name;
    }
  }
  return {};
}

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

}  // namespace

TEST(RasterTextGl, DetectsGlPainterAndLeavesInk) {
  QOpenGLContext context;
  if (!context.create()) {
    GTEST_SKIP() << "no OpenGL context available";
  }
  QOffscreenSurface surface;
  surface.setFormat(context.format());
  surface.create();
  if (!surface.isValid() || !context.makeCurrent(&surface)) {
    GTEST_SKIP() << "no usable offscreen GL surface";
  }

  QOpenGLFramebufferObject fbo(QSize(200, 60));
  ASSERT_TRUE(fbo.bind());
  {
    QOpenGLPaintDevice device(fbo.size());
    QPainter painter(&device);
    EXPECT_TRUE(PJ::painterUsesGlTextPath(&painter)) << "GL paint engine not detected";

    painter.fillRect(QRect(0, 0, 200, 60), Qt::white);
    painter.setPen(Qt::black);
    QFont font = painter.font();
    font.setPointSize(11);
    painter.setFont(font);

    const PJ::RasterTextEngine engine(std::make_unique<QwtPlainTextEngine>());
    const int before = PJ::RasterTextEngine::glRasterDrawCount();
    engine.draw(&painter, QRectF(6, 6, 188, 48), Qt::AlignLeft | Qt::AlignVCenter, u"Hello"_s);
    painter.end();
    EXPECT_GT(PJ::RasterTextEngine::glRasterDrawCount(), before) << "draw did not take the GL raster path";
  }

  EXPECT_GT(darkPixelCount(fbo.toImage()), 10) << "no text ink on the GL framebuffer";
  fbo.release();
  context.doneCurrent();
}

TEST(RasterTextGl, LegendTextOnGlCanvasRoutesThroughRasterPath) {
  // Install BEFORE any QwtText exists (the plot below creates them); mirrors
  // the pj_app main() call order.
  PJ::installRasterTextEngines();

  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const QString key = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/x"));
  ASSERT_FALSE(key.isEmpty());

  // Declared before the plot so the plot is destroyed first, detaching itself
  // from the window; every early GTEST_SKIP() return stays leak- and
  // double-delete-free.
  QWidget window;
  auto plot = std::make_unique<TestablePlotWidget>(&session, &catalog);
  ASSERT_NE(plot->addCurve(key), nullptr);
  plot->qwtPlot()->setAxisScale(QwtPlot::xBottom, 100.0, 300.0);
  plot->qwtPlot()->setAxisScale(QwtPlot::yLeft, -1.0, 5.0);

  auto* canvas = qobject_cast<QwtPlotOpenGLCanvas*>(plot->qwtPlot()->canvas());
  if (canvas == nullptr) {
    GTEST_SKIP() << "plot is not using the OpenGL canvas (use_opengl off)";
  }

  auto* layout = new QVBoxLayout(&window);
  layout->setContentsMargins(
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None),
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None));
  layout->addWidget(plot.get());
  window.resize(400, 300);
  window.show();
  QApplication::processEvents();

  if (canvas->context() == nullptr || !canvas->context()->isValid()) {
    GTEST_SKIP() << "canvas did not get a GL context on this platform";
  }

  // grabFramebuffer() forces a synchronous paintGL -> legend draw -> QwtText
  // -> installed engine, independent of window exposure.
  const int before = PJ::RasterTextEngine::glRasterDrawCount();
  const QImage frame = canvas->grabFramebuffer();
  EXPECT_GT(PJ::RasterTextEngine::glRasterDrawCount(), before)
      << "legend text on the GL canvas did not route through the CPU raster path";
  EXPECT_FALSE(frame.isNull());

  // Opt-in visual artifact for eyeballing the rendered legend text.
  if (const QByteArray dump_path = qgetenv("PJ_SAVE_TEST_FRAME"); !dump_path.isEmpty()) {
    frame.save(QString::fromLocal8Bit(dump_path));
  }
}

int main(int argc, char** argv) {
  // Force the GL canvas; honour an externally-set QT_QPA_PLATFORM so CI can
  // choose its GL-capable platform.
  QSettings().setValue(u"Preferences::use_opengl"_s, true);
  testing::InitGoogleTest(&argc, argv);
  QApplication app(argc, argv);
  return RUN_ALL_TESTS();
}
