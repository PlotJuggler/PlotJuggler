// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The plot's saved X-axis (time) range must be persisted in ABSOLUTE time, not
// in the display-relative seconds the Qwt axis speaks. PJ4's display offset is
// per-dataset and live (it tracks the "Use time offset" toggle and each
// dataset's earliest sample), so a range saved in display coordinates only
// frames the right window for the exact offset present at save time. Storing
// absolute time makes a restored range correct regardless of the offset state
// at load (different toggle position, reloaded data, a shared layout).
//
// The X edges are persisted TWICE: authoritative integer nanoseconds
// (left_ns/right_ns) plus human-readable decimal seconds (left/right, also the
// v3-layout fallback). An explicit x_basis marker ("absolute" for time-series,
// "value" for XY) records the coordinate meaning so load never re-infers it
// from plot mode. This suite pins the single-ns design: exact integer
// round-trip at epoch scale, the v3 decimal fallback, the x_basis marker, and
// the degenerate-range tolerance that keeps a sub-ns snapshot restorable.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDomDocument>
#include <QDomElement>
#include <QRectF>
#include <QtGlobal>
#include <string_view>

#include "pj_datastore/writer.hpp"
#include "pj_plotting/PlotWidget.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"
using namespace Qt::StringLiterals;

namespace {

// Samples sit at a realistic absolute epoch (~2020), so the per-dataset "Use
// time offset" shift is ~1.6e9 s -- large enough that a display-relative range
// ([0, 1]) and the absolute range ([1.6e9, 1.6e9 + 1]) are wildly different
// numbers. That gap is exactly what the bug under test produces, and it is also
// where a double's ULP (~238 ns at 1.6e9 s) would round away a deep-zoom window.
constexpr PJ::Timestamp kT0Ns = 1'600'000'000'000'000'000;  // 1.6e9 s, in ns
constexpr PJ::Timestamp kOneSecondNs = 1'000'000'000;
constexpr double kT0Sec = 1'600'000'000.0;

PJ::TopicId addScalarTopicAtEpoch(PJ::SessionManager& session, PJ::DatasetId dataset_id, std::string_view topic_name) {
  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto handle = writer.registerScalarSeries(dataset_id, topic_name, PJ::NumericType::kFloat64);
  EXPECT_TRUE(handle.has_value()) << handle.error();
  if (!handle.has_value()) {
    return 0;
  }
  writer.appendScalar(*handle, kT0Ns, 1.0);
  writer.appendScalar(*handle, kT0Ns + kOneSecondNs, 2.0);
  EXPECT_FALSE(session.commitChunks(writer.flushAll()).empty());
  return handle->topic_id;
}

QString keyForTopic(PJ::CatalogModel& catalog, PJ::TopicId topic_id) {
  for (const auto& curve : catalog.curves()) {
    if (const auto descriptor = catalog.curveDescriptor(curve.name); descriptor && descriptor->topic_id == topic_id) {
      return curve.name;
    }
  }
  return {};
}

// Mirror MainWindow's pre-load rebind (layout_xml::rebindCurveKeys): the saved
// <curve> carries topic/field, but xmlLoadState matches/creates curves by the
// "name" attribute. Setting it keeps the curve across the load so the plot can
// resolve the dataset's display offset.
void rebindCurveNames(QDomElement& plot_element, const QString& key) {
  for (QDomElement curve = plot_element.firstChildElement(u"curve"_s); !curve.isNull();
       curve = curve.nextSiblingElement(u"curve"_s)) {
    curve.setAttribute(u"name"_s, key);
  }
}

// A plot with one scalar curve drawn from a dataset at the absolute epoch above.
struct PlotFixture {
  PJ::SessionManager session;
  PJ::CatalogModel catalog{&session};
  PJ::PlotWidget plot{&session, &catalog};
  QString key;

  PlotFixture() {
    // Own TimeDomain (mirrors FileLoader's one-domain-per-source): "Use time
    // offset" writes the align-starts shift to the domain, so a default (id 0)
    // domain would carry no offset and the offset-on assertions could not hold.
    auto domain = session.dataEngine().createTimeDomain("drive");
    EXPECT_TRUE(domain.has_value()) << domain.error();
    auto dataset = session.dataEngine().createDataset(
        PJ::DatasetDescriptor{.source_name = "drive.mcap", .time_domain_id = *domain});
    EXPECT_TRUE(dataset.has_value()) << dataset.error();
    const PJ::TopicId topic = addScalarTopicAtEpoch(session, *dataset, "/imu/accel");
    key = keyForTopic(catalog, topic);
    EXPECT_FALSE(key.isEmpty());
    EXPECT_NE(plot.addCurve(key), nullptr);
  }
};

}  // namespace

// With "Use time offset" ON, the axis frames display window [0, 1] s. Saving must
// record the ABSOLUTE window: decimal left/right ~= [1.6e9, 1.6e9 + 1] s, the
// authoritative left_ns/right_ns as exact integers, and x_basis="absolute".
TEST(PlotWidgetRangeOffset, SavesAbsoluteTimeRange) {
  PlotFixture fixture;
  fixture.session.setUseTimeOffset(true);  // display offset = dataset min = kT0Ns
  fixture.plot.setZoomRectangle(QRectF(0.0, -5.0, 1.0, 10.0), /*emit_signal=*/false);

  QDomDocument doc;
  const QDomElement plot_element = fixture.plot.xmlSaveState(doc);
  const QDomElement range = plot_element.firstChildElement(u"range"_s);
  ASSERT_FALSE(range.isNull());

  EXPECT_FALSE(range.hasAttribute(u"x_absolute"_s)) << "the obsolete marker must not be written";
  EXPECT_EQ(range.attribute(u"x_basis"_s), u"absolute"_s);
  EXPECT_NEAR(range.attribute(u"left"_s).toDouble(), kT0Sec, 1e-3);
  EXPECT_NEAR(range.attribute(u"right"_s).toDouble(), kT0Sec + 1.0, 1e-3);
  // The integer edges are exact: display [0,1] + offset kT0Ns.
  EXPECT_EQ(range.attribute(u"left_ns"_s).toLongLong(), kT0Ns);
  EXPECT_EQ(range.attribute(u"right_ns"_s).toLongLong(), kT0Ns + kOneSecondNs);
}

// The integer-ns edges preserve an epoch-scale window EXACTLY, where the decimal
// seconds cannot: a 100 ns / 200 ns display offset from t0 would be rounded away
// by a double at 1.6e9 s (~238 ns ULP). left_ns/right_ns must carry it losslessly.
TEST(PlotWidgetRangeOffset, IntegerNsEdgesPreserveSubUlpWindow) {
  PlotFixture fixture;
  fixture.session.setUseTimeOffset(true);
  // Display window [100 ns, 200 ns] -> absolute [kT0Ns + 100, kT0Ns + 200].
  fixture.plot.setZoomRectangle(QRectF(100.0e-9, -5.0, 100.0e-9, 10.0), /*emit_signal=*/false);

  QDomDocument doc;
  const QDomElement plot_element = fixture.plot.xmlSaveState(doc);
  const QDomElement range = plot_element.firstChildElement(u"range"_s);
  ASSERT_FALSE(range.isNull());
  EXPECT_EQ(range.attribute(u"left_ns"_s).toLongLong(), kT0Ns + 100);
  EXPECT_EQ(range.attribute(u"right_ns"_s).toLongLong(), kT0Ns + 200);
}

// The core bug: save with the offset ON, load with it OFF. The restored axis
// must still frame the same ABSOLUTE instant. With the offset now 0, display ==
// absolute, so the axis reads [1.6e9, 1.6e9 + 1].
TEST(PlotWidgetRangeOffset, RestoresAbsoluteWindowAcrossOffsetChange) {
  PlotFixture fixture;
  fixture.session.setUseTimeOffset(true);
  fixture.plot.setZoomRectangle(QRectF(0.0, -5.0, 1.0, 10.0), /*emit_signal=*/false);

  QDomDocument doc;
  QDomElement plot_element = fixture.plot.xmlSaveState(doc);
  rebindCurveNames(plot_element, fixture.key);

  fixture.session.setUseTimeOffset(false);  // load-time offset = 0
  ASSERT_TRUE(fixture.plot.xmlLoadState(plot_element, /*autozoom=*/true));

  const QRectF rect = fixture.plot.currentBoundingRect();
  EXPECT_NEAR(rect.left(), kT0Sec, 1e-3);
  EXPECT_NEAR(rect.right(), kT0Sec + 1.0, 1e-3);
}

// Common reload case: offset ON at save AND load. The restored axis reproduces
// the same DISPLAY window [0, 1] (absolute round-tripped back through the same
// offset). Exercises the curve-present representative-offset path on load.
TEST(PlotWidgetRangeOffset, RestoresDisplayWindowWhenOffsetUnchanged) {
  PlotFixture fixture;
  fixture.session.setUseTimeOffset(true);
  fixture.plot.setZoomRectangle(QRectF(0.0, -5.0, 1.0, 10.0), /*emit_signal=*/false);

  QDomDocument doc;
  QDomElement plot_element = fixture.plot.xmlSaveState(doc);
  rebindCurveNames(plot_element, fixture.key);

  ASSERT_TRUE(fixture.plot.xmlLoadState(plot_element, /*autozoom=*/true));

  const QRectF rect = fixture.plot.currentBoundingRect();
  EXPECT_NEAR(rect.left(), 0.0, 1e-3);
  EXPECT_NEAR(rect.right(), 1.0, 1e-3);
}

// A deeply-zoomed 3 ns window must survive save/load EXACTLY through the integer
// edges, even at epoch scale where the decimal seconds are useless. Display
// window [100 ns, 103 ns] round-trips back to itself (offset unchanged), so the
// restored ns-domain edges reproduce the exact 3 ns span. This is the
// single-ns + clamp retarget of the donor's sub-ns "keep magnified snapshot
// restorable" case.
TEST(PlotWidgetRangeOffset, DeepMagnificationWindowRoundTripsExactly) {
  PlotFixture fixture;
  fixture.session.setUseTimeOffset(true);
  fixture.plot.setZoomRectangle(QRectF(100.0e-9, -5.0, 3.0e-9, 10.0), /*emit_signal=*/false);

  QDomDocument doc;
  QDomElement plot_element = fixture.plot.xmlSaveState(doc);
  const QDomElement range = plot_element.firstChildElement(u"range"_s);
  ASSERT_FALSE(range.isNull());
  // Non-degenerate: 3 ns apart at the integer edges.
  ASSERT_EQ(range.attribute(u"left_ns"_s).toLongLong(), kT0Ns + 100);
  ASSERT_EQ(range.attribute(u"right_ns"_s).toLongLong(), kT0Ns + 103);
  rebindCurveNames(plot_element, fixture.key);

  ASSERT_TRUE(fixture.plot.xmlLoadState(plot_element, /*autozoom=*/true));
  const QRectF rect = fixture.plot.currentBoundingRect();
  // Back in display coordinates: [100 ns, 103 ns], a real 3 ns window.
  EXPECT_NEAR(rect.left(), 100.0e-9, 1e-12);
  EXPECT_NEAR(rect.right() - rect.left(), 3.0e-9, 1e-12);
}

// A saved range whose integer edges are EQUAL (a viewport captured while zoomed
// below 1 ns) must NOT fail the load — the degenerate-range tolerance restores
// it as a ±0.5 ns window (1 ns wide) instead of falling through to auto-fit.
// Without this, a deep-zoom undo/redo snapshot would wedge permanently.
TEST(PlotWidgetRangeOffset, DegenerateNsRangeRestoresWithTolerance) {
  PlotFixture fixture;
  fixture.session.setUseTimeOffset(true);

  QDomDocument doc;
  QDomElement plot_element = doc.createElement(u"plot"_s);
  plot_element.setAttribute(u"mode"_s, u"TimeSeries"_s);
  QDomElement range = doc.createElement(u"range"_s);
  range.setAttribute(u"x_basis"_s, u"absolute"_s);
  // Equal ns edges: a sub-ns window that rounded to a single instant.
  range.setAttribute(u"left_ns"_s, QString::number(kT0Ns + 500));
  range.setAttribute(u"right_ns"_s, QString::number(kT0Ns + 500));
  range.setAttribute(u"left"_s, QString::number(kT0Sec, 'f', 6));
  range.setAttribute(u"right"_s, QString::number(kT0Sec, 'f', 6));
  range.setAttribute(u"top"_s, u"5.000000"_s);
  range.setAttribute(u"bottom"_s, u"-5.000000"_s);
  plot_element.appendChild(range);
  QDomElement curve = doc.createElement(u"curve"_s);
  curve.setAttribute(u"name"_s, fixture.key);
  plot_element.appendChild(curve);

  ASSERT_TRUE(fixture.plot.xmlLoadState(plot_element, /*autozoom=*/true));
  const QRectF rect = fixture.plot.currentBoundingRect();
  // Restored as a real (1 ns wide) window around 500 ns display, NOT auto-fit.
  EXPECT_GT(rect.width(), 0.0) << "degenerate range must restore, not auto-fit";
  EXPECT_NEAR(rect.width(), 1.0e-9, 1e-12);
  EXPECT_NEAR(0.5 * (rect.left() + rect.right()), 500.0e-9, 1e-12);
}

// The degenerate-range tolerance must also hold with "Use time offset" OFF, where
// the display value sits at wall-clock EPOCH scale (~1.6e9 s). A fixed +-0.5 ns
// nudge is far below the double ULP there (~2.4e-7 s), so both edges collapse back
// to one value and the load falls through to auto-fit -- wedging a deep-zoom undo/
// redo snapshot. The ULP-aware widening must keep the restored edges distinct.
TEST(PlotWidgetRangeOffset, DegenerateNsRangeRestoresAtEpochScaleWithOffsetOff) {
  PlotFixture fixture;
  fixture.session.setUseTimeOffset(false);  // offset = 0 -> display == absolute epoch

  QDomDocument doc;
  QDomElement plot_element = doc.createElement(u"plot"_s);
  plot_element.setAttribute(u"mode"_s, u"TimeSeries"_s);
  QDomElement range = doc.createElement(u"range"_s);
  range.setAttribute(u"x_basis"_s, u"absolute"_s);
  // Equal ns edges at epoch scale: a sub-ns window that rounded to one instant.
  range.setAttribute(u"left_ns"_s, QString::number(kT0Ns + 500));
  range.setAttribute(u"right_ns"_s, QString::number(kT0Ns + 500));
  range.setAttribute(u"left"_s, QString::number(kT0Sec, 'f', 6));
  range.setAttribute(u"right"_s, QString::number(kT0Sec, 'f', 6));
  range.setAttribute(u"top"_s, u"5.000000"_s);
  range.setAttribute(u"bottom"_s, u"-5.000000"_s);
  plot_element.appendChild(range);
  QDomElement curve = doc.createElement(u"curve"_s);
  curve.setAttribute(u"name"_s, fixture.key);
  plot_element.appendChild(curve);

  ASSERT_TRUE(fixture.plot.xmlLoadState(plot_element, /*autozoom=*/true));
  const QRectF rect = fixture.plot.currentBoundingRect();
  // The restored window must be a NON-DEGENERATE pair of distinct doubles at epoch
  // scale (not auto-fit). Its center stays at the saved instant (~1.6e9 s + 500 ns).
  EXPECT_NE(rect.left(), rect.right()) << "epoch-scale degenerate range must restore to distinct edges";
  EXPECT_GT(rect.width(), 0.0) << "must restore a real window, not auto-fit to the data";
  EXPECT_NEAR(0.5 * (rect.left() + rect.right()), kT0Sec, 1e-3);
}

// A schema-v3 layout carries decimal left/right only (no left_ns/right_ns) and,
// after normalizePlotRangeBasis on load, x_basis="absolute". The fallback leg
// must parse the decimals and convert with the current offset. Here an absolute
// window [kT0Sec, kT0Sec + 1] restores to display [0, 1].
TEST(PlotWidgetRangeOffset, V3DecimalOnlyRangeLoadsViaFallback) {
  PlotFixture fixture;
  fixture.session.setUseTimeOffset(true);  // offset = kT0Ns

  QDomDocument doc;
  QDomElement plot_element = doc.createElement(u"plot"_s);
  plot_element.setAttribute(u"mode"_s, u"TimeSeries"_s);
  QDomElement range = doc.createElement(u"range"_s);
  // A v3 layout after normalizePlotRangeBasis: annotated absolute, decimals only.
  range.setAttribute(u"x_basis"_s, u"absolute"_s);
  range.setAttribute(u"left"_s, QString::number(kT0Sec, 'f', 6));
  range.setAttribute(u"right"_s, QString::number(kT0Sec + 1.0, 'f', 6));
  range.setAttribute(u"top"_s, u"5.000000"_s);
  range.setAttribute(u"bottom"_s, u"-5.000000"_s);
  plot_element.appendChild(range);
  QDomElement curve = doc.createElement(u"curve"_s);
  curve.setAttribute(u"name"_s, fixture.key);
  plot_element.appendChild(curve);

  ASSERT_TRUE(fixture.plot.xmlLoadState(plot_element, /*autozoom=*/true));
  const QRectF rect = fixture.plot.currentBoundingRect();
  EXPECT_NEAR(rect.left(), 0.0, 1e-3);
  EXPECT_NEAR(rect.right(), 1.0, 1e-3);
}

// A time-series range with NO x_basis marker (a raw v3 layout not run through
// normalizePlotRangeBasis) still loads as absolute: xmlLoadState treats anything
// that is not x_basis="value" as absolute time. An absolute window
// [kT0Sec, kT0Sec + 1] restores to display [0, 1].
TEST(PlotWidgetRangeOffset, UnmarkedTimeRangeLoadsAsAbsolute) {
  PlotFixture fixture;
  fixture.session.setUseTimeOffset(true);  // offset = kT0Ns

  QDomDocument doc;
  QDomElement plot_element = doc.createElement(u"plot"_s);
  plot_element.setAttribute(u"mode"_s, u"TimeSeries"_s);
  QDomElement range = doc.createElement(u"range"_s);
  range.setAttribute(u"left"_s, QString::number(kT0Sec, 'f', 6));
  range.setAttribute(u"right"_s, QString::number(kT0Sec + 1.0, 'f', 6));
  range.setAttribute(u"top"_s, u"5.000000"_s);
  range.setAttribute(u"bottom"_s, u"-5.000000"_s);
  plot_element.appendChild(range);
  QDomElement curve = doc.createElement(u"curve"_s);
  curve.setAttribute(u"name"_s, fixture.key);
  plot_element.appendChild(curve);

  ASSERT_TRUE(fixture.plot.xmlLoadState(plot_element, /*autozoom=*/true));

  const QRectF rect = fixture.plot.currentBoundingRect();
  EXPECT_NEAR(rect.left(), 0.0, 1e-3);
  EXPECT_NEAR(rect.right(), 1.0, 1e-3);
}

// An XY plot's X is a data VALUE, not time: the range is written with
// x_basis="value", NO left_ns/right_ns, and the values are stored/restored
// verbatim (offset-independent) regardless of any active time offset.
TEST(PlotWidgetRangeOffset, XyPlotWritesValueBasisAndIgnoresOffset) {
  PlotFixture fixture;
  fixture.session.setUseTimeOffset(true);
  fixture.plot.setModeXY(true);
  fixture.plot.setZoomRectangle(QRectF(2.0, -5.0, 6.0, 10.0), /*emit_signal=*/false);

  QDomDocument doc;
  const QDomElement plot_element = fixture.plot.xmlSaveState(doc);
  const QDomElement range = plot_element.firstChildElement(u"range"_s);
  ASSERT_FALSE(range.isNull());
  EXPECT_EQ(range.attribute(u"x_basis"_s), u"value"_s);
  EXPECT_FALSE(range.hasAttribute(u"left_ns"_s)) << "XY X is a value, never integer ns";
  EXPECT_FALSE(range.hasAttribute(u"right_ns"_s));
  // Verbatim value range: [2, 8] (left 2, width 6), no offset applied.
  EXPECT_NEAR(range.attribute(u"left"_s).toDouble(), 2.0, 1e-6);
  EXPECT_NEAR(range.attribute(u"right"_s).toDouble(), 8.0, 1e-6);
}
