// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A streaming source gets the pj.settings.v1 store, scoped to its manifest id: what it stores in one session is
// there in the next, its destructor may still write, and a write from its poll thread fails cleanly.

#include <gtest/gtest.h>

#include <QSettings>
#include <QString>
#include <QStringList>
#include <functional>

#include "pj_runtime/PluginSettingsKeys.h"
#include "support/loader_test_support.h"
#include "support/stream_manager_fixture.h"

#ifndef PJ_MOCK_LAYOUT_STREAM_PLUGIN_PATH
#error "PJ_MOCK_LAYOUT_STREAM_PLUGIN_PATH must be defined"
#endif

using namespace Qt::StringLiterals;

namespace PJ {
namespace {

class StreamingPluginSettingsTest : public pj_app_test::StreamManagerFixture {
 protected:
  // Starts the test stream with `config`, runs `until` (if given) and stops it, joining its worker.
  void runSession(const QString& config, const std::function<bool()>& until = {}) {
    QString error;
    const DatasetId dataset = startFromLayout(config, error);
    ASSERT_NE(dataset, 0U) << error.toStdString();
    if (until) {
      ASSERT_TRUE(pj_app_test::pumpUntil(until));
    }
    ASSERT_TRUE(manager_->stopDatasetAndWait(dataset, QString{}));
  }

  [[nodiscard]] QStringList settingsLines() const {
    return probeLines().filter(u"settings"_s);
  }

  [[nodiscard]] static QString stored() {
    return QSettings().value(pluginSettingsGroup("mock-layout-stream") + u"/remembered"_s).toString();
  }
};

TEST_F(StreamingPluginSettingsTest, AValueStoredInOneSessionIsReadInTheNext) {
  runSession(uR"({"remember":"ws://robot:9871"})"_s);
  runSession(uR"({"remember":""})"_s);

  EXPECT_EQ(settingsLines(), (QStringList{u"settings (absent)"_s, u"settings ws://robot:9871"_s}));
  EXPECT_EQ(stored(), u"ws://robot:9871"_s);
  EXPECT_FALSE(QSettings().contains(u"remembered"_s));
}

TEST_F(StreamingPluginSettingsTest, ThePluginDestructorCanStillWrite) {
  runSession(uR"({"remember_on_destroy":"ws://last:9871"})"_s);
  EXPECT_EQ(stored(), u"ws://last:9871"_s);
}

TEST_F(StreamingPluginSettingsTest, AWriteFromThePollThreadFailsAndStoresNothing) {
  runSession(uR"({"remember_on_poll":"ws://poll:9871"})"_s, [this] { return !settingsLines().isEmpty(); });
  EXPECT_EQ(settingsLines(), QStringList{u"poll settings failed"_s});
  EXPECT_TRUE(stored().isEmpty());
}

}  // namespace
}  // namespace PJ
