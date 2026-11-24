// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A streaming source gets the pj.settings.v1 store, scoped to its manifest id: what it stores in one session is
// there in the next (the test stream, mock_layout_stream_plugin.cpp, reads and writes it in loadConfig).

#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <memory>

#include "LayoutXml.h"
#include "StreamingSourceManager.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/PluginSettingsKeys.h"

#ifndef PJ_MOCK_LAYOUT_STREAM_PLUGIN_PATH
#error "PJ_MOCK_LAYOUT_STREAM_PLUGIN_PATH must be defined"
#endif

using namespace Qt::StringLiterals;

namespace PJ {
namespace {

constexpr auto kProbeEnv = "PJ_LAYOUT_STREAM_PROBE_FILE";

class StreamingPluginSettingsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(extensions_dir_.isValid());
    ASSERT_TRUE(settings_dir_.isValid());
    ASSERT_TRUE(probe_dir_.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir_.path());
    const QString plugin = QString::fromUtf8(PJ_MOCK_LAYOUT_STREAM_PLUGIN_PATH);
    ASSERT_TRUE(QFile::copy(plugin, extensions_dir_.filePath(QFileInfo(plugin).fileName())));
    qputenv(kProbeEnv, probe_dir_.filePath(u"stream.probe"_s).toLocal8Bit());

    app_session_ = std::make_unique<AppSession>(extensions_dir_.path());
    manager_ = std::make_unique<StreamingSourceManager>(
        app_session_->sessionManager(), app_session_->extensionCatalog(), app_session_->catalogModel(),
        app_session_->topicDemandTracker(), /*dialog_parent=*/nullptr);
  }

  void TearDown() override {
    manager_->stopAllAndWait(QString{});
    manager_.reset();
    app_session_.reset();
    qunsetenv(kProbeEnv);
  }

  // Starts the test stream from a layout record with `config`, then stops it and joins its worker.
  void runSession(const QString& config) {
    layout_xml::StreamingSourceRef source;
    source.id = u"stream"_s;
    source.plugin_id = u"Mock Layout Stream"_s;
    source.plugin_manifest_id = u"mock-layout-stream"_s;
    source.plugin_config_json = config;
    QString error;
    const DatasetId dataset = manager_->startFromLayout(source, config, error);
    ASSERT_NE(dataset, 0U) << error.toStdString();
    ASSERT_TRUE(manager_->stopDatasetAndWait(dataset, QString{}));
  }

  // The "settings ..." lines the test stream recorded, in order.
  [[nodiscard]] QStringList settingsProbe() const {
    QFile file(probe_dir_.filePath(u"stream.probe"_s));
    if (!file.open(QIODevice::ReadOnly)) {
      return {};
    }
    return QString::fromUtf8(file.readAll())
        .split(u'\n', Qt::SkipEmptyParts)
        .filter(QRegularExpression(u"^settings "_s));
  }

  QTemporaryDir extensions_dir_;
  QTemporaryDir settings_dir_;
  QTemporaryDir probe_dir_;
  std::unique_ptr<AppSession> app_session_;
  std::unique_ptr<StreamingSourceManager> manager_;
};

TEST_F(StreamingPluginSettingsTest, AValueStoredInOneSessionIsReadInTheNext) {
  runSession(uR"({"remember":"ws://robot:9871"})"_s);
  runSession(uR"({"remember":""})"_s);

  EXPECT_EQ(settingsProbe(), (QStringList{u"settings (absent)"_s, u"settings ws://robot:9871"_s}));
  // Stored in the plugin's own group of the app settings, never among the app's root keys.
  const QSettings app;
  EXPECT_EQ(app.value(pluginSettingsGroup("mock-layout-stream") + u"/remembered"_s).toString(), u"ws://robot:9871"_s);
  EXPECT_FALSE(app.contains(u"remembered"_s));
}

}  // namespace
}  // namespace PJ
