// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>
#include <qwt_plot_curve.h>

#include <QApplication>
#include <QDomDocument>
#include <QPalette>
#include <QPen>
#include <QtGlobal>
#include <string_view>

#include "pj_datastore/writer.hpp"
#include "pj_plotting/PlotWidget.h"
#include "pj_plotting/PlotWidgetBase.h"
#include "pj_plotting/XYCurveDialog.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/CurveDescriptor.h"
#include "pj_runtime/SessionManager.h"
#include "pj_widgets/FrameworkTokens.h"
using namespace Qt::StringLiterals;

namespace {

PJ::TopicId addScalarTopic(PJ::SessionManager& session, PJ::DatasetId dataset_id, std::string_view topic_name) {
  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto handle_or = writer.registerScalarSeries(dataset_id, topic_name, PJ::NumericType::kFloat64);
  EXPECT_TRUE(handle_or.has_value()) << handle_or.error();
  if (!handle_or.has_value()) {
    return 0;
  }
  writer.appendScalar(*handle_or, 100, 1.0);
  writer.appendScalar(*handle_or, 200, 2.0);
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

}  // namespace

// Style and width are plot-level properties: changing one restyles/re-pens every
// existing curve AND is inherited by curves added afterwards. (Two curves in one
// plot can never differ in style/width — only colour is per-curve.)
TEST(PlotWidgetCurveStyle, StyleAndWidthAreInheritedByNewCurves) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const QString key_a = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/x"));
  const QString key_b = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/y"));
  const QString key_c = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/z"));
  ASSERT_FALSE(key_a.isEmpty());

  PJ::PlotWidget plot(&session, &catalog);
  auto* info_a = plot.addCurve(key_a);
  ASSERT_NE(info_a, nullptr);
  QwtPlotCurve* curve_a = info_a->curve;  // QwtPlotCurve* is heap-stable across later addCurve() reallocs.
  ASSERT_NE(curve_a, nullptr);
  ASSERT_EQ(curve_a->style(), QwtPlotCurve::Lines);

  // Plot-level style change restyles the existing curve...
  plot.setDefaultStyle(PJ::PlotWidgetBase::kDots);
  EXPECT_EQ(curve_a->style(), QwtPlotCurve::Dots);
  // ...and is inherited by a curve added afterwards.
  auto* info_b = plot.addCurve(key_b);
  ASSERT_NE(info_b, nullptr);
  ASSERT_NE(info_b->curve, nullptr);
  EXPECT_EQ(info_b->curve->style(), QwtPlotCurve::Dots);

  // Same for width: existing and new curves share the plot's line width.
  plot.setDefaultStyle(PJ::PlotWidgetBase::kLines);
  plot.setLineWidth(PJ::LineWidth::kPoints30);
  const double expected_width = PJ::lineWidthValue(PJ::LineWidth::kPoints30);
  EXPECT_DOUBLE_EQ(curve_a->pen().widthF(), expected_width);
  auto* info_c = plot.addCurve(key_c);
  ASSERT_NE(info_c, nullptr);
  ASSERT_NE(info_c->curve, nullptr);
  EXPECT_DOUBLE_EQ(info_c->curve->pen().widthF(), expected_width);
}

// A point-only (Dots) curve draws its dots at dotWidthValue, so they are as
// visible as the dots of a Lines+Dots curve — not 1px specks. (Regression for
// the old per-curve style toggle that left the thin line pen in place.)
TEST(PlotWidgetCurveStyle, DotsUseDotWidth) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const QString key_a = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/x"));
  ASSERT_FALSE(key_a.isEmpty());

  PJ::PlotWidget plot(&session, &catalog);
  plot.setDefaultStyle(PJ::PlotWidgetBase::kDots);
  auto* info_a = plot.addCurve(key_a);
  ASSERT_NE(info_a, nullptr);
  ASSERT_NE(info_a->curve, nullptr);
  EXPECT_EQ(info_a->curve->style(), QwtPlotCurve::Dots);
  EXPECT_DOUBLE_EQ(info_a->curve->pen().widthF(), PJ::dotWidthValue(PJ::LineWidth::kPoints10));
}

// Style is a plot-level property, so it is serialized once on the <plot>, never
// per <curve> (curves carry only colour + visibility).
TEST(PlotWidgetCurveStyle, StyleIsSavedOnThePlotNotPerCurve) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const QString key_a = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/x"));
  ASSERT_FALSE(key_a.isEmpty());

  PJ::PlotWidget plot(&session, &catalog);
  ASSERT_NE(plot.addCurve(key_a), nullptr);
  plot.setDefaultStyle(PJ::PlotWidgetBase::kDots);

  QDomDocument doc;
  const QDomElement element = plot.xmlSaveState(doc);
  EXPECT_EQ(element.attribute(u"style"_s), u"Dots"_s);
  const QDomElement curve = element.firstChildElement(u"curve"_s);
  ASSERT_FALSE(curve.isNull());
  EXPECT_FALSE(curve.hasAttribute(u"style"_s));
  EXPECT_FALSE(curve.hasAttribute(u"line_width"_s));
}

// XY (scatter) plots default to Dots, and a plot-level style/width change must
// NOT downgrade them to Lines. Regression: making style/width plot-level means
// every plot-level restyle runs over XY curves too. XY curves are NOT forced to a
// style — they use the plot-level style like any curve (the Curve Style toolbar
// controls them), and a later plot-level restyle keeps them on that style.
TEST(PlotWidgetCurveStyle, XyPlotUsesPlotLevelStyle) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const QString key_x = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/x"));
  const QString key_y = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/y"));
  ASSERT_FALSE(key_x.isEmpty());
  ASSERT_FALSE(key_y.isEmpty());

  PJ::PlotWidget plot(&session, &catalog);
  plot.setModeXY(true);
  plot.setDefaultStyle(PJ::PlotWidgetBase::kSticks);  // the user's chosen style

  auto* info = plot.addCurveXY(key_x, key_y, u"x vs y"_s);
  ASSERT_NE(info, nullptr);
  ASSERT_NE(info->curve, nullptr);
  EXPECT_EQ(info->curve->style(), QwtPlotCurve::Sticks);  // XY uses the plot style, not forced Dots

  // A plot-level width change re-applies the plot style to every curve, XY included.
  plot.setLineWidth(PJ::LineWidth::kPoints30);
  EXPECT_EQ(info->curve->style(), QwtPlotCurve::Sticks);
}

// Older PJ4 layouts kept the plot-level line_width at the stale default and stored
// the real width per-curve. Loading one must recover the width from the first curve
// instead of rendering everything thin.
TEST(PlotWidgetCurveStyle, OldLayoutRecoversLineWidthFromFirstCurve) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  PJ::PlotWidget plot(&session, &catalog);

  QDomDocument doc;
  QDomElement plot_el = doc.createElement(u"plot"_s);
  plot_el.setAttribute(u"mode"_s, u"TimeSeries"_s);
  plot_el.setAttribute(u"line_width"_s, u"1.0"_s);  // stale plot-level default
  QDomElement curve_el = doc.createElement(u"curve"_s);
  // The curve key need not resolve — the width is read from the element before curves load.
  curve_el.setAttribute(u"name"_s, u"dataset:1/topic:999/column:0"_s);
  curve_el.setAttribute(u"line_width"_s, u"3.00"_s);  // real old per-curve width
  plot_el.appendChild(curve_el);

  plot.xmlLoadState(plot_el);
  EXPECT_EQ(plot.lineWidth(), PJ::LineWidth::kPoints30);
}

// The XY dialog's alias auto-suggestion: common prefix + "[suffixX;suffixY]".
TEST(XYCurveDialog, SuggestAlias) {
  EXPECT_EQ(PJ::XYCurveDialog::suggestAlias(u"/imu/x"_s, u"/imu/y"_s), u"/imu/[x;y]"_s);
  EXPECT_EQ(PJ::XYCurveDialog::suggestAlias(u"abc"_s, u"xyz"_s), u"[abc;xyz]"_s);
}

// A plot can hold multiple XY curves; each carries its user alias as the title,
// renders as Dots, and saves the alias (the title is not derivable from x/y).
TEST(PlotWidgetCurveStyle, MultipleXyCurvesCarryAliasAndDots) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const QString key_x = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/x"));
  const QString key_y = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/y"));
  const QString key_z = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/z"));
  ASSERT_FALSE(key_x.isEmpty());

  PJ::PlotWidget plot(&session, &catalog);
  plot.setModeXY(true);
  plot.setDefaultStyle(PJ::PlotWidgetBase::kDots);  // the user picks Dots for the scatter
  auto* c1 = plot.addCurveXY(key_x, key_y, u"x vs y"_s);
  auto* c2 = plot.addCurveXY(key_x, key_z, u"x vs z"_s);
  ASSERT_NE(c1, nullptr);
  ASSERT_NE(c2, nullptr);
  ASSERT_EQ(plot.curveList().size(), 2U);
  EXPECT_EQ(c1->source_name, u"x vs y"_s);
  EXPECT_EQ(c2->source_name, u"x vs z"_s);
  EXPECT_EQ(c1->curve->style(), QwtPlotCurve::Dots);  // both inherit the plot style
  EXPECT_EQ(c2->curve->style(), QwtPlotCurve::Dots);

  // Save: each XY curve persists its alias (name attr).
  QDomDocument doc;
  const QDomElement element = plot.xmlSaveState(doc);
  int xy_curves = 0;
  for (QDomElement c = element.firstChildElement(u"curve"_s); !c.isNull(); c = c.nextSiblingElement(u"curve"_s)) {
    EXPECT_TRUE(c.hasAttribute(u"name"_s));
    ++xy_curves;
  }
  EXPECT_EQ(xy_curves, 2);
}

// Loading an XY curve restores its saved alias and the plot style WITHOUT a dialog
// (applyCurveElement passes the saved name straight to addCurveXY; xmlLoadState has
// already seeded the plot-level style, simulated here by setDefaultStyle).
TEST(PlotWidgetCurveStyle, XyLoadRestoresAliasWithoutDialog) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const QString key_x = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/x"));
  const QString key_y = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/y"));
  ASSERT_FALSE(key_x.isEmpty());

  PJ::PlotWidget plot(&session, &catalog);
  plot.setModeXY(true);
  plot.setDefaultStyle(PJ::PlotWidgetBase::kDots);  // the plot's restored style

  QDomDocument doc;
  QDomElement xy_el = doc.createElement(u"curve"_s);
  xy_el.setAttribute(u"curve_x"_s, key_x);  // post-rebind keys
  xy_el.setAttribute(u"curve_y"_s, key_y);
  xy_el.setAttribute(u"name"_s, u"my alias"_s);

  auto* loaded = plot.applyCurveElement(xy_el);
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->curve, nullptr);
  EXPECT_EQ(loaded->source_name, u"my alias"_s);
  EXPECT_EQ(loaded->curve->style(), QwtPlotCurve::Dots);  // inherits the plot style
}

// The empty-plot canvas paints the Data Backdrop surface for the ACTIVE theme,
// and follows a theme change delivered AFTER construction. Regression: the canvas
// colour used to be resolved once at construction, so a plot built before the dark
// palette synced (startup) or a runtime toggle kept the light backdrop. Theme is
// detected from QPalette::Window lightness, which Theme::syncApplicationPalette
// keeps in lockstep with the theme tokens.
TEST(PlotWidgetCanvas, DataBackdropFollowsTheme) {
  const auto set_window = [](const QColor& c) {
    QPalette pal = QApplication::palette();
    pal.setColor(QPalette::Window, c);
    QApplication::setPalette(pal);
    QApplication::processEvents();  // deliver ApplicationPaletteChange
  };
  // The code sets canvas->setPalette(QColor); mirror that derivation here.
  const auto expected_window = [](PJ::theme::Theme t) {
    return QPalette(PJ::theme::surface(PJ::theme::Surface::DataBackdrop, t)).color(QPalette::Window);
  };

  // Guard: the framework palette resource must actually resolve, else the two
  // themes collapse to the same invalid colour and every check below passes
  // trivially (the qrc must be linked into this target).
  const QColor dark_bg = PJ::theme::surface(PJ::theme::Surface::DataBackdrop, PJ::theme::Theme::Dark);
  const QColor light_bg = PJ::theme::surface(PJ::theme::Surface::DataBackdrop, PJ::theme::Theme::Light);
  ASSERT_TRUE(dark_bg.isValid() && light_bg.isValid()) << "framework palette not linked";
  ASSERT_EQ(dark_bg, QColor("#5C5C70"));
  ASSERT_EQ(light_bg, QColor("#FFFFFF"));
  ASSERT_NE(expected_window(PJ::theme::Theme::Dark), expected_window(PJ::theme::Theme::Light));

  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);

  // Start dark (Backdrop #373743, lightness < 128): resolved at construction.
  set_window(QColor("#373743"));
  PJ::PlotWidget plot(&session, &catalog);
  auto* canvas = plot.findChild<QWidget*>(QStringLiteral("qwtCanvas"));
  ASSERT_NE(canvas, nullptr);
  EXPECT_EQ(canvas->palette().color(QPalette::Window), expected_window(PJ::theme::Theme::Dark));

  // Toggle to light (Backdrop #eeeeee): must reach the canvas via changeEvent.
  set_window(QColor("#eeeeee"));
  EXPECT_EQ(canvas->palette().color(QPalette::Window), expected_window(PJ::theme::Theme::Light));

  // And back to dark.
  set_window(QColor("#373743"));
  EXPECT_EQ(canvas->palette().color(QPalette::Window), expected_window(PJ::theme::Theme::Dark));
}

// A default PlotWidget reserves a small top margin (Qwt aligns the canvas to the
// scales, leaving room for the top axis label). Embedded toolbox charts opt out
// via setCanvasAlignedToScales(false) so they sit flush against surrounding
// chrome (e.g. a toolbox banner) — the canvas then starts at the widget's top.
TEST(PlotWidgetCanvas, FlushTopWhenNotAlignedToScales) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  PJ::PlotWidget plot(&session, &catalog);
  plot.resize(600, 400);
  plot.show();
  qApp->processEvents();
  const auto canvas_top = [&]() {
    qApp->processEvents();
    auto* canvas = plot.findChild<QWidget*>(QStringLiteral("qwtCanvas"));
    return canvas ? canvas->mapTo(&plot, QPoint(0, 0)).y() : -1;
  };
  EXPECT_GT(canvas_top(), 0);  // default: aligned to scales, small top margin
  plot.setCanvasAlignedToScales(false);
  EXPECT_EQ(canvas_top(), 0);  // flush: canvas reaches the widget top
}
