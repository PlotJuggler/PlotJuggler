// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// An XY plot with "keep ratio" on fits each requested viewport to the canvas aspect ratio by widening one axis. A
// startup restore applies the saved range while the window still has a transient size; the resizes that follow must
// re-fit the saved range, not the already-widened view, or the zoom creeps outward on every save and restart.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDomDocument>
#include <QRectF>
#include <array>
#include <string_view>

#include "pj_datastore/writer.hpp"
#include "pj_plotting/PlotWidget.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"
using namespace Qt::StringLiterals;

namespace {

constexpr PJ::Timestamp kNs = 1'000'000'000;

PJ::TopicId addScalarTopic(
    PJ::SessionManager& session, PJ::DatasetId dataset_id, std::string_view topic_name, std::array<double, 3> values) {
  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto handle_or = writer.registerScalarSeries(dataset_id, topic_name, PJ::NumericType::kFloat64);
  EXPECT_TRUE(handle_or.has_value()) << handle_or.error();
  if (!handle_or.has_value()) {
    return 0;
  }
  for (int i = 0; i < 3; ++i) {
    writer.appendScalar(*handle_or, static_cast<PJ::Timestamp>(i + 1) * kNs, values[static_cast<std::size_t>(i)]);
  }
  EXPECT_FALSE(session.commitChunks(writer.flushAll()).empty());
  return handle_or->topic_id;
}

QString keyForTopic(PJ::CatalogModel& catalog, PJ::TopicId topic_id) {
  for (const auto& curve : catalog.curves()) {
    if (const auto descriptor = catalog.curveDescriptor(curve.name); descriptor && descriptor->topic_id == topic_id) {
      return curve.name;
    }
  }
  return {};
}

void resizeAndSettle(QWidget& widget, int width, int height) {
  widget.resize(width, height);
  QApplication::processEvents();
}

void expectSavedRange(const QDomElement& plot_element, double left, double right, double bottom, double top) {
  const QDomElement range = plot_element.firstChildElement(u"range"_s);
  ASSERT_FALSE(range.isNull());
  EXPECT_DOUBLE_EQ(range.attribute(u"left"_s).toDouble(), left);
  EXPECT_DOUBLE_EQ(range.attribute(u"right"_s).toDouble(), right);
  EXPECT_DOUBLE_EQ(range.attribute(u"bottom"_s).toDouble(), bottom);
  EXPECT_DOUBLE_EQ(range.attribute(u"top"_s).toDouble(), top);
}

void expectSameRect(const QRectF& actual, const QRectF& expected) {
  const double tolerance = 1e-6 * expected.width();
  EXPECT_NEAR(actual.left(), expected.left(), tolerance);
  EXPECT_NEAR(actual.right(), expected.right(), tolerance);
  EXPECT_NEAR(actual.top(), expected.top(), tolerance);
  EXPECT_NEAR(actual.bottom(), expected.bottom(), tolerance);
}

}  // namespace

// Save at the final window size, restore while the window is square, then grow it back: the range is the saved one.
TEST(PlotWidgetXyKeepRatio, StartupRestoreAtTransientSizeKeepsSavedRange) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const QString key_x = keyForTopic(catalog, addScalarTopic(session, *dataset, "/pos/x", {-1.0, 0.0, 1.0}));
  const QString key_y = keyForTopic(catalog, addScalarTopic(session, *dataset, "/pos/y", {-1.0, 0.5, 1.0}));
  ASSERT_FALSE(key_x.isEmpty());
  ASSERT_FALSE(key_y.isEmpty());

  PJ::PlotWidget plot(&session, &catalog);
  plot.setModeXY(true);
  plot.setKeepRatioXY(true);
  ASSERT_NE(plot.addCurveXY(key_x, key_y, u"y vs x"_s), nullptr);
  resizeAndSettle(plot, 900, 600);
  plot.show();
  QApplication::processEvents();
  plot.setZoomRectangle(QRectF(QPointF(-3.463, 2.305), QPointF(2.704, -1.917)), /*emit_signal=*/false);
  const QRectF saved = plot.currentBoundingRect();

  QDomDocument doc;
  QDomElement plot_element = plot.xmlSaveState(doc);
  plot.stampLiveCurveKeys(plot_element);

  resizeAndSettle(plot, 600, 600);
  ASSERT_TRUE(plot.xmlLoadState(plot_element));
  plot.setKeepRatioXY(true);  // the shell re-applies its global toggles to every restored plot
  resizeAndSettle(plot, 900, 600);

  expectSameRect(plot.currentBoundingRect(), saved);
}

// Narrowing and widening the window back is not a zoom gesture: the view returns to what it was.
TEST(PlotWidgetXyKeepRatio, ResizeRoundTripKeepsRange) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const QString key_x = keyForTopic(catalog, addScalarTopic(session, *dataset, "/pos/x", {-1.0, 0.0, 1.0}));
  const QString key_y = keyForTopic(catalog, addScalarTopic(session, *dataset, "/pos/y", {-1.0, 0.5, 1.0}));
  ASSERT_FALSE(key_x.isEmpty());
  ASSERT_FALSE(key_y.isEmpty());

  PJ::PlotWidget plot(&session, &catalog);
  plot.setModeXY(true);
  plot.setKeepRatioXY(true);
  ASSERT_NE(plot.addCurveXY(key_x, key_y, u"y vs x"_s), nullptr);
  resizeAndSettle(plot, 900, 600);
  plot.show();
  QApplication::processEvents();
  plot.setZoomRectangle(QRectF(QPointF(-3.0, 2.0), QPointF(3.0, -2.0)), /*emit_signal=*/false);
  const QRectF before = plot.currentBoundingRect();

  resizeAndSettle(plot, 600, 600);
  resizeAndSettle(plot, 900, 600);

  expectSameRect(plot.currentBoundingRect(), before);
}

// A layout stores the range the user asked for, not the axes widened to the canvas shape: a layout saved at one window
// shape and loaded at another would otherwise widen again on every save.
TEST(PlotWidgetXyKeepRatio, SaveStoresRequestedRangeAcrossWindowShapes) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const QString key_x = keyForTopic(catalog, addScalarTopic(session, *dataset, "/pos/x", {-1.0, 0.0, 1.0}));
  const QString key_y = keyForTopic(catalog, addScalarTopic(session, *dataset, "/pos/y", {-1.0, 0.5, 1.0}));
  ASSERT_FALSE(key_x.isEmpty());
  ASSERT_FALSE(key_y.isEmpty());

  PJ::PlotWidget plot(&session, &catalog);
  plot.setModeXY(true);
  plot.setKeepRatioXY(true);
  ASSERT_NE(plot.addCurveXY(key_x, key_y, u"y vs x"_s), nullptr);
  resizeAndSettle(plot, 900, 600);
  plot.show();
  QApplication::processEvents();
  plot.setZoomRectangle(QRectF(QPointF(-3.0, 2.0), QPointF(3.0, -2.0)), /*emit_signal=*/false);

  QDomDocument doc;
  QDomElement first_save = plot.xmlSaveState(doc);
  expectSavedRange(first_save, -3.0, 3.0, -2.0, 2.0);

  plot.stampLiveCurveKeys(first_save);
  resizeAndSettle(plot, 600, 600);
  ASSERT_TRUE(plot.xmlLoadState(first_save));
  plot.setKeepRatioXY(true);  // the shell re-applies its global toggles to every restored plot
  resizeAndSettle(plot, 600, 800);
  expectSavedRange(plot.xmlSaveState(doc), -3.0, 3.0, -2.0, 2.0);
}
