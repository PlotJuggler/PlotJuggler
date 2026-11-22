// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QDomDocument>
#include <limits>

#include "pj_runtime/XmlStateOptions.h"

using namespace Qt::StringLiterals;

namespace {

QDomElement element(QDomDocument& doc, const QString& xml) {
  EXPECT_TRUE(doc.setContent(xml));
  return doc.documentElement();
}

}  // namespace

TEST(XmlStateOptions, EnumReportsItsTokensAndFallback) {
  QDomDocument doc;
  const QDomElement el = element(doc, u"<cloud shape=\"cube\"/>"_s);
  PJ::XmlOptionsRecorder recorder;
  QString value;
  ASSERT_TRUE(PJ::readXmlEnum(el, u"shape"_s, {u"sphere"_s, u"cube"_s}, u"sphere"_s, value));
  EXPECT_EQ(value, u"cube"_s);
  ASSERT_EQ(recorder.options().size(), 1u);
  const PJ::XmlOption& option = recorder.options().front();
  EXPECT_EQ(option.element, u"cloud"_s);
  EXPECT_EQ(option.attribute, u"shape"_s);
  EXPECT_EQ(option.type, u"enum"_s);
  EXPECT_EQ(option.values, QStringList({u"sphere"_s, u"cube"_s}));
  EXPECT_EQ(option.fallback, u"sphere"_s);
  EXPECT_TRUE(recorder.firstRejection().isEmpty());
}

TEST(XmlStateOptions, AbsentEnumTakesTheFallback) {
  QDomDocument doc;
  QString value;
  ASSERT_TRUE(PJ::readXmlEnum(element(doc, u"<cloud/>"_s), u"shape"_s, {u"a"_s}, u"a"_s, value));
  EXPECT_EQ(value, u"a"_s);
}

TEST(XmlStateOptions, RefusedEnumNamesTheValueAndTheTokens) {
  QDomDocument doc;
  const QDomElement el = element(doc, u"<cloud shape=\"blob\"/>"_s);
  PJ::XmlOptionsRecorder recorder;
  QString value = u"kept"_s;
  EXPECT_FALSE(PJ::readXmlEnum(el, u"shape"_s, {u"sphere"_s, u"cube"_s}, u"sphere"_s, value));
  EXPECT_EQ(value, u"kept"_s);
  EXPECT_EQ(recorder.firstRejection(), u"<cloud> shape=\"blob\": expected one of sphere, cube"_s);
}

TEST(XmlStateOptions, FirstReportWinsAndRejectionsDoNotOverwrite) {
  QDomDocument doc;
  const QDomElement el = element(doc, u"<cloud a=\"x\" b=\"y\"/>"_s);
  PJ::XmlOptionsRecorder recorder;
  QString value;
  EXPECT_FALSE(PJ::readXmlEnum(el, u"a"_s, {u"1"_s}, u"1"_s, value));
  EXPECT_FALSE(PJ::readXmlEnum(el, u"b"_s, {u"2"_s}, u"2"_s, value));
  EXPECT_FALSE(PJ::readXmlEnum(el, u"a"_s, {u"other"_s}, u"other"_s, value));
  ASSERT_EQ(recorder.options().size(), 2u);
  EXPECT_EQ(recorder.options().front().values, QStringList({u"1"_s}));
  EXPECT_TRUE(recorder.firstRejection().startsWith(u"<cloud> a=\"x\""_s));
}

TEST(XmlStateOptions, NestedRecorderHandsItsReportsBack) {
  QDomDocument doc;
  const QDomElement el = element(doc, u"<cloud a=\"bad\"/>"_s);
  PJ::XmlOptionsRecorder outer;
  QString value;
  {
    PJ::XmlOptionsRecorder inner;
    EXPECT_FALSE(PJ::readXmlEnum(el, u"a"_s, {u"ok"_s}, u"ok"_s, value));
    EXPECT_EQ(inner.options().size(), 1u);
    EXPECT_TRUE(outer.options().empty());
  }
  EXPECT_EQ(outer.options().size(), 1u);
  EXPECT_FALSE(outer.firstRejection().isEmpty());
}

TEST(XmlStateOptions, WithoutARecorderReadersStillValidate) {
  QDomDocument doc;
  QString value;
  EXPECT_FALSE(PJ::readXmlEnum(element(doc, u"<cloud a=\"bad\"/>"_s), u"a"_s, {u"ok"_s}, u"ok"_s, value));
}

TEST(XmlStateOptions, UnreportedAttributesAreListedPerTag) {
  QDomDocument doc;
  const QDomElement root = element(doc, u"<scene a=\"1\" b=\"2\"><layer c=\"3\"/><layer c=\"4\" d=\"5\"/></scene>"_s);
  PJ::XmlOptionsRecorder recorder;
  static_cast<void>(PJ::readXmlText(root, u"a"_s));
  static_cast<void>(PJ::readXmlText(root.firstChildElement(), u"c"_s, u"0"_s));
  EXPECT_EQ(PJ::unreportedXmlAttributes(root, recorder.options()), QStringList({u"scene.b"_s, u"layer.d"_s}));
  EXPECT_EQ(recorder.options().back().fallback, u"0"_s);
}

TEST(XmlStateOptions, ActiveOnlyWhileARecorderLives) {
  EXPECT_FALSE(PJ::XmlOptionsRecorder::active());
  {
    PJ::XmlOptionsRecorder recorder;
    EXPECT_TRUE(PJ::XmlOptionsRecorder::active());
  }
  EXPECT_FALSE(PJ::XmlOptionsRecorder::active());
}

TEST(XmlStateOptions, ToleratedValueIsNotedAndReadsAsTheFallback) {
  QDomDocument doc;
  const QDomElement el = element(doc, u"<plot style=\"Blob\" grid=\"maybe\" empty=\"\"/>"_s);
  PJ::XmlOptionsRecorder recorder;
  QString style;
  ASSERT_TRUE(PJ::readXmlEnum(el, u"style"_s, {u"Lines"_s, u"Dots"_s}, u"Lines"_s, style, PJ::XmlBadValue::kTolerate));
  EXPECT_EQ(style, u"Lines"_s);
  EXPECT_EQ(recorder.firstRejection(), u"<plot> style=\"Blob\": expected one of Lines, Dots"_s);
  bool grid = false;
  ASSERT_TRUE(PJ::readXmlBool(el, u"grid"_s, true, grid, PJ::XmlBadValue::kTolerate));
  EXPECT_TRUE(grid);
  // An empty value reads as absent when tolerated: no second rejection, the fallback.
  QString empty = u"x"_s;
  ASSERT_TRUE(PJ::readXmlEnum(el, u"empty"_s, {u"a"_s}, u"a"_s, empty, PJ::XmlBadValue::kTolerate));
  EXPECT_EQ(empty, u"a"_s);
}

TEST(XmlStateOptions, BoolReportsItsTokensTrueFirst) {
  QDomDocument doc;
  const QDomElement el = element(doc, u"<grid on=\"0\" bad=\"yes\"/>"_s);
  PJ::XmlOptionsRecorder recorder;
  bool on = true;
  ASSERT_TRUE(PJ::readXmlBool(el, u"on"_s, true, on, PJ::XmlBadValue::kRefuse, u"1"_s, u"0"_s));
  EXPECT_FALSE(on);
  EXPECT_EQ(recorder.options().front().values, QStringList({u"1"_s, u"0"_s}));
  EXPECT_EQ(recorder.options().front().fallback, u"1"_s);
  bool bad = true;
  EXPECT_FALSE(PJ::readXmlBool(el, u"bad"_s, std::nullopt, bad));
  EXPECT_TRUE(bad);
  EXPECT_EQ(recorder.firstRejection(), u"<grid> bad=\"yes\": expected true or false"_s);
}

TEST(XmlStateOptions, IntegerBoundsAreExactAndNamedInTheReason) {
  QDomDocument doc;
  const QDomElement el = element(doc, u"<layer id=\"4294967295\" order=\"4294967296\"/>"_s);
  PJ::XmlOptionsRecorder recorder;
  quint32 id = 0;
  ASSERT_TRUE(PJ::readXmlInt<quint32>(el, u"id"_s, std::nullopt, 1, 4294967295u, id));
  EXPECT_EQ(id, 4294967295u);
  EXPECT_EQ(*recorder.options().front().max, 4294967295.0);
  quint32 order = 7;
  EXPECT_FALSE(PJ::readXmlInt<quint32>(el, u"order"_s, 0, 0, 4294967295u, order));
  EXPECT_EQ(order, 7u);
  EXPECT_EQ(recorder.firstRejection(), u"<layer> order=\"4294967296\": expected an integer in [0, 4294967295]"_s);
}

TEST(XmlStateOptions, FloatBoundsAreCheckedAtFloatPrecision) {
  QDomDocument doc;
  const QDomElement el = element(doc, u"<cloud size=\"0.001\" big=\"50\"/>"_s);
  PJ::XmlOptionsRecorder recorder;
  float size = 0.0F;
  ASSERT_TRUE(PJ::readXmlFloat(el, u"size"_s, 2.0F, 0.001F, 32.0F, size));
  EXPECT_FLOAT_EQ(size, 0.001F);
  EXPECT_EQ(*recorder.options().front().min, 0.001);
  EXPECT_EQ(recorder.options().front().fallback, u"2"_s);
  float big = 1.0F;
  EXPECT_FALSE(PJ::readXmlFloat(el, u"big"_s, 2.0F, 1.0F, 32.0F, big));
  // More digits than a float holds round onto the bound, as a layout load always read them; too small for a float is
  // refused.
  QDomDocument digits_doc;
  const QDomElement digits = element(digits_doc, u"<cloud zoom=\"20.0000001\" tiny=\"1e-50\"/>"_s);
  float zoom = 0.0F;
  EXPECT_TRUE(PJ::readXmlFloat(digits, u"zoom"_s, 1.0F, 0.1F, 20.0F, zoom));
  EXPECT_FLOAT_EQ(zoom, 20.0F);
  float tiny = 1.0F;
  EXPECT_FALSE(PJ::readXmlFloat(digits, u"tiny"_s, 1.0F, 0.0F, 10.0F, tiny));
  EXPECT_EQ(recorder.firstRejection(), u"<cloud> big=\"50\": expected a number in [1, 32]"_s);
}

TEST(XmlStateOptions, OpenBoundsAreReportedAbsent) {
  QDomDocument doc;
  const QDomElement el = element(doc, u"<cam x=\"1e39\" y=\"-5\"/>"_s);
  PJ::XmlOptionsRecorder recorder;
  float x = 0.0F;
  EXPECT_FALSE(
      PJ::readXmlFloat(
          el, u"x"_s, std::nullopt, std::numeric_limits<float>::lowest(), std::numeric_limits<float>::max(), x));
  EXPECT_EQ(recorder.firstRejection(), u"<cam> x=\"1e39\": expected a finite number"_s);
  double y = 0.0;
  ASSERT_TRUE(PJ::readXmlDouble(el, u"y"_s, std::nullopt, -std::numeric_limits<double>::infinity(), 0.0, y));
  EXPECT_EQ(y, -5.0);
  for (const PJ::XmlOption& option : recorder.options()) {
    EXPECT_FALSE(option.min.has_value()) << option.attribute.toStdString();
  }
  EXPECT_EQ(*recorder.options().back().max, 0.0);
}

TEST(XmlStateOptions, ValueProblemChecksTokensAndNumbers) {
  EXPECT_TRUE(PJ::xmlValueProblem({.type = u"enum"_s, .values = {u"a"_s}}, u"a"_s).isEmpty());
  EXPECT_EQ(PJ::xmlValueProblem({.type = u"enum"_s, .values = {u"a"_s}}, u"b"_s), u"expected one of a"_s);
  EXPECT_EQ(PJ::xmlValueProblem({.type = u"int"_s, .min = 1.0}, u"0"_s), u"expected an integer >= 1"_s);
  EXPECT_EQ(PJ::xmlValueProblem({.type = u"float"_s}, u"nan"_s), u"expected a finite number"_s);
  EXPECT_TRUE(PJ::xmlValueProblem({.type = u"color"_s}, u"whatever"_s).isEmpty());
}
