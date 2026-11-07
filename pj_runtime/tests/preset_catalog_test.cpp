// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QByteArray>
#include <QString>
#include <QUrl>

#include "pj_runtime/PresetCatalog.h"

using namespace Qt::StringLiterals;

namespace {

const QUrl kIndexUrl(u"https://example.com/app/presets/index.json"_s);

QList<PJ::PresetEntry> parse(const QByteArray& json, QString* error) {
  return PJ::parsePresetCatalog(json, kIndexUrl, error);
}

}  // namespace

TEST(PresetCatalogTest, RelativeReferencesAndDuplicateIds) {
  QString error;
  const QList<PJ::PresetEntry> entries = parse(
      R"({"presets": [
        {"id": "fr3", "title": "Franka FR3", "description": "Robot model.",
         "layout": "fr3/layout.pj4.xml", "thumbnail": "fr3/thumbnail.png"},
        {"id": "shared", "title": "Shared", "layout": "../shared/layout.pj4.xml"},
        {"id": "abs", "title": "Absolute", "layout": "/root/layout.pj4.xml",
         "thumbnail": "https://cdn.example.org/t.png"}
      ]})",
      &error);

  ASSERT_TRUE(error.isEmpty()) << error.toStdString();
  ASSERT_EQ(entries.size(), 3);
  EXPECT_EQ(entries[0].id, u"fr3"_s);
  EXPECT_EQ(entries[0].title, u"Franka FR3"_s);
  EXPECT_EQ(entries[0].description, u"Robot model."_s);
  EXPECT_EQ(entries[0].layout_url, QUrl(u"https://example.com/app/presets/fr3/layout.pj4.xml"_s));
  EXPECT_EQ(entries[0].thumbnail_url, QUrl(u"https://example.com/app/presets/fr3/thumbnail.png"_s));
  EXPECT_EQ(entries[1].layout_url, QUrl(u"https://example.com/app/shared/layout.pj4.xml"_s));
  EXPECT_TRUE(entries[1].thumbnail_url.isEmpty());
  EXPECT_TRUE(entries[1].description.isEmpty());
  EXPECT_EQ(entries[2].layout_url, QUrl(u"https://example.com/root/layout.pj4.xml"_s));
  EXPECT_EQ(entries[2].thumbnail_url, QUrl(u"https://cdn.example.org/t.png"_s));

  const QList<PJ::PresetEntry> duplicated =
      parse(R"({"presets": [{"id": "a", "layout": "a.xml"}, {"id": "a", "layout": "b.xml"}]})", &error);
  EXPECT_TRUE(duplicated.isEmpty());
  EXPECT_TRUE(error.contains(u"duplicate"_s)) << error.toStdString();
}

TEST(PresetCatalogTest, EmptyListIsNotAnError) {
  QString error = u"stale"_s;
  EXPECT_TRUE(parse(R"({"presets": []})", &error).isEmpty());
  EXPECT_TRUE(error.isEmpty());
}

TEST(PresetCatalogTest, TitleDefaultsToId) {
  QString error;
  const QList<PJ::PresetEntry> entries = parse(R"({"presets": [{"id": "fr3", "layout": "l.xml"}]})", &error);
  ASSERT_EQ(entries.size(), 1);
  EXPECT_EQ(entries[0].title, u"fr3"_s);
}

TEST(PresetCatalogTest, MalformedIndexIsAnError) {
  const char* const malformed[] = {
      "",                                                               // empty document
      "not json",                                                       // not JSON
      "[]",                                                             // root is not an object
      R"({"presets": {}})",                                             // presets is not an array
      R"({})",                                                          // presets missing
      R"({"presets": ["fr3"]})",                                        // entry is not an object
      R"({"presets": [{"layout": "l.xml"}]})",                          // id missing
      R"({"presets": [{"id": "", "layout": "l.xml"}]})",                // id empty
      R"({"presets": [{"id": 7, "layout": "l.xml"}]})",                 // id is not a string
      R"({"presets": [{"id": "a"}]})",                                  // layout missing
      R"({"presets": [{"id": "a", "layout": ""}]})",                    // layout empty
      R"({"presets": [{"id": "a", "layout": "l.xml", "title": 1}]})",   // title is not a string
      R"({"presets": [{"id": "a", "layout": "file:///etc/passwd"}]})",  // not an http(s) URL
      R"({"presets": [{"id": "a", "layout": "l.xml", "thumbnail": "data:image/png;base64,AA=="}]})",
  };
  for (const char* const json : malformed) {
    QString error;
    EXPECT_TRUE(parse(json, &error).isEmpty()) << json;
    EXPECT_FALSE(error.isEmpty()) << json;
  }
}

TEST(PresetCatalogTest, FindPresetById) {
  QString error;
  const QList<PJ::PresetEntry> entries =
      parse(R"({"presets": [{"id": "a", "layout": "a.pj4.xml"}, {"id": "b", "layout": "b.pj4.xml"}]})", &error);
  ASSERT_EQ(entries.size(), 2) << error.toStdString();
  ASSERT_NE(PJ::findPreset(entries, u"b"_s), nullptr);
  EXPECT_EQ(PJ::findPreset(entries, u"b"_s), &entries[1]);
  EXPECT_EQ(PJ::findPreset(entries, u"missing"_s), nullptr);
  EXPECT_EQ(PJ::findPreset(entries, QString()), nullptr);
}
