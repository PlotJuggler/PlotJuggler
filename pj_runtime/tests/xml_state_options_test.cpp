// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QDomDocument>

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
