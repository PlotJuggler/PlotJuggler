// SPDX-License-Identifier: MPL-2.0
// Shared main() for widget test runners (pj_add_test_runner MAIN gui): offscreen
// unless the caller chose a platform, sandboxed paths, and settings and data folders
// of this process alone. Each ctest case runs in its own process, so this runs once
// per case.
#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QWidget>
#include <QWindow>
#include <clocale>

namespace {

// A QRhi display pass (PJ_GRAPHICS_API, set by cmake/PjTests.cmake's backend
// passes) needs a window system that exposes windows; a non-interactive session
// (a service-hosted CI runner) never does. Then every case of the pass is skipped,
// or failed under PJ_REQUIRE_GPU_TESTS=1, as the GPU gate does for a missing GPU.
class DisplayPassEnvironment : public ::testing::Environment {
 public:
  void SetUp() override {
    QWidget probe;
    probe.resize(64, 64);
    probe.show();
    QElapsedTimer timer;
    timer.start();
    while (!probe.windowHandle()->isExposed() && timer.elapsed() < 5000) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    if (probe.windowHandle()->isExposed()) {
      return;
    }
    const std::string reason =
        "QPA '" + QGuiApplication::platformName().toStdString() + "' exposes no window in this session";
    if (qEnvironmentVariable("PJ_REQUIRE_GPU_TESTS") == QLatin1String("1")) {
      GTEST_FAIL() << reason;
    }
    GTEST_SKIP() << reason;
  }
};

// Removes what this process's application name placed: its settings file and its data folders.
void removeProcessFolders() {
  QFile::remove(QSettings().fileName());
  for (const QStandardPaths::StandardLocation location :
       {QStandardPaths::AppDataLocation, QStandardPaths::AppLocalDataLocation, QStandardPaths::AppConfigLocation,
        QStandardPaths::CacheLocation}) {
    const QString folder = QStandardPaths::writableLocation(location);
    if (folder.contains(QCoreApplication::applicationName())) {  // never a folder another process shares
      QDir(folder).removeRecursively();
    }
  }
}

}  // namespace

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
#ifdef Q_OS_MACOS
  // As pj_app's main(): an OpenGL QRhi on macOS needs a core-profile context.
  QSurfaceFormat core_format = QSurfaceFormat::defaultFormat();
  core_format.setVersion(4, 1);
  core_format.setProfile(QSurfaceFormat::CoreProfile);
  QSurfaceFormat::setDefaultFormat(core_format);
#endif
  QApplication app(argc, argv);
  std::setlocale(LC_NUMERIC, "C");  // as pj_app's main(): Luau cannot read "0.5" under a decimal-comma locale
  QCoreApplication::setOrganizationName(QStringLiteral("PlotJugglerTest"));
  // An application name of its own for every process: the name places the settings file and the
  // data folder, and several processes run at once (parallel ctest, other checkouts). The data
  // folder holds the default extension store, which a window opens and locks whatever plugin
  // folder it was given: sharing it, the process that lost the lock reported "another instance is
  // managing extensions", and a test that counts diagnostics failed.
  QCoreApplication::setApplicationName(QStringLiteral("%1-%2")
                                           .arg(QFileInfo(QString::fromLocal8Bit(argv[0])).baseName())
                                           .arg(QCoreApplication::applicationPid()));
  QSettings().clear();  // a crashed process may have left a file under this pid
  if (qEnvironmentVariableIsSet("PJ_GRAPHICS_API")) {
    ::testing::AddGlobalTestEnvironment(new DisplayPassEnvironment);
  }
  const int result = RUN_ALL_TESTS();
  removeProcessFolders();
  return result;
}
