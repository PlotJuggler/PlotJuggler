// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
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
using PJ::detail::kSavedFilepathKey;
using PJ::detail::parseDisplayName;
using PJ::detail::parseDisplaySuffix;
using PJ::detail::rewriteReplayFilepaths;
using PJ::detail::SavedFilepath;
using PJ::detail::stripHostReplayKeys;
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

  const QJsonDocument rewritten = QJsonDocument::fromJson(
      QByteArray::fromStdString(rewriteReplayFilepaths(config, u"/fresh/source.mcap"_s, SavedFilepath::kOmit).value()));
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
      QJsonDocument::fromJson(
          QByteArray::fromStdString(rewriteReplayFilepaths(config, u"fresh.csv"_s, SavedFilepath::kOmit).value()))
          .object();
  EXPECT_EQ(rewritten.value(u"filepath"_s).toString(), u"fresh.csv"_s);
  EXPECT_EQ(rewritten.value(u"__pj_fanout"_s).toArray()[0].toString(), u"not json"_s);
}

TEST(RewriteReplayFilepaths, EmptyConfigBecomesMinimalObject) {
  const QString fresh = QDir::cleanPath(QDir::tempPath() + u"/fresh.csv"_s);
  const std::string rewritten = rewriteReplayFilepaths("", fresh, SavedFilepath::kRecord).value();
  EXPECT_EQ(rewritten, QJsonDocument(QJsonObject{{u"filepath"_s, fresh}}).toJson(QJsonDocument::Compact).toStdString());
}

TEST(RewriteReplayFilepaths, RecordsTheReplacedPathOnTheOuterObjectAndEveryFanoutChild) {
  const QString root = QDir::cleanPath(QDir::tempPath());
  const QString old_top = root + u"/old/top.csv"_s;
  const QString old_child = root + u"/old/child.csv"_s;
  const QString fresh = root + u"/new/top.csv"_s;
  const QString child = QString::fromUtf8(
      QJsonDocument(QJsonObject{{u"filepath"_s, old_child}, {u"keep"_s, 1}}).toJson(QJsonDocument::Compact));
  const QJsonObject input{{u"filepath"_s, old_top}, {u"__pj_fanout"_s, QJsonArray{child}}};
  const std::string config = QJsonDocument(input).toJson(QJsonDocument::Compact).toStdString();

  const QJsonObject out =
      QJsonDocument::fromJson(
          QByteArray::fromStdString(rewriteReplayFilepaths(config, fresh, SavedFilepath::kRecord).value()))
          .object();
  const QString key{kSavedFilepathKey};
  EXPECT_EQ(out.value(u"filepath"_s).toString(), fresh);
  EXPECT_EQ(out.value(key).toString(), old_top);
  const QJsonObject out_child =
      QJsonDocument::fromJson(out.value(u"__pj_fanout"_s).toArray()[0].toString().toUtf8()).object();
  EXPECT_EQ(out_child.value(u"filepath"_s).toString(), fresh);
  EXPECT_EQ(out_child.value(key).toString(), old_child);
  EXPECT_EQ(out_child.value(u"keep"_s).toInt(), 1);
}

TEST(RewriteReplayFilepaths, RecordsNothingWhenOmittedOrTheOldValueCannotBeRebased) {
  const QString root = QDir::cleanPath(QDir::tempPath());
  const QString fresh = root + u"/new/run.csv"_s;
  const auto has_key = [&](const std::string& config, SavedFilepath saved) {
    const QJsonObject out =
        QJsonDocument::fromJson(QByteArray::fromStdString(rewriteReplayFilepaths(config, fresh, saved).value()))
            .object();
    return out.contains(u"__pj_saved_filepath"_s);
  };
  const auto with_old = [](const QJsonValue& value) {
    return QJsonDocument(QJsonObject{{u"filepath"_s, value}}).toJson(QJsonDocument::Compact).toStdString();
  };
  EXPECT_FALSE(has_key(with_old(root + u"/old/run.csv"_s), SavedFilepath::kOmit));
  EXPECT_TRUE(has_key(with_old(root + u"/old/run.csv"_s), SavedFilepath::kRecord));
  EXPECT_FALSE(has_key(with_old(u"run.csv"_s), SavedFilepath::kRecord));
  EXPECT_FALSE(has_key(with_old(u"pj-upload://x/run.csv"_s), SavedFilepath::kRecord));
  EXPECT_FALSE(has_key(with_old(u"s3://b/x"_s), SavedFilepath::kRecord));
  EXPECT_FALSE(has_key(with_old(root + u"/other/../new/./run.csv"_s), SavedFilepath::kRecord));
  EXPECT_FALSE(has_key(R"({"keep":1})", SavedFilepath::kRecord));
  EXPECT_FALSE(has_key(with_old(5), SavedFilepath::kRecord));
}

TEST(RewriteReplayFilepaths, DropsAStaleSavedFilepathItCannotVouchFor) {
  const QString root = QDir::cleanPath(QDir::tempPath());
  const QString fresh = root + u"/new/run.csv"_s;
  const QJsonObject input{{u"filepath"_s, u"run.csv"_s}, {u"__pj_saved_filepath"_s, root + u"/stale.csv"_s}};
  const std::string config = QJsonDocument(input).toJson(QJsonDocument::Compact).toStdString();
  for (const SavedFilepath saved : {SavedFilepath::kOmit, SavedFilepath::kRecord}) {
    const QJsonObject out =
        QJsonDocument::fromJson(QByteArray::fromStdString(rewriteReplayFilepaths(config, fresh, saved).value()))
            .object();
    EXPECT_FALSE(out.contains(u"__pj_saved_filepath"_s));
    EXPECT_EQ(out.value(u"filepath"_s).toString(), fresh);
  }
}

TEST(RewriteReplayFilepaths, RefusesANonEmptyConfigThatIsNotAJsonObject) {
  for (const char* input : {"not json", "[1]", "null"}) {
    EXPECT_FALSE(rewriteReplayFilepaths(input, u"fresh.csv"_s, SavedFilepath::kRecord).has_value()) << input;
  }
}

TEST(StripHostReplayKeys, RemovesTheKeyFromTheOuterObjectAndNestedFanoutChildren) {
  const std::string grandchild = R"({"filepath":"g","__pj_saved_filepath":"/g"})";
  const QJsonObject child{
      {u"filepath"_s, u"c"_s},
      {u"__pj_saved_filepath"_s, u"/c"_s},
      {u"__pj_fanout"_s, QJsonArray{QString::fromStdString(grandchild)}}};
  const QJsonObject input{
      {u"filepath"_s, u"t"_s},
      {u"__pj_saved_filepath"_s, u"/t"_s},
      {u"keep"_s, 1},
      {u"__pj_fanout"_s,
       QJsonArray{QString::fromUtf8(QJsonDocument(child).toJson(QJsonDocument::Compact)), u"not json"_s}}};
  const std::string stripped = stripHostReplayKeys(QJsonDocument(input).toJson(QJsonDocument::Compact).toStdString());
  EXPECT_EQ(stripped.find("__pj_saved_filepath"), std::string::npos) << stripped;
  const QJsonObject out = QJsonDocument::fromJson(QByteArray::fromStdString(stripped)).object();
  EXPECT_EQ(out.value(u"keep"_s).toInt(), 1);
  EXPECT_EQ(out.value(u"__pj_fanout"_s).toArray()[1].toString(), u"not json"_s);
}

TEST(StripHostReplayKeys, LeavesConfigsWithoutTheKeyByteForByte) {
  const std::string config = R"({ "b":1,  "a":2 })";
  EXPECT_EQ(stripHostReplayKeys(config), config);
}

TEST(StripHostReplayKeys, LeavesNonObjectConfigsUntouched) {
  for (const std::string config :
       {std::string("not json __pj_saved_filepath"), std::string(R"(["__pj_saved_filepath"])")}) {
    EXPECT_EQ(stripHostReplayKeys(config), config);
  }
}

}  // namespace
