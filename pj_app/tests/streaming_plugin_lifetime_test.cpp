// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <memory>

#include "LayoutXml.h"
#include "StreamingSourceManager.h"
#include "pj_runtime/AppSession.h"

#ifndef PJ_MOCK_LAYOUT_STREAM_PLUGIN_PATH
#error "PJ_MOCK_LAYOUT_STREAM_PLUGIN_PATH must be defined"
#endif

using namespace Qt::StringLiterals;

namespace PJ {
namespace {

constexpr auto kProbeEnv = "PJ_LAYOUT_STREAM_PROBE_FILE";

class StreamingPluginLifetimeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(extensions_dir_.isValid());
    ASSERT_TRUE(probe_dir_.isValid());
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

  [[nodiscard]] QStringList servicesProbe() const {
    QFile file(probe_dir_.filePath(u"stream.probe"_s));
    if (!file.open(QIODevice::ReadOnly)) {
      return {};
    }
    return QString::fromUtf8(file.readAll())
        .split(u'\n', Qt::SkipEmptyParts)
        .filter(QRegularExpression(u"^services "_s));
  }

  QTemporaryDir extensions_dir_;
  QTemporaryDir probe_dir_;
  std::unique_ptr<AppSession> app_session_;
  std::unique_ptr<StreamingSourceManager> manager_;
};

TEST_F(StreamingPluginLifetimeTest, AStreamReachesItsServicesFromStartAndItsDestructor) {
  const QString config = uR"({"probe_services":true})"_s;
  layout_xml::StreamingSourceRef source;
  source.id = u"stream"_s;
  source.plugin_id = u"Mock Layout Stream"_s;
  source.plugin_manifest_id = u"mock-layout-stream"_s;
  source.plugin_config_json = config;
  QString error;
  const DatasetId dataset = manager_->startFromLayout(source, config, error);
  ASSERT_NE(dataset, 0U) << error.toStdString();
  ASSERT_TRUE(manager_->stopDatasetAndWait(dataset, QString{}));

  EXPECT_EQ(servicesProbe(), (QStringList{u"services start:ok"_s, u"services destroy:ok"_s}));
}

}  // namespace
}  // namespace PJ
