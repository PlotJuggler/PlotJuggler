// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <string>
#include <vector>

#include "FanoutConfig.h"
using namespace Qt::StringLiterals;

namespace {

using PJ::detail::extractFanout;
using PJ::detail::parseDisplayName;
using PJ::detail::parseDisplaySuffix;
using PJ::detail::rewriteReplayFilepaths;
using Vec = std::vector<std::string>;

// --- extractFanout: fallback-to-single-instance paths -----------------------

TEST(ExtractFanout, EmptyConfigFallsBackToSingleInstance) {
  EXPECT_EQ(extractFanout(""), Vec{""});
}

TEST(ExtractFanout, NonObjectJsonReturnsConfigVerbatim) {
  EXPECT_EQ(extractFanout("not json"), Vec{"not json"});
  EXPECT_EQ(extractFanout("[1,2]"), Vec{"[1,2]"});
}

TEST(ExtractFanout, MissingKeyRunsSingleInstance) {
  const std::string cfg = R"({"foo":1})";
  EXPECT_EQ(extractFanout(cfg), Vec{cfg});
}

TEST(ExtractFanout, NonArrayValueFallsBack) {
  const std::string cfg = R"({"__pj_fanout":"x"})";
  EXPECT_EQ(extractFanout(cfg), Vec{cfg});
}

TEST(ExtractFanout, EmptyArrayFallsBack) {
  const std::string cfg = R"({"__pj_fanout":[]})";
  EXPECT_EQ(extractFanout(cfg), Vec{cfg});
}

// --- extractFanout: fanout paths -------------------------------------------

TEST(ExtractFanout, HappyPathReturnsEntriesInOrder) {
  const std::string cfg = R"({"__pj_fanout":["a","b"]})";
  EXPECT_EQ(extractFanout(cfg), (Vec{"a", "b"}));
}

// A malformed (non-string) entry must be skipped, not silently collapse the
// whole array — the survivors are still fanned out.
TEST(ExtractFanout, NonStringEntriesAreSkipped) {
  const std::string cfg = R"({"__pj_fanout":["a",5,"b"]})";
  EXPECT_EQ(extractFanout(cfg), (Vec{"a", "b"}));
}

// If skipping leaves nothing usable, fall back to single-instance — NOT an
// empty vector (an empty vector would send loadFile into the fanout branch with
// zero iterations, importing nothing while reporting success).
TEST(ExtractFanout, AllNonStringEntriesFallBackToSingleInstance) {
  const std::string cfg = R"({"__pj_fanout":[1,2]})";
  EXPECT_EQ(extractFanout(cfg), Vec{cfg});
}

// --- parseDisplaySuffix -----------------------------------------------------

TEST(ParseDisplaySuffix, EmptyConfigReturnsFallback) {
  EXPECT_EQ(parseDisplaySuffix("", u"fb"_s).toStdString(), std::string("fb"));
}

TEST(ParseDisplaySuffix, NonObjectReturnsFallback) {
  EXPECT_EQ(parseDisplaySuffix("not json", u"fb"_s).toStdString(), std::string("fb"));
}

TEST(ParseDisplaySuffix, MissingKeyReturnsFallback) {
  EXPECT_EQ(parseDisplaySuffix(R"({"foo":"bar"})", u"fb"_s).toStdString(), std::string("fb"));
}

TEST(ParseDisplaySuffix, NonStringValueReturnsFallback) {
  EXPECT_EQ(parseDisplaySuffix(R"({"display_suffix":7})", u"fb"_s).toStdString(), std::string("fb"));
}

TEST(ParseDisplaySuffix, EmptyStringValueReturnsFallback) {
  EXPECT_EQ(parseDisplaySuffix(R"({"display_suffix":""})", u"fb"_s).toStdString(), std::string("fb"));
}

TEST(ParseDisplaySuffix, PresentValueReturnsSuffix) {
  EXPECT_EQ(parseDisplaySuffix(R"({"display_suffix":"cam_left"})", u"fb"_s).toStdString(), std::string("cam_left"));
}

// --- parseDisplayName -------------------------------------------------------

TEST(ParseDisplayName, EmptyConfigReturnsEmpty) {
  EXPECT_TRUE(parseDisplayName("").isEmpty());
}

TEST(ParseDisplayName, NonObjectReturnsEmpty) {
  EXPECT_TRUE(parseDisplayName("not json").isEmpty());
  EXPECT_TRUE(parseDisplayName("[1,2]").isEmpty());
}

TEST(ParseDisplayName, MissingKeyReturnsEmpty) {
  EXPECT_TRUE(parseDisplayName(R"({"foo":"bar"})").isEmpty());
}

TEST(ParseDisplayName, NonStringValueReturnsEmpty) {
  EXPECT_TRUE(parseDisplayName(R"({"display_name":7})").isEmpty());
}

TEST(ParseDisplayName, EmptyStringValueReturnsEmpty) {
  EXPECT_TRUE(parseDisplayName(R"({"display_name":""})").isEmpty());
}

TEST(ParseDisplayName, PresentValueReturnsName) {
  EXPECT_EQ(parseDisplayName(R"({"display_name":"pusht_v21"})").toStdString(), std::string("pusht_v21"));
}

// --- rewriteReplayFilepaths -------------------------------------------------

TEST(RewriteReplayFilepaths, RewritesTopLevelAndRecursiveFanoutChildren) {
  const std::string nested =
      R"({"filepath":"old-inner","display_suffix":"left","__pj_fanout":["{\"filepath\":\"old-leaf\",\"keep\":3}"]})";
  QJsonObject input;
  input.insert(u"filepath"_s, u"old-top"_s);
  input.insert(u"keep"_s, QJsonObject{{u"filepath"_s, u"not-a-source"_s}});
  input.insert(u"__pj_fanout"_s, QJsonArray{QString::fromStdString(nested), 7});
  const std::string config = QJsonDocument(input).toJson(QJsonDocument::Compact).toStdString();

  const QJsonDocument rewritten =
      QJsonDocument::fromJson(QByteArray::fromStdString(rewriteReplayFilepaths(config, u"/fresh/source.mcap"_s)));
  ASSERT_TRUE(rewritten.isObject());
  const QJsonObject root = rewritten.object();
  EXPECT_EQ(root.value(u"filepath"_s).toString(), u"/fresh/source.mcap"_s);
  EXPECT_EQ(root.value(u"keep"_s).toObject().value(u"filepath"_s).toString(), u"not-a-source"_s)
      << "only explicit __pj_fanout children belong to the source replay contract";

  const QJsonArray first_fanout = root.value(u"__pj_fanout"_s).toArray();
  ASSERT_EQ(first_fanout.size(), 2);
  EXPECT_EQ(first_fanout[1].toInt(), 7);
  const QJsonObject child = QJsonDocument::fromJson(first_fanout[0].toString().toUtf8()).object();
  EXPECT_EQ(child.value(u"filepath"_s).toString(), u"/fresh/source.mcap"_s);
  EXPECT_EQ(child.value(u"display_suffix"_s).toString(), u"left"_s);
  const QJsonObject leaf =
      QJsonDocument::fromJson(child.value(u"__pj_fanout"_s).toArray()[0].toString().toUtf8()).object();
  EXPECT_EQ(leaf.value(u"filepath"_s).toString(), u"/fresh/source.mcap"_s);
  EXPECT_EQ(leaf.value(u"keep"_s).toInt(), 3);
}

TEST(RewriteReplayFilepaths, PreservesMalformedNestedFanoutString) {
  const std::string config = R"({"filepath":"old","__pj_fanout":["not json"]})";
  const QJsonObject rewritten =
      QJsonDocument::fromJson(QByteArray::fromStdString(rewriteReplayFilepaths(config, u"fresh.csv"_s))).object();
  EXPECT_EQ(rewritten.value(u"filepath"_s).toString(), u"fresh.csv"_s);
  EXPECT_EQ(rewritten.value(u"__pj_fanout"_s).toArray()[0].toString(), u"not json"_s);
}

TEST(RewriteReplayFilepaths, EmptyOrMalformedOuterConfigBecomesMinimalObject) {
  for (const std::string& input : {std::string{}, std::string{"not json"}}) {
    const QJsonObject rewritten =
        QJsonDocument::fromJson(QByteArray::fromStdString(rewriteReplayFilepaths(input, u"fresh.csv"_s))).object();
    EXPECT_EQ(rewritten.size(), 1);
    EXPECT_EQ(rewritten.value(u"filepath"_s).toString(), u"fresh.csv"_s);
  }
}

}  // namespace
