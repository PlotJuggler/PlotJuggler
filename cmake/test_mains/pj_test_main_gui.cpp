// SPDX-License-Identifier: MPL-2.0
// Shared main() for widget test runners (pj_add_test_runner MAIN gui): offscreen
// unless the caller chose a platform, sandboxed paths, and empty per-runner settings.
// Each ctest case runs in its own process, so this runs once per case.
#include <gtest/gtest.h>

#include <QApplication>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

int main(int argc, char** argv) {
  if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  QStandardPaths::setTestModeEnabled(true);
  ::testing::InitGoogleTest(&argc, argv);
  // ctest discovery (--gtest_list_tests) only needs the case names: answer it
  // before Qt starts, because a cold fontconfig cache can push QApplication
  // construction past the 5 s discovery timeout on a fresh CI runner.
  if (::testing::GTEST_FLAG(list_tests)) {
    return RUN_ALL_TESTS();
  }
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("PlotJugglerTest"));
  QCoreApplication::setApplicationName(QFileInfo(QString::fromLocal8Bit(argv[0])).baseName());
  QSettings().clear();
  return RUN_ALL_TESTS();
}
