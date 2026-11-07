// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QString>
#include <QTranslator>

class QCoreApplication;
class QSettings;

namespace PJ {

// Owns the startup translator for the entire UI lifetime. Construct after the
// settings backend is ready and before any window; destroy before the app.
// Changing the saved preference never retranslates an existing widget tree.
class AppLanguage final {
 public:
  static inline const QString kPreferenceKey = QStringLiteral("Preferences::language");

  // Accepts zh_CN/zh-CN; all other values safely use the English source text.
  [[nodiscard]] static QString normalizedLanguage(const QString& language);

  // A fresh profile keeps the English UI, independent of the OS locale.
  [[nodiscard]] static QString preferredLanguage(const QSettings& settings);

  explicit AppLanguage(QCoreApplication& app, const QString& language);
  ~AppLanguage();

  AppLanguage(const AppLanguage&) = delete;
  AppLanguage& operator=(const AppLanguage&) = delete;

  [[nodiscard]] const QString& language() const {
    return language_;
  }
  [[nodiscard]] bool catalogLoaded() const {
    return catalog_loaded_;
  }

 private:
  QCoreApplication& app_;
  QString language_;
  QTranslator translator_;
  bool catalog_loaded_ = false;
};

}  // namespace PJ
