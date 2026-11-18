// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A curve can be on a plot before its topic has any rows (an on-demand derived
// series starts computing only once displayed). The fit the drop path runs then
// sees no data and frames nothing; the non-live rows must still bring the whole
// curve into view, chunk after chunk, until the user zooms or pans. A plot that
// had data at fit time keeps the user's zoom, as for a file load.

#include <gtest/gtest.h>

#include <QApplication>
#include <algorithm>
#include <initializer_list>
#include <limits>
#include <optional>

#include "pj_datastore/writer.hpp"
#include "pj_plotting/PlotWidget.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"

namespace {

constexpr PJ::Timestamp kT0Ns = 1'600'000'000'000'000'000;  // ~2020 epoch, in ns
constexpr PJ::Timestamp kOneSecondNs = 1'000'000'000;
// Far outside the [-1, 1] band an empty plot shows, so an unfitted Y misses it.
constexpr double kBaseValue = 100.0;

QString keyForTopic(PJ::CatalogModel& catalog, PJ::TopicId topic_id) {
  for (const auto& curve : catalog.curves()) {
    if (const auto descriptor = catalog.curveDescriptor(curve.name); descriptor && descriptor->topic_id == topic_id) {
      return curve.name;
    }
  }
  return {};
}

// One plot showing a float64 curve whose topic is registered but has no rows yet.
struct FirstRowsFixture {
  PJ::SessionManager session;
  PJ::CatalogModel catalog{&session};
  PJ::PlotWidget plot{&session, &catalog};
  PJ::DataWriter writer = session.dataEngine().createWriter();
  PJ::ScalarSeriesHandle handle{};
  PJ::DatasetId dataset_id = 0;

  FirstRowsFixture() {
    auto domain = session.dataEngine().createTimeDomain("run");
    EXPECT_TRUE(domain.has_value()) << domain.error();
    auto dataset =
        session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "run.mcap", .time_domain_id = *domain});
    EXPECT_TRUE(dataset.has_value()) << dataset.error();

    dataset_id = *dataset;
    auto registered = writer.registerScalarSeries(*dataset, "/derived", PJ::NumericType::kFloat64);
    EXPECT_TRUE(registered.has_value()) << registered.error();
    handle = *registered;
    session.notifyIngest({handle.topic_id});  // the catalog lists the empty topic

    const QString key = keyForTopic(catalog, handle.topic_id);
    EXPECT_FALSE(key.isEmpty()) << "empty topic must still be in the catalog";
    EXPECT_NE(plot.addCurve(key), nullptr);
  }

  // Appends samples at the given seconds after kT0Ns and commits them (non-live).
  void commitSamples(std::initializer_list<int> seconds) {
    for (const int second : seconds) {
      writer.appendScalar(handle, kT0Ns + second * kOneSecondNs, kBaseValue + second);
    }
    EXPECT_FALSE(session.commitChunks(writer.flushAll()).empty());
  }

  // Replaces the plot's curve with one on a new topic without rows; commitSamples writes to it from then on.
  void replaceWithEmptySeries() {
    const QString old_key = keyForTopic(catalog, handle.topic_id);
    auto registered = writer.registerScalarSeries(dataset_id, "/derived_output", PJ::NumericType::kFloat64);
    ASSERT_TRUE(registered.has_value()) << registered.error();
    handle = *registered;
    session.notifyIngest({handle.topic_id});
    plot.replaceCurve(old_key, keyForTopic(catalog, handle.topic_id));
  }

  [[nodiscard]] double xSpan() const {
    const QRectF view = plot.currentBoundingRect();
    return view.right() - view.left();
  }

  // A zoom or pan, as the zoomer, panners and magnifier report it.
  void userZoomTo(const QRectF& rect) {
    plot.setZoomRectangle(rect, /*emit_signal=*/false);
    emit plot.viewResized(rect);
  }

  void expectXRange(double left, double right) const {
    const QRectF view = plot.currentBoundingRect();
    EXPECT_NEAR(view.left(), left, 1e-6);
    EXPECT_NEAR(view.right(), right, 1e-6);
  }
};

}  // namespace

// The fit ran before any row existed: the first rows, and every later chunk of a
// series computed in pieces, must be framed whole while nobody touches the view.
TEST(PlotWidgetFirstRowsFit, RowsFitAPlotFittedWithoutData) {
  FirstRowsFixture fixture;
  fixture.plot.zoomOut(/*emit_signal=*/false);
  ASSERT_EQ(fixture.xSpan(), 0.0) << "precondition: a fit without data frames nothing";

  fixture.commitSamples({0, 2});
  EXPECT_NEAR(fixture.xSpan(), 2.0, 1e-6) << "the X axis must cover both samples";

  fixture.commitSamples({5});
  fixture.commitSamples({10});
  EXPECT_NEAR(fixture.xSpan(), 10.0, 1e-6) << "the X axis must cover every chunk";
  const QRectF view = fixture.plot.currentBoundingRect();
  EXPECT_LE(std::min(view.top(), view.bottom()), kBaseValue);
  EXPECT_GE(std::max(view.top(), view.bottom()), kBaseValue + 10.0) << "Y must cover every chunk too";
}

// A linked peer still waiting for rows has an empty X window to share: taking it
// must not reduce this plot's fit to Y, which would leave X at [0, 0] for good.
TEST(PlotWidgetFirstRowsFit, EmptyExternalWindowKeepsTheFullFit) {
  FirstRowsFixture fixture;
  fixture.plot.zoomOut(/*emit_signal=*/false);
  fixture.plot.setVisibleXRange(0.0, 0.0);

  fixture.commitSamples({0, 2});

  EXPECT_NEAR(fixture.xSpan(), 2.0, 1e-6);
}

// Wheel or pan over a plot with X=[0, 0] reports a NaN or empty window. Nothing
// was framed, so the first rows must still be.
TEST(PlotWidgetFirstRowsFit, GestureThatFramesNothingKeepsTheFit) {
  const double not_a_number = std::numeric_limits<double>::quiet_NaN();
  for (const QRectF& reported : {QRectF(0.0, 1.0, 0.0, -2.0), QRectF(not_a_number, 1.0, not_a_number, -2.0)}) {
    FirstRowsFixture fixture;
    fixture.plot.zoomOut(/*emit_signal=*/false);
    emit fixture.plot.viewResized(reported);

    fixture.commitSamples({0, 2});

    EXPECT_NEAR(fixture.xSpan(), 2.0, 1e-6);
  }
}

// Linked peers took this plot's empty or partial window, so each fit must reach them.
TEST(PlotWidgetFirstRowsFit, FullFitTellsLinkedPeers) {
  FirstRowsFixture fixture;
  fixture.plot.zoomOut(/*emit_signal=*/false);
  std::optional<QRectF> announced;
  QObject::connect(
      &fixture.plot, &PJ::PlotWidget::rectChanged, &fixture.plot,
      [&announced](PJ::PlotWidget*, QRectF rect) { announced = rect; });

  fixture.commitSamples({0, 2});

  ASSERT_TRUE(announced.has_value());
  EXPECT_NEAR(announced->right() - announced->left(), 2.0, 1e-6);
}

// The user's fits (the toolbar Zoom Out, a drop, a layout restore) that found no rows: the first plot to receive rows
// pulls its linked peer to its window, which lines linked plots of different datasets up after a load or a stream
// start.
TEST(PlotWidgetFirstRowsFit, FirstRowsPullALinkedPeerAfterAUserFit) {
  FirstRowsFixture fixture;
  auto peer_series = fixture.writer.registerScalarSeries(fixture.dataset_id, "/peer", PJ::NumericType::kFloat64);
  ASSERT_TRUE(peer_series.has_value()) << peer_series.error();
  fixture.session.notifyIngest({peer_series->topic_id});
  PJ::PlotWidget peer(&fixture.session, &fixture.catalog);
  ASSERT_NE(peer.addCurve(keyForTopic(fixture.catalog, peer_series->topic_id)), nullptr);
  // MainWindow's linked zoom: a plot's announced window becomes the peer's X window.
  QObject::connect(&fixture.plot, &PJ::PlotWidget::rectChanged, &peer, [&peer](PJ::PlotWidget*, QRectF rect) {
    peer.setVisibleXRange(rect.left(), rect.right());
  });
  fixture.plot.zoomOut(/*emit_signal=*/false);
  peer.zoomOut(/*emit_signal=*/false);

  fixture.commitSamples({0, 2});

  const QRectF view = fixture.plot.currentBoundingRect();
  EXPECT_NEAR(peer.currentBoundingRect().left(), view.left(), 1e-6);
  EXPECT_NEAR(peer.currentBoundingRect().right(), view.right(), 1e-6);
  EXPECT_NEAR(view.right() - view.left(), 2.0, 1e-6);
}

// An unlinked fit (an agent's or a plugin's plot) took no window from the linked peers, so the fit the first rows
// complete must not move them either.
TEST(PlotWidgetFirstRowsFit, UnlinkedFitStaysQuietWhenRowsArrive) {
  FirstRowsFixture fixture;
  fixture.plot.zoomOutUnlinked();
  int announced = 0;
  QObject::connect(&fixture.plot, &PJ::PlotWidget::rectChanged, &fixture.plot, [&announced](PJ::PlotWidget*, QRectF) {
    ++announced;
  });

  fixture.commitSamples({0, 2});
  fixture.commitSamples({10});

  EXPECT_NEAR(fixture.xSpan(), 10.0, 1e-6) << "the plot is still fitted";
  EXPECT_EQ(announced, 0);
}

// A Zoom Out after an unlinked fit is the user's again: its refit reaches the peers.
TEST(PlotWidgetFirstRowsFit, ZoomOutAfterAnUnlinkedFitAnnouncesAgain) {
  FirstRowsFixture fixture;
  fixture.plot.zoomOutUnlinked();
  fixture.plot.zoomOut(/*emit_signal=*/false);
  int announced = 0;
  QObject::connect(&fixture.plot, &PJ::PlotWidget::rectChanged, &fixture.plot, [&announced](PJ::PlotWidget*, QRectF) {
    ++announced;
  });

  fixture.commitSamples({0, 2});

  EXPECT_EQ(announced, 1);
}

// Filter Apply swaps the only curve for its output, which computes only now.
TEST(PlotWidgetFirstRowsFit, ReplacingTheCurveKeepsTheFit) {
  FirstRowsFixture fixture;
  fixture.plot.zoomOut(/*emit_signal=*/false);
  fixture.replaceWithEmptySeries();

  fixture.commitSamples({0, 2});

  EXPECT_NEAR(fixture.xSpan(), 2.0, 1e-6);
}

// An emptied plot has nothing left to fit: a curve added later brings its own zoom.
TEST(PlotWidgetFirstRowsFit, EmptiedPlotDropsTheFit) {
  FirstRowsFixture fixture;
  fixture.plot.zoomOut(/*emit_signal=*/false);
  fixture.plot.removeAllCurves();
  ASSERT_NE(fixture.plot.addCurve(keyForTopic(fixture.catalog, fixture.handle.topic_id)), nullptr);
  const QRectF before = fixture.plot.currentBoundingRect();

  fixture.commitSamples({0, 2});

  fixture.expectXRange(before.left(), before.right());
}

// A zoom or pan after the first rows stops the framing: later chunks keep it.
TEST(PlotWidgetFirstRowsFit, GestureAfterFirstRowsStopsTheFit) {
  FirstRowsFixture fixture;
  fixture.plot.zoomOut(/*emit_signal=*/false);
  fixture.commitSamples({0, 2});
  const QRectF zoomed = fixture.plot.currentBoundingRect().adjusted(0.5, 0.0, -0.5, 0.0);
  fixture.userZoomTo(zoomed);

  fixture.commitSamples({10});

  fixture.expectXRange(zoomed.left(), zoomed.right());
}

// A zoom or pan made while the plot waits for its first rows is the user's
// framing: the rows must not override it.
TEST(PlotWidgetFirstRowsFit, GestureBeforeFirstRowsCancelsTheFit) {
  FirstRowsFixture fixture;
  fixture.plot.zoomOut(/*emit_signal=*/false);
  const QRectF chosen(5.0, 3.0, 4.0, -6.0);  // x in [5, 9], y in [-3, 3]
  fixture.userZoomTo(chosen);

  fixture.commitSamples({0, 2});

  fixture.expectXRange(chosen.left(), chosen.right());
}

// An X window imposed from outside (the zoom link aligning a dropped plot to its
// peers) stays; the first rows still fit Y over it.
TEST(PlotWidgetFirstRowsFit, ExternalXWindowKeepsXAndFitsY) {
  FirstRowsFixture fixture;
  fixture.plot.zoomOut(/*emit_signal=*/false);
  const double t0_sec = static_cast<double>(kT0Ns) / static_cast<double>(kOneSecondNs);
  fixture.plot.setVisibleXRange(t0_sec - 1.0, t0_sec + 1.0);

  fixture.commitSamples({0, 2});

  fixture.expectXRange(t0_sec - 1.0, t0_sec + 1.0);
  const QRectF view = fixture.plot.currentBoundingRect();
  const double y_min = std::min(view.top(), view.bottom());
  const double y_max = std::max(view.top(), view.bottom());
  EXPECT_LE(y_min, kBaseValue);
  EXPECT_GE(y_max, kBaseValue) << "Y must fit the sample inside the window";
}

// A plot with data that the user zoomed keeps that zoom when more non-live rows
// arrive.
TEST(PlotWidgetFirstRowsFit, UserZoomSurvivesLaterRows) {
  FirstRowsFixture fixture;
  fixture.commitSamples({0, 2});
  fixture.plot.zoomOut(/*emit_signal=*/false);
  const QRectF zoomed = fixture.plot.currentBoundingRect().adjusted(0.5, 0.0, -0.5, 0.0);
  fixture.plot.setZoomRectangle(zoomed, /*emit_signal=*/false);

  fixture.commitSamples({10});

  fixture.expectXRange(zoomed.left(), zoomed.right());
}
