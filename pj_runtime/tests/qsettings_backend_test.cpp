// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QFile>
#include <QFileDevice>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <exception>
#include <string>
#include <thread>
#include <vector>

#include "pj_runtime/PluginSettingsKeys.h"
#include "pj_runtime/QSettingsBackend.h"
#include "recording_test_utils.h"

#ifndef Q_OS_WIN
#include <unistd.h>  // geteuid: root ignores the file permissions the unwritable-file tests rely on
#endif

using namespace Qt::StringLiterals;

namespace {

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
  EXPECT_EQ(PJ::QSettingsBackend::forPlugin("", path), nullptr);
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

// Writes `keys` (each to its own name) at the root of the INI file at `path`.
void seedRoot(const QString& path, const QStringList& keys) {
  QSettings root(path, QSettings::IniFormat);
  for (const QString& key : keys) {
    root.setValue(key, key);
  }
}

// A store's first open copies the root keys its plugin may own into it; the root is never written.
TEST(QSettingsBackend, TheFirstOpenCopiesThePluginsRootKeysOnce) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path = dir.filePath("settings.ini");
  seedRoot(path, {u"assistant.backend"_s, u"agent/history"_s});
  { QSettings(path, QSettings::IniFormat).setValue(u"mcap_cloud/server_history"_s, QStringList{u"a"_s, u"b"_s}); }

  (void)PJ::QSettingsBackend::forPlugin("toolbox-assistant-agent", path);
  (void)PJ::QSettingsBackend::forPlugin("mcap-cloud", path);
  seedRoot(path, {u"assistant.late"_s});
  { QSettings(path, QSettings::IniFormat).setValue(u"assistant.backend"_s, u"changed"_s); }

  auto assistant = PJ::QSettingsBackend::forPlugin("toolbox-assistant-agent", path);
  EXPECT_EQ(assistant->getString("assistant.backend"), "assistant.backend");
  EXPECT_EQ(assistant->getString("agent/history"), "agent/history");
  EXPECT_FALSE(assistant->contains("assistant.late"));
  EXPECT_EQ(
      PJ::QSettingsBackend::forPlugin("mcap-cloud", path)->getStringList("mcap_cloud/server_history"),
      (std::vector<std::string>{"a", "b"}));
}

TEST(QSettingsBackend, RemovingACopiedKeyLeavesTheRootAndStaysRemoved) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path = dir.filePath("settings.ini");
  seedRoot(path, {u"bridge/url"_s});
  {
    auto store = PJ::QSettingsBackend::forPlugin("bridge", path);
    ASSERT_TRUE(store->contains("bridge/url"));
    store->remove("bridge/url");
    EXPECT_FALSE(store->contains("bridge/url"));
  }
  EXPECT_FALSE(PJ::QSettingsBackend::forPlugin("bridge", path)->contains("bridge/url"));
  EXPECT_EQ(QSettings(path, QSettings::IniFormat).value(u"bridge/url"_s).toString(), u"bridge/url"_s);
}

TEST(QSettingsBackend, AnIdWithUnderscoresMatchesItsRootKeys) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path = dir.filePath("settings.ini");
  seedRoot(path, {u"my_plugin/url"_s, u"my-plugin.mode"_s});
  auto store = PJ::QSettingsBackend::forPlugin("my_plugin", path);
  EXPECT_EQ(store->getString("my_plugin/url"), "my_plugin/url");
  EXPECT_EQ(store->getString("my-plugin.mode"), "my-plugin.mode");
}

TEST(QSettingsBackend, TheAppsAndOtherPluginsKeysAreNotCopied) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path = dir.filePath("settings.ini");
  const QStringList foreign{
      u"StyleSheet::theme"_s,
      u"MainWindow.streamingSource"_s,
      u"ui/icon_size"_s,
      u"pj_scene3d/urdf_browse_dir"_s,
      u"PluginConfig/agent"_s,
      u"DialogGeometry/agent"_s,
      u"PluginSettings/other/agent"_s,
      u"BrowserPersistence/agent"_s,
      u"mosaico/metadata_query"_s,
      u"source_presentation/v1/YQ/display_name"_s,
  };
  seedRoot(path, foreign);
  // An id that names the app's lowercase groups still reaches none of them.
  for (const char* id : {"toolbox-assistant-agent", "ui-pj-scene3d"}) {
    auto store = PJ::QSettingsBackend::forPlugin(id, path);
    for (const QString& key : foreign) {
      EXPECT_FALSE(store->contains(key.toStdString())) << id << " " << key.toStdString();
    }
  }
}

// The store's own bookkeeping in its group is no key of the plugin's.
TEST(QSettingsBackend, AStoresBookkeepingIsInvisibleToItsPlugin) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path = dir.filePath("settings.ini");
  auto store = PJ::QSettingsBackend::forPlugin("bridge", path);
  QSettings file(path, QSettings::IniFormat);
  file.beginGroup(PJ::pluginSettingsGroup("bridge"));
  const QStringList bookkeeping = file.allKeys();
  ASSERT_EQ(bookkeeping.size(), 1);
  const std::string key = bookkeeping.front().toStdString();
  EXPECT_FALSE(store->contains(key));
  EXPECT_FALSE(store->getString(key).has_value());
  EXPECT_THROW(store->setString(key, "x"), std::exception);
  EXPECT_THROW(store->setString("/" + key + "/", "x"), std::exception);
  store->remove(key);
  EXPECT_TRUE(file.contains(bookkeeping.front()));
}

TEST(QSettingsBackend, AScopedStoreRefusesAnEmptyKey) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  auto store = PJ::QSettingsBackend::forPlugin("bridge", dir.filePath("settings.ini"));
  EXPECT_THROW(store->setString("", "x"), std::exception);
  EXPECT_THROW(store->setString("/", "x"), std::exception);
  EXPECT_THROW((void)store->getString(""), std::exception);
}

// pj.settings.v1 is GUI-thread only: every call from another thread fails instead of racing the QSettings.
TEST(QSettingsBackend, RefusesCallsOffItsThread) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  auto store = PJ::QSettingsBackend::forPlugin("bridge", dir.filePath("settings.ini"));
  store->setString("kept", "yes");
  int threw = 0;
  std::thread([&] {
    const auto attempt = [&](const auto& call) {
      try {
        call();
      } catch (const std::exception&) {
        ++threw;
      }
    };
    attempt([&] { store->setString("durable/url", "ws://robot:9871"); });
    attempt([&] { store->setStringList("list", {"a"}); });
    attempt([&] { (void)store->getString("kept"); });
    attempt([&] { (void)store->getStringList("kept"); });
    attempt([&] { (void)store->contains("kept"); });
    attempt([&] { store->remove("kept"); });
  }).join();
  EXPECT_EQ(threw, 6);
  EXPECT_FALSE(store->contains("durable/url"));
  EXPECT_EQ(store->getString("kept"), "yes");
}

bool runsAsRoot() {
#ifndef Q_OS_WIN
  return geteuid() == 0;
#else
  return false;
#endif
}

// A write QSettings cannot store throws and changes nothing the store reads back.
TEST(QSettingsBackend, AWriteThatCannotBeStoredFails) {
  if (runsAsRoot()) {
    GTEST_SKIP() << "running as root: file permissions cannot make the settings file unwritable";
  }
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path = dir.filePath("settings.ini");
  { PJ::QSettingsBackend::forPlugin("bridge", path)->setString("url", "ws://old"); }
  ASSERT_TRUE(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::ReadUser));

  auto store = PJ::QSettingsBackend::forPlugin("bridge", path);
  EXPECT_THROW(store->setString("url", "ws://new"), std::exception);
  EXPECT_THROW(store->setStringList("history", {"a"}), std::exception);
  EXPECT_EQ(store->getString("url"), "ws://old");
  EXPECT_FALSE(store->contains("history"));
  ASSERT_TRUE(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
}

// A file QSettings could not fully parse still takes writes.
TEST(QSettingsBackend, AMalformedLineDoesNotFailLaterWrites) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path = dir.filePath("settings.ini");
  {
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("[General]\nthis line has no equals sign\n\"unterminated=1\n");
  }
  {
    auto store = PJ::QSettingsBackend::forPlugin("bridge", path);
    EXPECT_NO_THROW(store->setString("url", "ws://robot:9871"));
    EXPECT_EQ(store->getString("url"), "ws://robot:9871");
  }
  EXPECT_EQ(PJ::QSettingsBackend::forPlugin("bridge", path)->getString("url"), "ws://robot:9871");
}

// A remove QSettings cannot store throws and leaves the key in place.
TEST(QSettingsBackend, ARemoveThatCannotBeStoredFails) {
  if (runsAsRoot()) {
    GTEST_SKIP() << "running as root: file permissions cannot make the settings file unwritable";
  }
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path = dir.filePath("settings.ini");
  { PJ::QSettingsBackend::forPlugin("bridge", path)->setString("url", "ws://old"); }
  ASSERT_TRUE(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::ReadUser));

  auto store = PJ::QSettingsBackend::forPlugin("bridge", path);
  EXPECT_THROW(store->remove("url"), std::exception);
  EXPECT_EQ(store->getString("url"), "ws://old");
  ASSERT_TRUE(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
}

// Qt's INI writer stores an empty list as @Invalid(): the key is present and its value is empty.
TEST(QSettingsBackend, AnEmptyListIsPresentAfterARestart) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path = dir.filePath("settings.ini");
  {
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("[PluginSettings]\nbridge\\h=@Invalid()\n");
  }
  {
    auto store = PJ::QSettingsBackend::forPlugin("bridge", path);
    EXPECT_TRUE(store->contains("h"));
    EXPECT_EQ(store->getStringList("h"), std::vector<std::string>{});
    EXPECT_EQ(store->getString("h"), "");
    store->setStringList("cleared", {});
  }
  auto store = PJ::QSettingsBackend::forPlugin("bridge", path);
  EXPECT_TRUE(store->contains("cleared"));
  EXPECT_EQ(store->getStringList("cleared"), std::vector<std::string>{});
}

// A plugin's store sees only the app's own settings file: no system-scope or organization-wide fallback.
TEST(QSettingsBackend, AStoreReadsAndCopiesNoFallbackFile) {
  const PJ::test::IsolatedQtSettings isolated(u"PJSettingsTestOrg"_s, u"PJSettingsTestApp"_s);
  ASSERT_TRUE(isolated.isValid());
  QTemporaryDir system_dir;
  ASSERT_TRUE(system_dir.isValid());
  QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, system_dir.path());
  for (const QString& file :
       {system_dir.filePath(u"PJSettingsTestOrg/PJSettingsTestApp.ini"_s),
        system_dir.filePath(u"PJSettingsTestOrg.ini"_s)}) {
    QSettings system(file, QSettings::IniFormat);
    system.setValue(u"bridge/url"_s, u"ws://system"_s);
    system.setValue(PJ::pluginSettingsGroup("bridge") + u"/leak"_s, u"system"_s);
  }
  ASSERT_EQ(QSettings().value(u"bridge/url"_s).toString(), u"ws://system"_s) << "the fallback is not in effect";

  auto store = PJ::QSettingsBackend::forPlugin("bridge");
  EXPECT_FALSE(store->contains("bridge/url"));
  EXPECT_FALSE(store->contains("leak"));
}

// The reserved key matches in any case: Windows registry and INI keys are case-insensitive.
TEST(QSettingsBackend, TheReservedKeyIsReservedInAnyCase) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path = dir.filePath("settings.ini");
  auto store = PJ::QSettingsBackend::forPlugin("bridge", path);
  for (const std::string key : {"__PJ_ROOT_KEYS_COPIED__", "__Pj_Root_Keys_Copied__"}) {
    EXPECT_FALSE(store->contains(key)) << key;
    EXPECT_THROW(store->setString(key, "x"), std::exception) << key;
    store->remove(key);
  }
  QSettings file(path, QSettings::IniFormat);
  file.beginGroup(PJ::pluginSettingsGroup("bridge"));
  EXPECT_EQ(file.allKeys().size(), 1);
}

// An unscoped backend refuses an empty key too: QSettings::remove("") would clear the whole file.
TEST(QSettingsBackend, AnUnscopedBackendRefusesAnEmptyKey) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::QSettingsBackend backend(dir.filePath("settings.ini"));
  backend.setString("kept", "yes");
  EXPECT_THROW(backend.remove(""), std::exception);
  EXPECT_THROW(backend.remove("/"), std::exception);
  EXPECT_THROW(backend.setString("", "x"), std::exception);
  EXPECT_EQ(backend.getString("kept"), "yes");
}

}  // namespace
