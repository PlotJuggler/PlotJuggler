// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QFile>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <exception>
#include <string>
#include <thread>
#include <vector>

#include "pj_base/sdk/service_registry.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/PluginSettingsKeys.h"
#include "pj_runtime/QSettingsBackend.h"

using namespace Qt::StringLiterals;

namespace {

TEST(QSettingsBackend, RegisterPluginSettingsGivesAStoreOnlyToAPluginWithAnId) {
  PJ::ServiceRegistryBuilder unnamed;
  EXPECT_EQ(PJ::registerPluginSettings(unnamed, ""), nullptr);
  EXPECT_FALSE(PJ::sdk::ServiceRegistry(unnamed.view()).get<PJ::sdk::SettingsStoreService>().has_value());

  PJ::ServiceRegistryBuilder named;
  const auto store = PJ::registerPluginSettings(named, "some-plugin");
  EXPECT_NE(store, nullptr);
  EXPECT_TRUE(PJ::sdk::ServiceRegistry(named.view()).get<PJ::sdk::SettingsStoreService>().has_value());
}

TEST(QSettingsBackend, AbsentKeyReportsNulloptAndNotContained) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::QSettingsBackend backend(dir.filePath("settings.ini"));

  EXPECT_FALSE(backend.getString("mosaico/metadata_query").has_value());
  EXPECT_FALSE(backend.getStringList("mosaico/server_history").has_value());
  EXPECT_FALSE(backend.contains("mosaico/metadata_query"));
}

TEST(QSettingsBackend, RoundTripsString) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::QSettingsBackend backend(dir.filePath("settings.ini"));

  backend.setString("mosaico/metadata_query", "topic ~ 'cam'");
  const auto value = backend.getString("mosaico/metadata_query");
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, "topic ~ 'cam'");
}

TEST(QSettingsBackend, RoundTripsStringList) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::QSettingsBackend backend(dir.filePath("settings.ini"));

  const std::vector<std::string> servers{"grpc+tls://a:1", "grpc+tls://b:2"};
  backend.setStringList("mosaico/server_history", servers);
  const auto got = backend.getStringList("mosaico/server_history");
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(*got, servers);
}

TEST(QSettingsBackend, ContainsThenRemove) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::QSettingsBackend backend(dir.filePath("settings.ini"));

  backend.setString("mosaico/range_lower", "42");
  EXPECT_TRUE(backend.contains("mosaico/range_lower"));

  backend.remove("mosaico/range_lower");
  EXPECT_FALSE(backend.contains("mosaico/range_lower"));
  EXPECT_FALSE(backend.getString("mosaico/range_lower").has_value());
}

TEST(QSettingsBackend, PersistsAcrossInstancesOnSameFile) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path = dir.filePath("settings.ini");

  {
    PJ::QSettingsBackend writer(path);
    writer.setString("mosaico/server_cache/h/api_key", "secret");
    writer.setStringList("mosaico/server_history", {"grpc+tls://a:1"});
  }

  PJ::QSettingsBackend reader(path);
  const auto key = reader.getString("mosaico/server_cache/h/api_key");
  ASSERT_TRUE(key.has_value());
  EXPECT_EQ(*key, "secret");

  const auto history = reader.getStringList("mosaico/server_history");
  ASSERT_TRUE(history.has_value());
  ASSERT_EQ(history->size(), 1U);
  EXPECT_EQ((*history)[0], "grpc+tls://a:1");
}

TEST(QSettingsBackend, PluginStoresDoNotCollideAndPersist) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path = dir.filePath("settings.ini");
  {
    auto first = PJ::QSettingsBackend::forPlugin("first-plugin", path);
    auto second = PJ::QSettingsBackend::forPlugin("second-plugin", path);
    first->setString("url", "ws://first");
    second->setString("url", "ws://second");
    second->remove("url");
    EXPECT_EQ(first->getString("url"), "ws://first");
  }
  EXPECT_EQ(PJ::QSettingsBackend::forPlugin("first-plugin", path)->getString("url"), "ws://first");
  QSettings file(path, QSettings::IniFormat);
  EXPECT_EQ(file.value(PJ::pluginSettingsGroup("first-plugin") + u"/url"_s).toString(), u"ws://first"_s);
  EXPECT_FALSE(file.contains(u"url"_s));
}

TEST(QSettingsBackend, PluginIdCannotReachIntoAnotherPluginsStore) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path = dir.filePath("settings.ini");
  auto outer = PJ::QSettingsBackend::forPlugin("a", path);
  auto nested = PJ::QSettingsBackend::forPlugin("a/b", path);
  outer->setString("b/key", "outer");
  EXPECT_FALSE(nested->contains("key"));
}

TEST(QSettingsBackend, NoPluginIdNoStore) {
  EXPECT_EQ(PJ::QSettingsBackend::forPlugin(""), nullptr);
}

// Plugin keys written before stores were scoped sit at the root of the app settings. A store reads its own through
// and keeps a copy; removing one removes the root copy too, or the next read would bring it back.
TEST(QSettingsBackend, AStoreReadsItsLegacyRootKeysThrough) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path = dir.filePath("settings.ini");
  {
    QSettings root(path, QSettings::IniFormat);
    root.setValue(u"assistant.backend"_s, u"codex"_s);
    root.setValue(u"assistant.conv.transcript"_s, u"old"_s);
    root.setValue(u"mcap_cloud/server_history"_s, QStringList{u"a"_s, u"b"_s});
    root.setValue(u"source_presentation/v1/YQ/display_name"_s, u"Run 1"_s);
  }
  auto assistant = PJ::QSettingsBackend::forPlugin("toolbox-assistant-agent", path);
  EXPECT_EQ(assistant->getString("assistant.backend"), "codex");
  EXPECT_EQ(
      PJ::QSettingsBackend::forPlugin("mcap-cloud", path)->getStringList("mcap_cloud/server_history"),
      (std::vector<std::string>{"a", "b"}));
  EXPECT_EQ(
      PJ::QSettingsBackend::forPlugin("toolbox-mosaico", path)->getString("source_presentation/v1/YQ/display_name"),
      "Run 1");
  EXPECT_TRUE(assistant->contains("assistant.conv.transcript"));
  assistant->remove("assistant.conv.transcript");
  EXPECT_FALSE(assistant->contains("assistant.conv.transcript"));

  QSettings file(path, QSettings::IniFormat);
  EXPECT_EQ(
      file.value(PJ::pluginSettingsGroup("toolbox-assistant-agent") + u"/assistant.backend"_s).toString(), u"codex"_s);
  EXPECT_EQ(file.value(u"assistant.backend"_s).toString(), u"codex"_s);
  EXPECT_FALSE(file.contains(u"assistant.conv.transcript"_s));
}

TEST(QSettingsBackend, TheAppsAndOtherPluginsKeysAreNotReadThrough) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path = dir.filePath("settings.ini");
  const QStringList foreign{
      u"StyleSheet::theme"_s,          u"MainWindow.streamingSource"_s, u"ui/icon_size"_s,
      u"pj_scene3d/urdf_browse_dir"_s, u"PluginConfig/agent"_s,         u"DialogGeometry/agent"_s,
      u"PluginSettings/other/agent"_s, u"BrowserPersistence/agent"_s,   u"mosaico/metadata_query"_s,
  };
  {
    QSettings root(path, QSettings::IniFormat);
    for (const QString& key : foreign) {
      root.setValue(key, u"x"_s);
    }
  }
  // An id that names the app's lowercase groups still reaches none of them.
  for (const char* id : {"toolbox-assistant-agent", "ui-pj-scene3d"}) {
    auto store = PJ::QSettingsBackend::forPlugin(id, path);
    for (const QString& key : foreign) {
      EXPECT_FALSE(store->contains(key.toStdString())) << id << " " << key.toStdString();
      EXPECT_FALSE(store->getString(key.toStdString()).has_value()) << id << " " << key.toStdString();
    }
  }
}

// pj.settings.v1 is GUI-thread only: a call from another thread fails instead of racing the QSettings.
TEST(QSettingsBackend, RefusesCallsOffItsThread) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  auto store = PJ::QSettingsBackend::forPlugin("bridge", dir.filePath("settings.ini"));
  bool threw = false;
  std::thread([&] {
    try {
      store->setString("durable/url", "ws://robot:9871");
    } catch (const std::exception&) {
      threw = true;
    }
  }).join();
  EXPECT_TRUE(threw);
  EXPECT_FALSE(store->contains("durable/url"));
}

TEST(QSettingsBackend, AWriteThatCannotBeStoredFails) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  QFile blocker(dir.filePath("not-a-dir"));
  ASSERT_TRUE(blocker.open(QIODevice::WriteOnly));
  blocker.close();
  auto store = PJ::QSettingsBackend::forPlugin("bridge", dir.filePath("not-a-dir/settings.ini"));
  EXPECT_THROW(store->setString("durable/url", "ws://robot:9871"), std::exception);
  EXPECT_THROW(store->setStringList("history", {"a"}), std::exception);
}

}  // namespace
