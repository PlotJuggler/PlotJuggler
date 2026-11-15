// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// appSettings() is one long-lived QSettings per thread, read by the browser's
// hot paths (appSetting()). It may only replace a
// fresh QSettings() on a hot path if it never reads a stale value: every write,
// remove and clear made through any other default QSettings, on any thread,
// before or after that instance is destroyed, must be visible through it at once.
// Checked in the native format and in IniFormat, the browser's session store.

#include <gtest/gtest.h>

#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>
#include <thread>

#include "pj_runtime/QSettingsBackend.h"
#include "pj_widgets/AppSettings.h"
#include "pj_widgets/SvgUtil.h"
using namespace Qt::StringLiterals;

namespace {

const QString kKey = u"AppSettingsTest/value"_s;

QString read() {
  return PJ::appSettings().value(kKey, u"fallback"_s).toString();
}

class AppSettingsTest : public ::testing::TestWithParam<QSettings::Format> {
 protected:
  void SetUp() override {
    ASSERT_TRUE(dir_.isValid());
    saved_format_ = QSettings::defaultFormat();
    QSettings::setDefaultFormat(GetParam());
    QSettings::setPath(GetParam(), QSettings::UserScope, dir_.path());
    QSettings::setPath(GetParam(), QSettings::SystemScope, dir_.path() + u"/system"_s);
  }
  void TearDown() override {
    QSettings::setDefaultFormat(saved_format_);
  }

  // Each case runs on a fresh thread, so its appSettings() instance is created
  // under this case's format and path rather than reused from another test.
  template <typename Body>
  static void onFreshThread(Body body) {
    std::thread thread(body);
    thread.join();
  }

  QTemporaryDir dir_;
  QSettings::Format saved_format_ = QSettings::NativeFormat;
};

// (a) a write through another instance; (b) a remove through it.
TEST_P(AppSettingsTest, SeesWritesAndRemovesThroughAnotherLiveInstance) {
  onFreshThread([] {
    EXPECT_EQ(read(), u"fallback"_s);  // the instance now exists
    QSettings other;
    other.setValue(kKey, u"one"_s);
    EXPECT_EQ(read(), u"one"_s);
    other.remove(kKey);
    EXPECT_EQ(read(), u"fallback"_s);
  });
}

// (c) the same once the writing instance is gone (it syncs on destruction).
TEST_P(AppSettingsTest, SeesWritesAndRemovesAfterTheWriterIsDestroyed) {
  onFreshThread([] {
    EXPECT_EQ(read(), u"fallback"_s);
    QSettings().setValue(kKey, u"two"_s);
    EXPECT_EQ(read(), u"two"_s);
    QSettings().remove(kKey);
    EXPECT_EQ(read(), u"fallback"_s);
  });
}

// (d) a write from another thread's QSettings.
TEST_P(AppSettingsTest, SeesWritesFromAnotherThread) {
  onFreshThread([] {
    EXPECT_EQ(read(), u"fallback"_s);
    std::thread([] { QSettings().setValue(kKey, u"three"_s); }).join();
    EXPECT_EQ(read(), u"three"_s);
    std::thread([] {
      QSettings settings;
      settings.setValue(kKey, u"four"_s);
      settings.sync();
    }).join();
    EXPECT_EQ(read(), u"four"_s);
  });
}

// (e) a clear through another instance.
TEST_P(AppSettingsTest, SeesClearThroughAnotherInstance) {
  onFreshThread([] {
    QSettings().setValue(kKey, u"five"_s);
    EXPECT_EQ(read(), u"five"_s);
    QSettings other;
    other.clear();
    EXPECT_EQ(read(), u"fallback"_s);
  });
}

// (f) a write and a remove through the plugin settings service, which keeps its
// own long-lived QSettings.
TEST_P(AppSettingsTest, SeesThePluginSettingsService) {
  onFreshThread([] {
    EXPECT_EQ(read(), u"fallback"_s);
    PJ::QSettingsBackend backend;
    backend.setString(kKey.toStdString(), "six");
    EXPECT_EQ(read(), u"six"_s);
    backend.setStringList(kKey.toStdString(), {"seven"});
    EXPECT_EQ(PJ::appSettings().value(kKey).toStringList(), QStringList{u"seven"_s});
    backend.remove(kKey.toStdString());
    EXPECT_EQ(read(), u"fallback"_s);
  });
}

// The hot readers built on it: a theme or precision change saved the way the
// Preferences dialog and Theme save it (a plain QSettings) is read back at once.
TEST_P(AppSettingsTest, ThemeAndPrecisionFollowPlainWrites) {
  onFreshThread([] {
    QSettings().setValue(QString::fromLatin1(PJ::kThemeSettingsKey), u"dark"_s);
    QSettings().setValue(QString::fromLatin1(PJ::kPrecisionSettingsKey), 6);
    EXPECT_EQ(PJ::currentTheme(), u"dark"_s);
    EXPECT_EQ(PJ::valuePrecision(), 6);
    EXPECT_EQ(PJ::appSettings().value(QString::fromLatin1(PJ::kThemeSettingsKey)).toString(), u"dark"_s);
    QSettings().setValue(QString::fromLatin1(PJ::kThemeSettingsKey), u"light"_s);
    QSettings().remove(QString::fromLatin1(PJ::kPrecisionSettingsKey));
    EXPECT_EQ(PJ::currentTheme(), u"light"_s);
    EXPECT_EQ(PJ::valuePrecision(), 3);
    EXPECT_FALSE(PJ::appSettings().contains(QString::fromLatin1(PJ::kPrecisionSettingsKey)));
  });
}

INSTANTIATE_TEST_SUITE_P(
    Formats, AppSettingsTest, ::testing::Values(QSettings::NativeFormat, QSettings::IniFormat),
    [](const ::testing::TestParamInfo<QSettings::Format>& param_info) {
      return param_info.param == QSettings::IniFormat ? "IniFormat" : "NativeFormat";
    });

}  // namespace
