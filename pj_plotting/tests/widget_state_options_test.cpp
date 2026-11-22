// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The plot and state-strip validators are self-describing: validating a saved state reports every attribute it
// carries (the options the MCP widget_state tool serves), every reported token validates, and a refusal names the
// attribute, exactly where xmlLoadState refuses.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDomDocument>
#include <functional>
#include <string_view>
#include <utility>

#include "pj_datastore/writer.hpp"
#include "pj_plotting/PlotWidget.h"
#include "pj_plotting/StateTransitionsDockWidget.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/PlaybackEngine.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/XmlStateOptions.h"
using namespace Qt::StringLiterals;

namespace {

using Validate = std::function<bool(const QDomElement&)>;

struct Session {
  PJ::SessionManager session;
  PJ::CatalogModel catalog{&session};
  PJ::DatasetId dataset = 0;
  QString key_a;
  QString key_b;

  Session() {
    dataset = *session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
    key_a = addTopic("/a");
    key_b = addTopic("/b");
  }

  QString addTopic(std::string_view name) {
    PJ::DataWriter writer = session.dataEngine().createWriter();
    auto handle = writer.registerScalarSeries(dataset, name, PJ::NumericType::kFloat64);
    EXPECT_TRUE(handle.has_value());
    writer.appendScalar(*handle, 1'000'000'000, 1.0);
    writer.appendScalar(*handle, 2'000'000'000, 2.0);
    EXPECT_FALSE(session.commitChunks(writer.flushAll()).empty());
    for (const auto& curve : catalog.curves()) {
      if (const auto descriptor = catalog.curveDescriptor(curve.name);
          descriptor && descriptor->topic_id == handle->topic_id) {
        return curve.name;
      }
    }
    return {};
  }
};

QDomElement copyOf(QDomDocument& doc, const QDomElement& element) {
  doc.appendChild(doc.importNode(element, /*deep=*/true));
  return doc.documentElement();
}

QDomElement firstWithTag(const QDomElement& root, const QString& tag) {
  return root.tagName() == tag ? root : root.elementsByTagName(tag).at(0).toElement();
}

QDomElement savedTimeSeriesPlot(Session& s, QDomDocument& doc) {
  PJ::PlotWidget plot(&s.session, &s.catalog);
  EXPECT_NE(plot.addCurve(s.key_a), nullptr);
  EXPECT_NE(plot.addCurve(s.key_b), nullptr);
  plot.setZoomRectangle(QRectF(0.0, -5.0, 1.0, 10.0), /*emit_signal=*/false);
  plot.setDatasetMarkerScopeVisible(s.dataset, PJ::MarkerScope::kDataset, false);
  const QDomElement saved = plot.xmlSaveState(doc);
  EXPECT_FALSE(saved.firstChildElement(u"marker_scope"_s).isNull());
  return saved;
}

QDomElement savedXyPlot(Session& s, QDomDocument& doc) {
  PJ::PlotWidget plot(&s.session, &s.catalog);
  plot.setModeXY(true);
  EXPECT_NE(plot.addCurveXY(s.key_a, s.key_b, u"b vs a"_s), nullptr);
  plot.setZoomRectangle(QRectF(0.0, -5.0, 1.0, 10.0), /*emit_signal=*/false);
  return plot.xmlSaveState(doc);
}

}  // namespace

TEST(WidgetStateOptions, SavedPlotsAreSelfDescribing) {
  Session s;
  PJ::PlotWidget validator(&s.session, &s.catalog);
  const Validate validate = [&validator](const QDomElement& e) { return validator.xmlValidateState(e); };
  for (const auto& save : {savedTimeSeriesPlot, savedXyPlot}) {
    QDomDocument doc;
    const QDomElement saved = save(s, doc);
    ASSERT_FALSE(saved.firstChildElement(u"range"_s).isNull());
    ASSERT_FALSE(saved.firstChildElement(u"curve"_s).isNull());
    // A plot's mode must agree with its curves' shape (ModeFlipOnAPopulatedPlotIsNoted).
    EXPECT_EQ(PJ::xmlSelfDescriptionProblems(validate, saved, {.skip_tokens = {u"plot.mode"_s}}), QStringList{});
  }
}

TEST(WidgetStateOptions, SavedStateStripIsSelfDescribing) {
  Session s;
  PJ::PlaybackEngine playback;
  PJ::StateTransitionsDockWidget strip(&s.session, &s.catalog, &playback);
  // An unresolved row is saved through the same writer as a bound one, without needing discrete data here.
  QDomDocument seed;
  ASSERT_TRUE(seed.setContent(
      u"<state_transitions zoom_px_per_ns=\"4e-08\"><series dataset_id=\"9\" dataset_source=\"late.mcap\" "
      "dataset_path=\"/data/late.mcap\" topic=\"/state\" field=\"\" visible=\"0\"/></state_transitions>"_s));
  ASSERT_TRUE(strip.xmlLoadState(seed.documentElement()));
  QDomDocument doc;
  const QDomElement saved = strip.xmlSaveState(doc);
  ASSERT_FALSE(saved.firstChildElement(u"series"_s).isNull());
  EXPECT_EQ(
      PJ::xmlSelfDescriptionProblems([&strip](const QDomElement& e) { return strip.xmlValidateState(e); }, saved),
      QStringList{});
}

// xmlLoadState maps an unknown token to its fallback; the validator keeps that tolerance and notes the value.
TEST(WidgetStateOptions, UnknownTokensStayTolerated) {
  Session s;
  PJ::PlotWidget plot(&s.session, &s.catalog);
  QDomDocument saved_doc;
  const QDomElement saved = savedTimeSeriesPlot(s, saved_doc);
  for (const auto& [tag, attribute] :
       {std::pair{u"plot"_s, u"mode"_s}, std::pair{u"plot"_s, u"style"_s}, std::pair{u"plot"_s, u"line_width"_s},
        std::pair{u"range"_s, u"x_basis"_s}, std::pair{u"curve"_s, u"visible"_s},
        std::pair{u"marker_scope"_s, u"scope"_s}}) {
    QDomDocument doc;
    const QDomElement root = copyOf(doc, saved);
    firstWithTag(root, tag).setAttribute(attribute, u"bogus"_s);
    PJ::XmlOptionsRecorder recorder;
    EXPECT_TRUE(plot.xmlValidateState(root)) << attribute.toStdString();
    // Tolerated for a layout load, yet noted: widget_state refuses it.
    EXPECT_TRUE(recorder.firstRejection().startsWith(u'<' + tag + u"> "_s + attribute + u"=\"bogus\""_s))
        << recorder.firstRejection().toStdString();
  }
}

// A layout load tolerates a mode its curves do not match; widget_state refuses it, since the plot it builds is broken
// (time-series curves drawn as XY, or every XY curve dropped).
TEST(WidgetStateOptions, ModeFlipOnAPopulatedPlotIsNoted) {
  Session s;
  PJ::PlotWidget plot(&s.session, &s.catalog);
  const std::pair<QDomElement (*)(Session&, QDomDocument&), QString> cases[] = {
      {savedTimeSeriesPlot, u"<plot> mode=\"XYPlot\": a time-series curve in an XYPlot"_s},
      {savedXyPlot, u"<plot> mode=\"TimeSeries\": an XY curve in a TimeSeries plot"_s},
  };
  for (const auto& [save, rejection] : cases) {
    QDomDocument doc;
    QDomElement saved = save(s, doc);
    const bool xy = saved.attribute(u"mode"_s) == u"XYPlot"_s;
    {
      PJ::XmlOptionsRecorder recorder;
      EXPECT_TRUE(plot.xmlValidateState(saved));
      EXPECT_EQ(recorder.firstRejection(), QString());
    }
    saved.setAttribute(u"mode"_s, xy ? u"TimeSeries"_s : u"XYPlot"_s);
    PJ::XmlOptionsRecorder recorder;
    EXPECT_TRUE(plot.xmlValidateState(saved));
    EXPECT_EQ(recorder.firstRejection(), rejection);
  }
}

// The plot refuses only an invalid or duplicate pending curve, and the refusal names the attribute.
TEST(WidgetStateOptions, RefusalNamesTheAttribute) {
  struct Case {
    QString plot_attributes;
    QString curves;
    QString rejection_prefix;
  };
  const Case cases[] = {
      {u"mode=\"TimeSeries\""_s, u"<curve pending_intent=\"true\" topic=\"/later\" field=\"\" dataset_id=\"abc\"/>"_s,
       u"<curve> dataset_id=\"abc\": "_s},
      {u"mode=\"XYPlot\""_s, u"<curve pending_intent=\"true\" topic=\"/later\" field=\"\"/>"_s,
       u"<curve> x_topic=\"\": "_s},
      {u"mode=\"TimeSeries\""_s,
       u"<curve pending_intent=\"true\" topic=\"/later\"/><curve pending_intent=\"true\" topic=\"/later\"/>"_s,
       u"<curve>: duplicates"_s},
  };
  Session s;
  PJ::PlotWidget plot(&s.session, &s.catalog);
  for (const Case& c : cases) {
    QDomDocument doc;
    ASSERT_TRUE(doc.setContent(u"<plot "_s + c.plot_attributes + u">"_s + c.curves + u"</plot>"_s));
    PJ::XmlOptionsRecorder recorder;
    EXPECT_FALSE(plot.xmlValidateState(doc.documentElement()));
    EXPECT_TRUE(recorder.firstRejection().startsWith(c.rejection_prefix)) << recorder.firstRejection().toStdString();
    EXPECT_FALSE(plot.xmlLoadState(doc.documentElement()));
  }
  QDomDocument doc;
  ASSERT_TRUE(doc.setContent(u"<plot/>"_s));
  PJ::PlaybackEngine playback;
  PJ::StateTransitionsDockWidget strip(&s.session, &s.catalog, &playback);
  PJ::XmlOptionsRecorder recorder;
  EXPECT_FALSE(strip.xmlValidateState(doc.documentElement()));
  EXPECT_EQ(recorder.firstRejection(), u"<plot>: expected a <state_transitions> element"_s);
}
