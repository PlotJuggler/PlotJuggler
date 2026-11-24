// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDomDocument>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
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

  static QHash<QString, QString> decodePluginSettings(const QVariant& stored) {
    return BrowserPersistence::decodePluginSettings(stored);
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
      u"MainWindow.dateTimeDisplay"_s,
      u"Preferences::clock_utc"_s,
      u"MainWindow.timeTrackerSetting"_s,
      u"MainWindow.legendStatus"_s,
      u"MainWindow.panelBottomExpandedHeight"_s,
      u"MainWindow.streamingBufferValue"_s,
      u"MainWindow.streamingSource"_s,
      u"MainWindow.cloudSource"_s,
      u"CurveListPanel/show_topics"_s,
      u"CurveListPanel/show_values"_s,
      u"EasterEgg/next_date"_s,
      u"EasterEgg/character"_s,
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

TEST(BrowserPersistencePolicy, OnlyPluginKeysUnderDurableAreKept) {
  const QString url = u"ws://robot.local:9871"_s;
  EXPECT_EQ(BrowserPersistence::sanitizeDurablePluginSetting(u"PluginSettings/some-bridge/durable/url"_s, url), url);
  EXPECT_EQ(
      BrowserPersistence::sanitizeDurablePluginSetting(u"PluginSettings/some-bridge/durable/history/0"_s, url), url);

  for (const QString& key : {
           u"PluginSettings/some-bridge/url"_s,
           u"PluginSettings/some-bridge/durable"_s,
           u"PluginSettings/some-bridge/durable/"_s,
           u"PluginSettings//durable/url"_s,
           u"PluginSettings/some-bridge/nested/durable/url"_s,
           u"PluginConfig/some-bridge/durable/url"_s,
           u"durable/url"_s,
           u"StyleSheet::theme"_s,
       }) {
    EXPECT_FALSE(BrowserPersistence::sanitizeDurablePluginSetting(key, url).has_value()) << key.toStdString();
  }
}

TEST(BrowserPersistencePolicy, DurablePluginValuesAreBoundedStrings) {
  const QString key = u"PluginSettings/some-bridge/durable/url"_s;
  const qsizetype cap = BrowserPersistence::kMaxDurablePluginValueLength;
  EXPECT_TRUE(BrowserPersistence::sanitizeDurablePluginSetting(key, QString(cap, u'x')).has_value());
  EXPECT_FALSE(BrowserPersistence::sanitizeDurablePluginSetting(key, QString(cap + 1, u'x')).has_value());
  EXPECT_TRUE(BrowserPersistence::sanitizeDurablePluginSetting(key, QString()).has_value());

  EXPECT_FALSE(BrowserPersistence::sanitizeDurablePluginSetting(key, QStringList{u"a"_s, u"b"_s}).has_value());
  EXPECT_FALSE(BrowserPersistence::sanitizeDurablePluginSetting(key, 42).has_value());
  EXPECT_FALSE(BrowserPersistence::sanitizeDurablePluginSetting(key, QVariant{}).has_value());
  EXPECT_FALSE(BrowserPersistence::sanitizeDurablePluginSetting(key, u"pj-upload://dead"_s).has_value());
  EXPECT_FALSE(BrowserPersistence::sanitizeDurablePluginSetting(key, u"/pj_uploads/dead/run.mcap"_s).has_value());
  EXPECT_FALSE(BrowserPersistence::sanitizeDurablePluginSetting(key, u"line\nbreak"_s).has_value());
}

TEST(BrowserPersistencePolicy, CollectsDurablePluginSettingsAndDropsTheRest) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  QSettings settings(dir.filePath(u"session.ini"_s), QSettings::IniFormat);
  settings.setValue(u"PluginSettings/bridge/durable/url"_s, u"ws://robot:9871"_s);
  settings.setValue(u"PluginSettings/bridge/token"_s, u"secret"_s);
  settings.setValue(u"PluginSettings/other/durable/url"_s, u"ws://other:8765"_s);
  settings.setValue(
      u"PluginSettings/other/durable/blob"_s, QString(BrowserPersistence::kMaxDurablePluginValueLength + 1, u'x'));
  settings.setValue(u"PluginConfig/bridge"_s, u"{\"url\":\"ws://robot:9871\"}"_s);
  settings.setValue(u"StyleSheet::theme"_s, u"dark"_s);
  // One plugin past the per-plugin cap keeps the first keys in key order.
  for (int index = 0; index <= BrowserPersistence::kMaxDurablePluginKeysPerPlugin; ++index) {
    settings.setValue(u"PluginSettings/chatty/durable/k%1"_s.arg(index), u"v"_s);
  }

  const QHash<QString, QString> kept = BrowserPersistence::durablePluginSettings(settings);
  EXPECT_EQ(kept.value(u"PluginSettings/bridge/durable/url"_s), u"ws://robot:9871"_s);
  EXPECT_EQ(kept.value(u"PluginSettings/other/durable/url"_s), u"ws://other:8765"_s);
  EXPECT_FALSE(kept.contains(u"PluginSettings/bridge/token"_s));
  EXPECT_FALSE(kept.contains(u"PluginSettings/other/durable/blob"_s));
  EXPECT_FALSE(kept.contains(u"PluginConfig/bridge"_s));
  EXPECT_FALSE(kept.contains(u"StyleSheet::theme"_s));
  qsizetype chatty = 0;
  for (auto it = kept.cbegin(); it != kept.cend(); ++it) {
    chatty += it.key().startsWith(u"PluginSettings/chatty/"_s) ? 1 : 0;
  }
  EXPECT_EQ(chatty, BrowserPersistence::kMaxDurablePluginKeysPerPlugin);
  EXPECT_FALSE(
      kept.contains(u"PluginSettings/chatty/durable/k%1"_s.arg(BrowserPersistence::kMaxDurablePluginKeysPerPlugin)));
  EXPECT_EQ(kept.size(), 2 + BrowserPersistence::kMaxDurablePluginKeysPerPlugin);
}

TEST(BrowserPersistencePolicy, StoredPluginSettingsAreRecheckedOnRestore) {
  const QByteArray stored = R"({"version":1,"values":{
    "PluginSettings/bridge/durable/url":"ws://robot:9871",
    "PluginSettings/bridge/token":"secret",
    "PluginSettings/bridge/durable/count":3,
    "StyleSheet::theme":"dark"
  }})";
  const QHash<QString, QString> restored = BrowserPersistenceTestPeer::decodePluginSettings(stored);
  EXPECT_EQ(restored, (QHash<QString, QString>{{u"PluginSettings/bridge/durable/url"_s, u"ws://robot:9871"_s}}));

  EXPECT_TRUE(BrowserPersistenceTestPeer::decodePluginSettings(QVariant{}).isEmpty());
  EXPECT_TRUE(BrowserPersistenceTestPeer::decodePluginSettings(u"not bytes"_s).isEmpty());
  EXPECT_TRUE(BrowserPersistenceTestPeer::decodePluginSettings(QByteArray("not JSON")).isEmpty());
  EXPECT_TRUE(
      BrowserPersistenceTestPeer::decodePluginSettings(
          QByteArray(R"({"version":2,"values":{"PluginSettings/bridge/durable/url":"ws://robot:9871"}})"))
          .isEmpty());
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
  // Cloud-materialized source records are source-bound: one outside <previouslyLoaded_Datafiles> in a hand-crafted
  // document must still be rejected.
  EXPECT_FALSE(
      BrowserPersistence::isSafeGenericLayoutRecipe(
          "<root pj4_version=\"4\" binding=\"generic\"><materialize provider=\"p\"/></root>"));
  EXPECT_FALSE(BrowserPersistence::isSafeGenericLayoutRecipe(QByteArray(256 * 1024 + 1, 'x')));
}

TEST(BrowserPersistenceTest, StreamRecordsNeverBecomeDurableRecipes) {
  // A stream record carries plugin configuration (endpoints, maybe credentials), so neither it nor its identity may
  // enter durable browser storage, even in a document marked generic.
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

  // The shell retains an opened browser layout as a recipe only when it is not source-bound; a stream record
  // alone makes a layout source-bound.
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
