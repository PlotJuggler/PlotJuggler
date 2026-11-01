// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// parseSettingsSchema: what the dock's defaults and the config panel's form are built from.

#include <gtest/gtest.h>

#include "pj_custom_widgets/SettingsSchema.h"

using namespace Qt::StringLiterals;

namespace PJ {
namespace {

TEST(SettingsSchemaTest, ReadsEveryFieldOfAnEntry) {
  const std::vector<SettingDecl> settings = parseSettingsSchema(
      uR"([{"key":"gain","type":"number","label":"Gain","default":2.5,"min":0,"max":10},
           {"key":"mode","type":"enum","options":["arc",3]}])"_s);
  ASSERT_EQ(settings.size(), 2U);
  const SettingDecl& gain = settings[0];
  EXPECT_EQ(gain.key, u"gain"_s);
  EXPECT_EQ(gain.type, u"number"_s);
  EXPECT_EQ(gain.label, u"Gain"_s);
  EXPECT_EQ(gain.default_value, QJsonValue(2.5));
  EXPECT_EQ(gain.min, 0.0);
  EXPECT_EQ(gain.max, 10.0);
  const SettingDecl& mode = settings[1];
  EXPECT_TRUE(mode.label.isEmpty());
  EXPECT_TRUE(mode.default_value.isUndefined()) << "no `default` is not a null default";
  EXPECT_FALSE(mode.min.has_value());
  EXPECT_FALSE(mode.max.has_value());
  EXPECT_EQ(mode.options, (QJsonArray{u"arc"_s, 3})) << "options keep their JSON type";
}

TEST(SettingsSchemaTest, DropsEntriesThatAreNotObjectsWithAKey) {
  const std::vector<SettingDecl> settings =
      parseSettingsSchema(uR"([7, "text", {"type":"boolean"}, {"key":"","type":"text"}, {"key":"on"}])"_s);
  ASSERT_EQ(settings.size(), 1U);
  EXPECT_EQ(settings[0].key, u"on"_s);
  EXPECT_TRUE(settings[0].type.isEmpty());
}

TEST(SettingsSchemaTest, ANullDefaultIsKeptAndANonNumericBoundIsNot) {
  const std::vector<SettingDecl> settings = parseSettingsSchema(uR"([{"key":"k","default":null,"min":"0"}])"_s);
  ASSERT_EQ(settings.size(), 1U);
  EXPECT_TRUE(settings[0].default_value.isNull());
  EXPECT_FALSE(settings[0].min.has_value());
}

TEST(SettingsSchemaTest, AnythingButAnArrayDeclaresNothing) {
  EXPECT_TRUE(parseSettingsSchema(QString()).empty());
  EXPECT_TRUE(parseSettingsSchema(u"[]"_s).empty());
  EXPECT_TRUE(parseSettingsSchema(uR"({"key":"gain"})"_s).empty());
  EXPECT_TRUE(parseSettingsSchema(u"[{\"key\":"_s).empty());
}

}  // namespace
}  // namespace PJ
