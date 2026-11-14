// SPDX-License-Identifier: MPL-2.0
#include <QApplication>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QLocale>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

#include "AppLanguage.h"

using namespace Qt::StringLiterals;

// Check Qt's actual widget lookup contexts, not only direct translator calls.
class QtFileDialogLanguageTest : public QObject {
  Q_OBJECT

 private slots:
  void repeatedEnglishAndChineseFileDialogs() {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    QSKIP("QFileDialog's native QtProject settings cannot be redirected with QSettings::setPath");
#else
    for (const QString& requested : {u"en_US"_s, u"zh_CN"_s, u"en_US"_s, u"zh_CN"_s}) {
      const PJ::AppLanguage language(*qApp, requested);
      const bool chinese = requested == "zh_CN"_L1;
      QFileDialog dialog;
      dialog.setOption(QFileDialog::DontUseNativeDialog);
      QCOMPARE(dialog.labelText(QFileDialog::LookIn), chinese ? u"查找范围(&L)："_s : u"&Look in:"_s);
      QCOMPARE(dialog.labelText(QFileDialog::FileType), chinese ? u"文件类型(&T)："_s : u"Files of &type:"_s);
    }
#endif
  }

  void repeatedEnglishAndChineseWidgets() {
    const QLocale original;
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    for (const QString& requested : {u"en_US"_s, u"zh_CN"_s, u"en_US"_s, u"zh_CN"_s}) {
      const PJ::AppLanguage language(*qApp, requested);
      const bool chinese = requested == "zh_CN"_L1;
      QDialogButtonBox buttons(QDialogButtonBox::Cancel);
      QCOMPARE(buttons.button(QDialogButtonBox::Cancel)->text(), chinese ? u"取消"_s : u"Cancel"_s);
      QFileIconProvider icons;
      const QString folder_type = icons.type(QFileInfo(directory.path()));
      if (chinese) {
        QCOMPARE(folder_type, u"文件夹"_s);
      } else {
        QVERIFY(folder_type == "Folder"_L1 || folder_type == "File Folder"_L1);
      }
      QCOMPARE(QLocale(), original);
      QCOMPARE(QLocale().toString(1234.5), original.toString(1234.5));
    }
  }
};

int main(int argc, char** argv) {
  QTemporaryDir settings_directory;
  if (!settings_directory.isValid()) {
    return 1;
  }
  // QFileDialog saves under QtProject, not the test application's name.
  // Its explicit NativeFormat is path-redirectable on Linux, not Windows/macOS.
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_directory.path());
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
  QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settings_directory.path());
#endif
  QApplication app(argc, argv);
  QtFileDialogLanguageTest test;
  return QTest::qExec(&test, argc, argv);
}
#include "qt_file_dialog_language_test.moc"
