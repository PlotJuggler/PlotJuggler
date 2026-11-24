#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A StreamingSourceManager over a real AppSession with the test stream (mock_layout_stream_plugin.cpp) staged per
// case, its settings in a directory of the case's own and its probe lines in a file of the case's own. The including
// test defines PJ_MOCK_LAYOUT_STREAM_PLUGIN_PATH.

#include <gtest/gtest.h>

#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <memory>

#include "LayoutXml.h"
#include "StreamingSourceManager.h"
#include "loader_test_support.h"
#include "pj_runtime/AppSession.h"

namespace pj_app_test {

class StreamManagerFixture : public ::testing::Test {
 protected:
  static constexpr auto kProbeEnv = "PJ_LAYOUT_STREAM_PROBE_FILE";

  void SetUp() override {
    ASSERT_TRUE(extensions_dir_.isValid());
    ASSERT_TRUE(settings_dir_.isValid());
    ASSERT_TRUE(probe_dir_.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir_.path());
    const QString plugin = QString::fromUtf8(PJ_MOCK_LAYOUT_STREAM_PLUGIN_PATH);
    ASSERT_TRUE(QFile::copy(plugin, extensions_dir_.filePath(QFileInfo(plugin).fileName())));
    qputenv(kProbeEnv, probeFile().toLocal8Bit());

    app_session_ = std::make_unique<PJ::AppSession>(extensions_dir_.path());
    manager_ = std::make_unique<PJ::StreamingSourceManager>(
        app_session_->sessionManager(), app_session_->extensionCatalog(), app_session_->catalogModel(),
        app_session_->topicDemandTracker(), /*dialog_parent=*/nullptr);
  }

  void TearDown() override {
    if (manager_ != nullptr) {
      manager_->stopAllAndWait(QString{});
    }
    manager_.reset();
    app_session_.reset();
    qunsetenv(kProbeEnv);
  }

  [[nodiscard]] QString probeFile() const {
    return probe_dir_.filePath(QStringLiteral("stream.probe"));
  }

  [[nodiscard]] QStringList probeLines() const {
    return pj_app_test::probeLines(probeFile());
  }

  // Starts the test stream from a layout record with `config`, no dialog. 0 with `error` set on failure.
  [[nodiscard]] PJ::DatasetId startFromLayout(const QString& config, QString& error) {
    PJ::layout_xml::StreamingSourceRef source;
    source.id = QStringLiteral("stream");
    source.plugin_id = QStringLiteral("Mock Layout Stream");
    source.plugin_manifest_id = QStringLiteral("mock-layout-stream");
    source.plugin_config_json = config;
    return manager_->startFromLayout(source, config, error);
  }

  QTemporaryDir extensions_dir_;
  QTemporaryDir settings_dir_;
  QTemporaryDir probe_dir_;
  std::unique_ptr<PJ::AppSession> app_session_;
  std::unique_ptr<PJ::StreamingSourceManager> manager_;
};

}  // namespace pj_app_test
