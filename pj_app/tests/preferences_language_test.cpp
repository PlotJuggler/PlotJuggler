// SPDX-License-Identifier: MPL-2.0
#include <gtest/gtest.h>

#include <QApplication>
#include <QFontInfo>
#include <QLabel>
#include <QScrollArea>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

#include "AppLanguage.h"
#include "PreferencesDialog.h"
#include "Theme.h"
#include "pj_widgets/ComboBox.h"
#include "support/gui_test_env.h"

using namespace Qt::StringLiterals;

namespace {

TEST(PreferencesLanguageTest, FreshProfileOffersChineseAndEnglishWithARestartNotice) {
  PJ::Theme theme;
  PJ::PreferencesDialog dialog(theme);
  const auto* combo = dialog.findChild<PJ::ComboBox*>(u"languageCombo"_s);
  ASSERT_NE(combo, nullptr);
  ASSERT_EQ(combo->count(), 2);
  EXPECT_EQ(combo->currentData().toString(), u"en_US"_s);
  EXPECT_EQ(combo->itemData(0).toString(), u"zh_CN"_s);
  EXPECT_EQ(combo->itemData(1).toString(), u"en_US"_s);
  const auto* notice = dialog.findChild<QLabel*>(u"labelLanguageRestart"_s);
  ASSERT_NE(notice, nullptr);
  EXPECT_FALSE(notice->text().isEmpty());
}

TEST(PreferencesLanguageTest, AcceptSavesNextLaunchLanguageWithoutRetranslatingTheSession) {
  const PJ::AppLanguage running(*qApp, u"zh_CN"_s);
  ASSERT_TRUE(running.catalogLoaded());
  PJ::Theme theme;
  {
    PJ::PreferencesDialog dialog(theme);
    auto* combo = dialog.findChild<PJ::ComboBox*>(u"languageCombo"_s);
    ASSERT_NE(combo, nullptr);
    combo->setCurrentIndex(combo->findData(u"en_US"_s));
    EXPECT_FALSE(QSettings().contains(PJ::AppLanguage::kPreferenceKey));
    dialog.accept();
  }
  EXPECT_EQ(PJ::AppLanguage::preferredLanguage(QSettings()), u"en_US"_s);
  EXPECT_NE(QCoreApplication::translate("PreferencesContent", "Language:"), u"Language:"_s);
  PJ::PreferencesDialog reopened(theme);
  const auto* combo = reopened.findChild<PJ::ComboBox*>(u"languageCombo"_s);
  ASSERT_NE(combo, nullptr);
  EXPECT_EQ(combo->currentData().toString(), u"en_US"_s);
}

TEST(PreferencesLanguageTest, EnglishToChineseTakesEffectOnlyOnNextLaunch) {
  QSettings().setValue(PJ::AppLanguage::kPreferenceKey, u"en_US"_s);
  PJ::Theme theme;
  {
    const PJ::AppLanguage running(*qApp, PJ::AppLanguage::preferredLanguage(QSettings()));
    PJ::PreferencesDialog dialog(theme);
    auto* combo = dialog.findChild<PJ::ComboBox*>(u"languageCombo"_s);
    ASSERT_NE(combo, nullptr);
    ASSERT_EQ(combo->currentData().toString(), u"en_US"_s);
    const int chinese_index = combo->findData(u"zh_CN"_s);
    ASSERT_GE(chinese_index, 0);
    combo->setCurrentIndex(chinese_index);
    EXPECT_EQ(QSettings().value(PJ::AppLanguage::kPreferenceKey).toString(), u"en_US"_s);
    dialog.accept();
    EXPECT_EQ(QSettings().value(PJ::AppLanguage::kPreferenceKey).toString(), u"zh_CN"_s);
    EXPECT_EQ(QCoreApplication::translate("PreferencesContent", "Language:"), u"Language:"_s);
  }
  const PJ::AppLanguage restarted(*qApp, PJ::AppLanguage::preferredLanguage(QSettings()));
  ASSERT_TRUE(restarted.catalogLoaded());
  PJ::PreferencesDialog reopened(theme);
  const auto* combo = reopened.findChild<PJ::ComboBox*>(u"languageCombo"_s);
  ASSERT_NE(combo, nullptr);
  EXPECT_EQ(combo->currentData().toString(), u"zh_CN"_s);
  EXPECT_NE(QCoreApplication::translate("PreferencesContent", "Language:"), u"Language:"_s);
}

TEST(PreferencesLanguageTest, OkKeepsASavedLanguageThisBuildDoesNotSupport) {
  QSettings().setValue(PJ::AppLanguage::kPreferenceKey, u"ja_JP"_s);
  PJ::Theme theme;
  {
    PJ::PreferencesDialog dialog(theme);
    dialog.accept();
  }
  EXPECT_EQ(QSettings().value(PJ::AppLanguage::kPreferenceKey).toString(), u"ja_JP"_s);
}

TEST(PreferencesLanguageTest, CancelLeavesTheSavedLanguageAlone) {
  QSettings().setValue(PJ::AppLanguage::kPreferenceKey, u"en_US"_s);
  PJ::Theme theme;
  {
    PJ::PreferencesDialog dialog(theme);
    auto* combo = dialog.findChild<PJ::ComboBox*>(u"languageCombo"_s);
    ASSERT_NE(combo, nullptr);
    EXPECT_EQ(combo->currentData().toString(), u"en_US"_s);
    combo->setCurrentIndex(combo->findData(u"zh_CN"_s));
    dialog.reject();
  }
  EXPECT_EQ(QSettings().value(PJ::AppLanguage::kPreferenceKey).toString(), u"en_US"_s);
}

TEST(PreferencesLanguageTest, SessionOverrideDoesNotChangeTheNextLaunchSelection) {
  QSettings().setValue(PJ::AppLanguage::kPreferenceKey, u"zh_CN"_s);
  const PJ::AppLanguage running(*qApp, u"en_US"_s);
  PJ::Theme theme;
  PJ::PreferencesDialog dialog(theme);
  const auto* combo = dialog.findChild<PJ::ComboBox*>(u"languageCombo"_s);
  ASSERT_NE(combo, nullptr);
  EXPECT_EQ(combo->currentData().toString(), u"zh_CN"_s);
}

TEST(PreferencesLanguageTest, LanguageSelectorHonorsBothSizeHintsAcrossFonts) {
  struct RestoreApplicationFont {
    QFont original = qApp->font();
    ~RestoreApplicationFont() {
      QApplication::setFont(original);
    }
  } restore_font;
  for (const QFont& font :
       {restore_font.original, QFont(u"DejaVu Sans"_s, 9), QFont(u"DejaVu Sans"_s, 12),
        QFont(u"Liberation Sans"_s, 9)}) {
    SCOPED_TRACE(font.toString().toStdString());
    QApplication::setFont(font);
    SCOPED_TRACE(QFontInfo(qApp->font()).family().toStdString());
    for (const QString& requested : {u"en_US"_s, u"zh_CN"_s}) {
      SCOPED_TRACE(requested.toStdString());
      const PJ::AppLanguage running(*qApp, requested);
      PJ::Theme theme;
      PJ::PreferencesDialog dialog(theme);
      auto* combo = dialog.findChild<PJ::ComboBox*>(u"languageCombo"_s);
      ASSERT_NE(combo, nullptr);
      dialog.resize(420, 300);
      dialog.show();
      QTest::qWait(20);

      // Compare against a freshly populated control as well as Qt's cached
      // minimum: a cached empty-model hint can be wider OR narrower than text.
      PJ::ComboBox populated(combo->parentWidget());
      populated.setObjectName(combo->objectName());
      populated.setFont(combo->font());
      populated.setSizeAdjustPolicy(combo->sizeAdjustPolicy());
      for (int index = 0; index < combo->count(); ++index) {
        populated.addItem(combo->itemText(index), combo->itemData(index));
      }
      populated.ensurePolished();
      EXPECT_GE(combo->width(), combo->minimumSizeHint().width());
      EXPECT_GE(combo->width(), populated.sizeHint().expandedTo(populated.minimumSizeHint()).width());
      dialog.reject();
    }
  }
}

TEST(PreferencesLanguageTest, MinimumSizeKeepsTranslatedAppearanceControlsReachable) {
  for (const QString& requested : {u"en_US"_s, u"zh_CN"_s}) {
    const PJ::AppLanguage running(*qApp, requested);
    PJ::Theme theme;
    PJ::PreferencesDialog dialog(theme);
    dialog.resize(420, 300);
    dialog.show();
    QTest::qWait(20);
    auto* scroll = dialog.findChild<QScrollArea*>(u"appearanceScrollArea"_s);
    auto* combo = dialog.findChild<PJ::ComboBox*>(u"languageCombo"_s);
    auto* notice = dialog.findChild<QLabel*>(u"labelLanguageRestart"_s);
    auto* last_control = dialog.findChild<QWidget*>(u"mcpServerToggle"_s);
    ASSERT_NE(last_control, nullptr);
    // Browser builds hide MCP; the system picker is also platform-dependent.
    if (last_control->isHidden()) {
      last_control = dialog.findChild<QWidget*>(u"systemFileDialogToggle"_s);
    }
    ASSERT_NE(last_control, nullptr);
    if (last_control->isHidden()) {
      last_control = dialog.findChild<QWidget*>(u"telemetryToggle"_s);
    }
    ASSERT_NE(scroll, nullptr);
    ASSERT_NE(combo, nullptr);
    ASSERT_NE(notice, nullptr);
    ASSERT_NE(last_control, nullptr);
    EXPECT_GE(combo->width(), combo->minimumSizeHint().width());
    EXPECT_GE(notice->height(), notice->heightForWidth(notice->width()));
    for (QWidget* control : {static_cast<QWidget*>(combo), static_cast<QWidget*>(notice), last_control}) {
      scroll->ensureWidgetVisible(control, 0, 0);
      QCoreApplication::processEvents();
      const QRect visible_rect(control->mapTo(scroll->viewport(), QPoint(0, 0)), control->size());
      EXPECT_TRUE(scroll->viewport()->rect().contains(visible_rect));
    }
    dialog.reject();
  }
}

// Each discovered case gets a private settings file, like the other shell
// preference tests, without touching the user's actual application profile.
class LanguageSettingsEnvironment : public ::testing::Environment {
 public:
  void SetUp() override {
    if (!pj_app_test::isTestSuiteSelected("PreferencesLanguageTest")) {
      return;
    }
    QCoreApplication::setOrganizationName(u"PlotJugglerTest"_s);
    QCoreApplication::setApplicationName(u"preferences_language_test"_s);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir_.path());
  }

 private:
  QTemporaryDir settings_dir_;
};

static auto* const kLanguageEnv = ::testing::AddGlobalTestEnvironment(new LanguageSettingsEnvironment);

}  // namespace
