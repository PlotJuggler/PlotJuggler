// SPDX-License-Identifier: MPL-2.0
#include "AppLanguage.h"

#include <QCoreApplication>
#include <QDebug>
#include <QSettings>
#include <QVariant>

using namespace Qt::StringLiterals;

namespace PJ {

QString AppLanguage::normalizedLanguage(const QString& language) {
  return language == "zh_CN"_L1 || language == "zh-CN"_L1 ? u"zh_CN"_s : u"en_US"_s;
}

QString AppLanguage::preferredLanguage(const QSettings& settings) {
  return normalizedLanguage(settings.value(kPreferenceKey, u"en_US"_s).toString());
}

AppLanguage::AppLanguage(QCoreApplication& app, const QString& language) : language_(normalizedLanguage(language)) {
  if (language_ == "zh_CN"_L1) {
    catalog_loaded_ = translator_.load(u":/i18n/plotjuggler4_zh_CN.qm"_s) && app.installTranslator(&translator_);
    if (!catalog_loaded_) {
      qWarning("Simplified Chinese translation catalog is missing; using English source text");
    }
  }
}

}  // namespace PJ

// The pinned Qt's qtbase_zh_CN lacks the current QFileDialog labels and
// file-icon keys. Extract them into the application catalog so the merged
// catalog covers them; drop an entry once Qt's own catalog carries it.
[[maybe_unused]] static const char* const kQtFileDialogKeys[] = {
    QT_TRANSLATE_NOOP("QFileDialog", "&Look in:"),
    QT_TRANSLATE_NOOP("QFileDialog", "Files of &type:"),
    QT_TRANSLATE_NOOP("QAbstractFileIconProvider", "Drive"),
    QT_TRANSLATE_NOOP("QAbstractFileIconProvider", "File"),
    QT_TRANSLATE_NOOP("QAbstractFileIconProvider", "Unknown"),
};
[[maybe_unused]] static const struct {
  const char* source;
  const char* comment;
} kQtFileIconProviderKeys[] = {
    QT_TRANSLATE_NOOP3("QAbstractFileIconProvider", "File Folder", "Match Windows Explorer"),
    QT_TRANSLATE_NOOP3("QAbstractFileIconProvider", "Folder", "All other platforms"),
    QT_TRANSLATE_NOOP3("QAbstractFileIconProvider", "Alias", "macOS Finder"),
    QT_TRANSLATE_NOOP3("QAbstractFileIconProvider", "Shortcut", "All other platforms"),
};
