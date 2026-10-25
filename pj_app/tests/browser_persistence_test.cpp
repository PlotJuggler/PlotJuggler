// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDomDocument>
#include <QString>
#include <QVariant>

#include "BrowserPersistence.h"
#include "LayoutXml.h"
using namespace Qt::StringLiterals;

namespace PJ {

class BrowserPersistenceTestPeer {
 public:
  enum class Status {
    kAbsent,
    kValid,
    kRejected,
  };

  static Status decodeStatus(const QVariant& stored) {
    switch (BrowserPersistence::decodePreferences(stored).status) {
      case BrowserPersistence::PreferenceDecodeStatus::kAbsent:
        return Status::kAbsent;
      case BrowserPersistence::PreferenceDecodeStatus::kValid:
        return Status::kValid;
      case BrowserPersistence::PreferenceDecodeStatus::kRejected:
        return Status::kRejected;
    }
    return Status::kRejected;
  }

  static QHash<QString, QVariant> decodeValues(const QVariant& stored) {
    return BrowserPersistence::decodePreferences(stored).values;
  }
};

namespace {

TEST(BrowserPersistencePolicy, PreferenceEnvelopeDistinguishesAbsentValidAndRejectedStorage) {
  using Status = BrowserPersistenceTestPeer::Status;

  EXPECT_EQ(BrowserPersistenceTestPeer::decodeStatus(QVariant{}), Status::kAbsent);
  EXPECT_EQ(BrowserPersistenceTestPeer::decodeStatus(QByteArray(R"({"version":1,"values":{}})")), Status::kValid);

  EXPECT_EQ(BrowserPersistenceTestPeer::decodeStatus(u"wrong QVariant type"_s), Status::kRejected);
  EXPECT_EQ(BrowserPersistenceTestPeer::decodeStatus(QByteArray("not JSON")), Status::kRejected);
  EXPECT_EQ(BrowserPersistenceTestPeer::decodeStatus(QByteArray(R"({"version":2,"values":{}})")), Status::kRejected);
  EXPECT_EQ(BrowserPersistenceTestPeer::decodeStatus(QByteArray(64 * 1024 + 1, 'x')), Status::kRejected);
}

TEST(BrowserPersistencePolicy, PreferenceEnvelopeRestoresOnlySanitizedValues) {
  const QByteArray stored = R"({"version":1,"values":{
    "StyleSheet::theme":"dark",
    "Preferences::precision":"4",
    "MainWindow.buttonDots":true,
    "FileLoader/lastDir":"/private/path"
  }})";

  const QHash<QString, QVariant> values = BrowserPersistenceTestPeer::decodeValues(stored);
  EXPECT_EQ(values.size(), 2);
  EXPECT_EQ(values.value(u"StyleSheet::theme"_s), QVariant(u"dark"_s));
  EXPECT_EQ(values.value(u"MainWindow.buttonDots"_s), QVariant(true));
}

TEST(BrowserPersistencePolicy, DurablePreferenceAllowlistIsAnExactClosedSet) {
  const QStringList expected{
      u"StyleSheet::theme"_s,
      u"Preferences::precision"_s,
      u"Preferences::curve_color_global"_s,
      u"Preferences::check_updates_on_startup"_s,
      u"Preferences::splash_mode"_s,
      u"Preferences::auto_zoom_plots"_s,
      u"Preferences::dialog_geometry"_s,
      u"ui/icon_size"_s,
      u"ui/icon_padding"_s,
      u"ui/layout_padding"_s,
      u"ui/layout_spacing"_s,
      u"MainWindow.buttonShowpoint"_s,
      u"MainWindow.buttonActivateGrid"_s,
      u"MainWindow.buttonDots"_s,
      u"MainWindow.buttonRatio"_s,
      u"MainWindow.buttonLink"_s,
      u"MainWindow.useTimeOffset"_s,
      u"MainWindow.timeTrackerSetting"_s,
      u"MainWindow.legendStatus"_s,
      u"MainWindow.panelBottomExpandedHeight"_s,
      u"MainWindow.streamingBufferValue"_s,
      u"MainWindow.streamingSource"_s,
      u"MainWindow.cloudSource"_s,
      u"CurveListPanel/show_topics"_s,
      u"CurveListPanel/show_values"_s,
  };

  EXPECT_EQ(BrowserPersistence::durablePreferenceKeys(), expected);
}

TEST(BrowserPersistencePolicy, AllowsOnlySchemaOwnedPreferences) {
  EXPECT_TRUE(BrowserPersistence::isDurablePreferenceKey(u"StyleSheet::theme"_s));
  EXPECT_TRUE(BrowserPersistence::isDurablePreferenceKey(u"Preferences::precision"_s));
  EXPECT_TRUE(BrowserPersistence::isDurablePreferenceKey(u"MainWindow.streamingBufferValue"_s));
  EXPECT_TRUE(BrowserPersistence::isDurablePreferenceKey(u"CurveListPanel/show_values"_s));

  EXPECT_FALSE(BrowserPersistence::isDurablePreferenceKey(u"FileLoader/lastDir"_s));
  EXPECT_FALSE(BrowserPersistence::isDurablePreferenceKey(u"File/recent"_s));
  EXPECT_FALSE(BrowserPersistence::isDurablePreferenceKey(u"Layout/recent"_s));
  EXPECT_FALSE(BrowserPersistence::isDurablePreferenceKey(u"PluginConfig/CSV Loader"_s));
  EXPECT_FALSE(BrowserPersistence::isDurablePreferenceKey(u"DialogGeometry/plugin"_s));
  EXPECT_FALSE(BrowserPersistence::isDurablePreferenceKey(u"pj.settings.v1/arbitrary"_s));
  EXPECT_FALSE(BrowserPersistence::isDurablePreferenceKey(u"MainWindow.lastLayoutDirectory"_s));
  EXPECT_FALSE(BrowserPersistence::isDurablePreferenceKey(u"pj_scene3d/urdf_browse_dir"_s));
}

TEST(BrowserPersistencePolicy, RejectsWrongTypesRangesAndPathShapedIdentifiers) {
  EXPECT_EQ(BrowserPersistence::sanitizePreference(u"Preferences::precision"_s, 4), QVariant(4));
  EXPECT_FALSE(BrowserPersistence::sanitizePreference(u"Preferences::precision"_s, 0).has_value());
  EXPECT_FALSE(BrowserPersistence::sanitizePreference(u"Preferences::precision"_s, u"4"_s).has_value());

  EXPECT_EQ(BrowserPersistence::sanitizePreference(u"MainWindow.buttonDots"_s, true), QVariant(true));
  EXPECT_FALSE(BrowserPersistence::sanitizePreference(u"MainWindow.buttonDots"_s, 1).has_value());

  EXPECT_EQ(BrowserPersistence::sanitizePreference(u"StyleSheet::theme"_s, u"dark"_s), QVariant(u"dark"_s));
  EXPECT_FALSE(BrowserPersistence::sanitizePreference(u"StyleSheet::theme"_s, u"system"_s).has_value());
  EXPECT_FALSE(BrowserPersistence::sanitizePreference(u"Preferences::splash_mode"_s, u"other"_s).has_value());

  EXPECT_TRUE(BrowserPersistence::sanitizePreference(u"MainWindow.streamingSource"_s, u"MQTT Source"_s).has_value());
  EXPECT_FALSE(
      BrowserPersistence::sanitizePreference(u"MainWindow.streamingSource"_s, u"/home/user/source"_s).has_value());
  EXPECT_FALSE(BrowserPersistence::sanitizePreference(u"MainWindow.cloudSource"_s, u"pj-upload://dead"_s).has_value());

  EXPECT_TRUE(
      BrowserPersistence::sanitizePreference(u"Preferences::dialog_geometry"_s, QByteArray(4096, 'x')).has_value());
  EXPECT_FALSE(
      BrowserPersistence::sanitizePreference(u"Preferences::dialog_geometry"_s, QByteArray(4097, 'x')).has_value());
}

TEST(BrowserPersistencePolicy, AcceptsOnlyBoundedSourceFreeLayoutRecipes) {
  const QByteArray safe = "<root pj4_version=\"4\" binding=\"generic\"><tabbed_widget/></root>";
  EXPECT_TRUE(BrowserPersistence::isSafeGenericLayoutRecipe(safe));
  EXPECT_FALSE(BrowserPersistence::isSafeGenericLayoutRecipe("<root pj4_version=\"4\" binding=\"source\"/>"));
  EXPECT_FALSE(
      BrowserPersistence::isSafeGenericLayoutRecipe(
          "<root pj4_version=\"4\" binding=\"generic\"><previouslyLoaded_Datafiles/></root>"));
  EXPECT_FALSE(
      BrowserPersistence::isSafeGenericLayoutRecipe(
          "<root pj4_version=\"4\" binding=\"generic\"><x>pj-upload://dead</x></root>"));
  // Cloud-materialized source records are source-bound by definition; even a
  // hand-crafted document carrying one outside <previouslyLoaded_Datafiles>
  // must never enter durable browser storage.
  EXPECT_FALSE(
      BrowserPersistence::isSafeGenericLayoutRecipe(
          "<root pj4_version=\"4\" binding=\"generic\"><materialize provider=\"p\"/></root>"));
  EXPECT_FALSE(BrowserPersistence::isSafeGenericLayoutRecipe(QByteArray(256 * 1024 + 1, 'x')));
}

TEST(BrowserPersistenceTest, StreamRecordsNeverBecomeDurableRecipes) {
  // A stream record carries plugin configuration (endpoints, maybe credentials), so neither the
  // record nor its identity may enter durable browser storage, even in a document marked generic.
  const QByteArray stream_block =
      "<root pj4_version=\"5\" binding=\"generic\"><previouslyLoaded_StreamingSources/></root>";
  const QByteArray nested_record =
      "<root pj4_version=\"5\" binding=\"generic\"><tabbed_widget><streamInfo id=\"robot\"/></tabbed_widget></root>";
  const QByteArray stream_qualifier =
      "<root pj4_version=\"4\" binding=\"generic\"><plot><curve "
      "dataset_path=\"pj-stream&#x3a;//robot\"/></plot></root>";
  for (const QByteArray& xml : {stream_block, nested_record, stream_qualifier}) {
    EXPECT_FALSE(BrowserPersistence::isSafeGenericLayoutRecipe(xml)) << xml.toStdString();
  }
  BrowserPersistence persistence;
  QString error;
  EXPECT_FALSE(persistence.rememberGenericLayout(u"robot.pj4.xml"_s, stream_block, &error));
  EXPECT_FALSE(error.isEmpty());
  EXPECT_TRUE(persistence.recentLayouts().isEmpty());

  // The shell retains an opened browser layout as a recipe only when it is not source-bound:
  // a stream record alone makes a layout source-bound.
  const auto source_bound = [](const char* xml) {
    QDomDocument doc;
    EXPECT_TRUE(static_cast<bool>(doc.setContent(QByteArray(xml)))) << xml;
    return layout_xml::isSourceBoundLayout(doc);
  };
  EXPECT_TRUE(source_bound(
      R"(<root binding="source"><previouslyLoaded_StreamingSources><streamInfo id="robot">)"
      R"(<plugin ID="PlotJuggler Bridge"><![CDATA[{"url":"ws://10.0.0.2:9871"}]]></plugin>)"
      R"(</streamInfo></previouslyLoaded_StreamingSources></root>)"));
  EXPECT_TRUE(source_bound(
      R"(<root binding="source"><previouslyLoaded_Datafiles><fileInfo filename="run.csv"/>)"
      R"(</previouslyLoaded_Datafiles></root>)"));
  EXPECT_FALSE(source_bound(
      R"(<root binding="generic"><previouslyLoaded_StreamingSources><streamInfo id="robot"/>)"
      R"(</previouslyLoaded_StreamingSources></root>)"));
  EXPECT_FALSE(source_bound(R"(<root binding="source"><tabbed_widget/></root>)"));
}

TEST(BrowserPersistencePolicy, RecentRecipesAreDeduplicatedBoundedAndClearable) {
  BrowserPersistence persistence;
  for (int index = 0; index < 7; ++index) {
    const QByteArray xml =
        u"<root pj4_version=\"4\" binding=\"generic\"><profile index=\"%1\"/></root>"_s.arg(index).toUtf8();
    EXPECT_TRUE(persistence.rememberGenericLayout(u"layout-%1.pj4.xml"_s.arg(index), xml));
  }
  ASSERT_EQ(persistence.recentLayouts().size(), 5);
  EXPECT_EQ(persistence.recentLayouts().front().name, u"layout-6.pj4.xml"_s);
  EXPECT_EQ(persistence.recentLayouts().back().name, u"layout-2.pj4.xml"_s);

  const QByteArray replacement = "<root pj4_version=\"4\" binding=\"generic\"><profile replacement=\"true\"/></root>";
  EXPECT_TRUE(persistence.rememberGenericLayout(u"layout-4.pj4.xml"_s, replacement));
  ASSERT_EQ(persistence.recentLayouts().size(), 5);
  EXPECT_EQ(persistence.recentLayouts().front().name, u"layout-4.pj4.xml"_s);
  EXPECT_EQ(persistence.recentLayouts().front().xml, replacement);

  persistence.clearRecentLayouts();
  EXPECT_TRUE(persistence.recentLayouts().isEmpty());
}

}  // namespace
}  // namespace PJ
