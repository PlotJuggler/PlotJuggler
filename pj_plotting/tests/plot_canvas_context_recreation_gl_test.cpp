// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Real-GL regression for the plot OpenGL canvas across a context recreation
// (what ADS does on float/undock/layout-restore, since AA_ShareOpenGLContexts
// is deliberately off). QwtPlotOpenGLCanvas caches its content in an internal
// FBO and its initializeGL() is a no-op, so a recreated context can be left
// blitting a framebuffer owned by the destroyed context -> the whole canvas
// goes black/frozen. This pins that the canvas still draws real content after
// the context is recreated.
//
// Needs a real GL >= 4.5 context AND a platform that actually recreates the
// QOpenGLWidget context on reparent; it self-skips otherwise (e.g. offscreen).

#include <gtest/gtest.h>
#include <qwt_plot.h>
#include <qwt_plot_opengl_canvas.h>

#include <QApplication>
#include <QImage>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QSettings>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>
#include <QtGlobal>
#include <string_view>
#include <utility>

#include "pj_datastore/writer.hpp"
#include "pj_gpu_test_gate.h"  // PJ_SKIP_WITHOUT_GPU
#include "pj_plotting/PlotWidget.h"
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

std::pair<int, int> glVersionOf(QOpenGLContext* ctx) {
  if (ctx == nullptr || !ctx->isValid()) {
    return {0, 0};
  }
  return parseGlVersion(reinterpret_cast<const char*>(ctx->functions()->glGetString(GL_VERSION)));
}

// A real render has both light (background) and dark (curve/axes) pixels; a
// black/frozen frame collapses to near-zero luminance spread.
bool hasVisibleContent(const QImage& img) {
  const QImage rgb = img.convertToFormat(QImage::Format_ARGB32);
  int lo = 255;
  int hi = 0;
  for (int y = 0; y < rgb.height(); ++y) {
    const auto* row = reinterpret_cast<const QRgb*>(rgb.scanLine(y));
    for (int x = 0; x < rgb.width(); ++x) {
      const int lum = qGray(row[x]);
      lo = std::min(lo, lum);
      hi = std::max(hi, lum);
    }
  }
  return (hi - lo) > 40;
}

}  // namespace

TEST(PlotCanvasContextRecreation, CanvasStillDrawsAfterContextRecreation) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const QString key = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/x"));
  ASSERT_FALSE(key.isEmpty());

  auto plot = std::make_unique<TestablePlotWidget>(&session, &catalog);
  ASSERT_NE(plot->addCurve(key), nullptr);
  plot->qwtPlot()->setAxisScale(QwtPlot::xBottom, 100.0, 300.0);
  plot->qwtPlot()->setAxisScale(QwtPlot::yLeft, -1.0, 5.0);

  auto* canvas = qobject_cast<QwtPlotOpenGLCanvas*>(plot->qwtPlot()->canvas());
  if (canvas == nullptr) {
    GTEST_SKIP() << "plot is not using the OpenGL canvas (use_opengl off)";
  }

  QWidget window1;
  auto* layout1 = new QVBoxLayout(&window1);
  layout1->setContentsMargins(
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None),
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None));
  layout1->addWidget(plot.get());
  window1.resize(400, 300);
  window1.show();
  QApplication::processEvents();

  if (glVersionOf(canvas->context()) < std::pair<int, int>(4, 5)) {
    plot->setParent(nullptr);  // detach before the stack windows unwind, else window1
                               // and the unique_ptr both delete the plot (double free)
    PJ_SKIP_WITHOUT_GPU("no real GL >= 4.5 context (offscreen/software)");
  }

  bool context_recreated = false;
  QObject::connect(
      canvas->context(), &QOpenGLContext::aboutToBeDestroyed, canvas,
      [&context_recreated] { context_recreated = true; }, Qt::DirectConnection);

  const QImage before = canvas->grabFramebuffer();
  ASSERT_TRUE(hasVisibleContent(before)) << "canvas drew nothing before reparent";

  // ADS float/undock/layout-restore == reparent to a different top-level window,
  // which recreates the QOpenGLWidget's (unshared) context.
  QWidget window2;
  auto* layout2 = new QVBoxLayout(&window2);
  layout2->setContentsMargins(
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None),
      PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None));
  plot->setParent(nullptr);
  layout2->addWidget(plot.get());
  window2.resize(400, 300);  // same size -> the cached FBO would be reused as-is
  window2.show();
  QApplication::processEvents();

  if (!context_recreated) {
    plot->setParent(nullptr);  // same detach-before-unwind guard (window2 owns plot here)
    GTEST_SKIP() << "platform did not recreate the GL context on reparent";
  }

  const QImage after = canvas->grabFramebuffer();
  EXPECT_TRUE(hasVisibleContent(after)) << "canvas went blank after GL context recreation (stale FBO)";

  plot->setParent(nullptr);  // detach before layout2 is destroyed
}

int main(int argc, char** argv) {
  // Force the GL canvas; a real platform (not offscreen) is needed for a true
  // context recreation on reparent, so honour an externally-set QT_QPA_PLATFORM.
  QSettings().setValue(u"Preferences::use_opengl"_s, true);
  testing::InitGoogleTest(&argc, argv);
  QApplication app(argc, argv);
  return RUN_ALL_TESTS();
}
