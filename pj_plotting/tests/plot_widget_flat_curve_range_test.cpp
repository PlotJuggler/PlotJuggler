// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A time series whose samples all share one value must still get a readable Y
// axis. PlotWidgetBase::getVisualizationRangeY derives its headroom from a 2.5%
// proportional margin, which is exactly 0 when min == max, collapsing the range
// to zero height (Qwt then draws a flat, unreadable axis). The fallback pads a
// flat curve by a fixed ±0.1 so the constant value sits centered with headroom.

#include <gtest/gtest.h>

#include <QApplication>
#include <QtGlobal>
#include <string_view>

#include "pj_datastore/writer.hpp"
#include "pj_plotting/PlotWidget.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"

namespace {

constexpr PJ::Timestamp kT0Ns = 1'600'000'000'000'000'000;  // ~2020 epoch, in ns
constexpr PJ::Timestamp kOneSecondNs = 1'000'000'000;
constexpr double kFlatValue = 5.0;

QString keyForTopic(PJ::CatalogModel& catalog, PJ::TopicId topic_id) {
  for (const auto& curve : catalog.curves()) {
    if (const auto descriptor = catalog.curveDescriptor(curve.name); descriptor && descriptor->topic_id == topic_id) {
      return curve.name;
    }
  }
  return {};
}

// One plot showing a single curve whose two samples both equal kFlatValue.
struct FlatCurveFixture {
  PJ::SessionManager session;
  PJ::CatalogModel catalog{&session};
  PJ::PlotWidget plot{&session, &catalog};

  FlatCurveFixture() {
    auto domain = session.dataEngine().createTimeDomain("run");
    EXPECT_TRUE(domain.has_value()) << domain.error();
    auto dataset =
        session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "run.mcap", .time_domain_id = *domain});
    EXPECT_TRUE(dataset.has_value()) << dataset.error();

    PJ::DataWriter writer = session.dataEngine().createWriter();
    auto handle = writer.registerScalarSeries(*dataset, "/const", PJ::NumericType::kFloat64);
    EXPECT_TRUE(handle.has_value()) << handle.error();
    writer.appendScalar(*handle, kT0Ns, kFlatValue);
    writer.appendScalar(*handle, kT0Ns + kOneSecondNs, kFlatValue);
    EXPECT_FALSE(session.commitChunks(writer.flushAll()).empty());

    EXPECT_NE(plot.addCurve(keyForTopic(catalog, handle->topic_id)), nullptr);
  }
};

}  // namespace

// The Y extent of an all-same-value curve must be a visible ±0.1 band centered
// on the constant, not a zero-height range.
TEST(PlotWidgetFlatCurveRange, ConstantCurveGetsPaddedYRange) {
  FlatCurveFixture fixture;
  fixture.plot.zoomOut(/*emit_signal=*/false);

  const QRectF rect = fixture.plot.maxZoomRect();
  // top()/bottom() carry the Y max/min (the rect is built y-up), so top > bottom.
  EXPECT_NEAR(rect.top(), kFlatValue + 0.1, 1e-6);
  EXPECT_NEAR(rect.bottom(), kFlatValue - 0.1, 1e-6);
  EXPECT_NEAR(rect.top() - rect.bottom(), 0.2, 1e-6) << "flat curve must not collapse to zero height";
}
