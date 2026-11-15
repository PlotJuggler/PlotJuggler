// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QSettings>
#include <QString>
#include <QVariant>

#include "pj_widgets/SettingsCache.h"
#include "pj_widgets/SvgUtil.h"
using namespace Qt::StringLiterals;

namespace {

const QString kKey = QString::fromLatin1(PJ::kThemeSettingsKey);

// Restores the user's real theme value whatever the test leaves behind.
class SettingsCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    saved_ = QSettings().value(kKey);
    PJ::invalidateCachedSettings();
  }
  void TearDown() override {
    if (saved_.isValid()) {
      QSettings().setValue(kKey, saved_);
    } else {
      QSettings().remove(kKey);
    }
    PJ::invalidateCachedSettings();
  }
  QVariant saved_;
};

TEST_F(SettingsCacheTest, WriteIsObservedByTheNextRead) {
  PJ::writeCachedSetting(kKey, u"dark"_s);
  EXPECT_EQ(PJ::currentTheme(), u"dark"_s);
  PJ::writeCachedSetting(kKey, u"light"_s);
  EXPECT_EQ(PJ::currentTheme(), u"light"_s);
  // The write reached QSettings too, so other processes and readers see it.
  EXPECT_EQ(QSettings().value(kKey).toString(), u"light"_s);
}

TEST_F(SettingsCacheTest, DirectWritesNeedAnInvalidate) {
  PJ::writeCachedSetting(kKey, u"light"_s);
  QSettings().setValue(kKey, u"dark"_s);
  EXPECT_EQ(PJ::currentTheme(), u"light"_s);  // the documented contract: cached
  PJ::invalidateCachedSettings();
  EXPECT_EQ(PJ::currentTheme(), u"dark"_s);
}

TEST_F(SettingsCacheTest, AbsentKeyReadsTheFallback) {
  QSettings().remove(kKey);
  PJ::invalidateCachedSettings();
  EXPECT_EQ(PJ::currentTheme(), u"light"_s);
  EXPECT_EQ(PJ::cachedSetting(kKey, u"other"_s).toString(), u"other"_s);
}

}  // namespace
