// Copyright 2026 Pablo Iñigo Blasco
// SPDX-License-Identifier: MPL-2.0

// End-to-end tests for PluginCheckRunner: exercise the real helper binary
// (pj-plugin-check) in a child process and assert the parent-side runner
// surfaces the expected typed result. The helper path is injected via the
// PJ_PLUGIN_CHECK_HELPER_PATH definition set by CMake.

#include "pj_marketplace/plugin_check_runner.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QString>
#include <QTemporaryDir>
using namespace Qt::StringLiterals;

// ThreadSanitizer-active detection: GCC defines __SANITIZE_THREAD__; Clang
// reports it through __has_feature(thread_sanitizer).
#if defined(__SANITIZE_THREAD__)
#define PJ_TSAN_BUILD 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define PJ_TSAN_BUILD 1
#endif
#endif

namespace {

#ifndef PJ_PLUGIN_CHECK_HELPER_PATH
#error "PJ_PLUGIN_CHECK_HELPER_PATH must be defined at compile time"
#endif

constexpr const char* kHelperPath = PJ_PLUGIN_CHECK_HELPER_PATH;

PJ::PluginCheckOptions helperOptions() {
  return {std::chrono::milliseconds(15'000), QString::fromLatin1(kHelperPath)};
}

}  // namespace

// Sanity: locateHelper honours an explicit override.
TEST(PluginCheckRunnerTest, LocateHelperReturnsExplicitOverride) {
  const QString override_path = u"/opt/custom/pj-plugin-check"_s;
  EXPECT_EQ(PJ::PluginCheckRunner::locateHelper(override_path), override_path);
}

// Runner returns `ok=false` when the directory does not exist. The helper
// itself is reachable, so this exercises the helper's own no-valid-plugin
// exit code path (kExitNoValidPlugin = 4).
TEST(PluginCheckRunnerTest, RejectsMissingDirectory) {
  const PJ::PluginCheckRunner runner(helperOptions());
  const PJ::PluginCheckResult result = runner.run(u"/tmp/pj-plugin-check-test-missing-path"_s);
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.exit_status, QProcess::NormalExit);
  EXPECT_TRUE(result.plugins.isEmpty());
}

// Runner returns `ok=false` for an empty directory (no DSOs to admit) but the
// child still exits cleanly — no crash reaches the parent.
TEST(PluginCheckRunnerTest, RejectsEmptyDirectoryWithoutCrash) {
  QTemporaryDir empty_dir;
  ASSERT_TRUE(empty_dir.isValid());

  const PJ::PluginCheckRunner runner(helperOptions());
  const PJ::PluginCheckResult result = runner.run(empty_dir.path());

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.exit_status, QProcess::NormalExit);
  EXPECT_TRUE(result.plugins.isEmpty());
  EXPECT_TRUE(result.parent_error.isEmpty() || !result.parent_error.contains(u"crashed"_s));
}

// A directory that only contains non-DSO files is rejected the same way,
// confirming the helper filters candidates without opening arbitrary bytes.
TEST(PluginCheckRunnerTest, RejectsDirectoryWithoutPluginDsos) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  QFile noise(QDir(dir.path()).absoluteFilePath(u"README.txt"_s));
  ASSERT_TRUE(noise.open(QIODevice::WriteOnly));
  noise.write("not a plugin");
  noise.close();

  const PJ::PluginCheckRunner runner(helperOptions());
  const PJ::PluginCheckResult result = runner.run(dir.path());

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.exit_status, QProcess::NormalExit);
  EXPECT_TRUE(result.plugins.isEmpty());
}

// The child gets the Qt library directory first on the loader search path, ahead of what the parent had.
TEST(PluginCheckRunnerTest, ChildEnvironmentPutsTheQtLibrariesFirst) {
#if defined(__linux__) || defined(_WIN32)
#if defined(_WIN32)
  const QString variable = u"PATH"_s;
#else
  const QString variable = u"LD_LIBRARY_PATH"_s;
#endif
  QProcessEnvironment base;
  base.insert(u"UNRELATED"_s, u"kept"_s);
  const QString sep = QDir::listSeparator();

  const QProcessEnvironment empty = PJ::PluginCheckRunner::childEnvironment(base, u"/qt/lib"_s);
  EXPECT_EQ(empty.value(variable), u"/qt/lib"_s);
  EXPECT_EQ(empty.value(u"UNRELATED"_s), u"kept"_s);

  base.insert(variable, u"/app/lib"_s);
  const QProcessEnvironment prepended = PJ::PluginCheckRunner::childEnvironment(base, u"/qt/lib"_s);
  EXPECT_EQ(prepended.value(variable), u"/qt/lib"_s + sep + u"/app/lib"_s);

  EXPECT_EQ(PJ::PluginCheckRunner::childEnvironment(base, QString()).value(variable), u"/app/lib"_s);
#else
  GTEST_SKIP() << "no loader search-path variable is managed on this platform";
#endif
}

#ifdef PJ_TEST_WIDGET_PLUGIN_PATH
// A Widget library is admitted with family "widget": the helper must name the family, not "unknown".
TEST(PluginCheckRunnerTest, ReportsWidgetFamily) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString target =
      QDir(dir.path()).absoluteFilePath(QFileInfo(QString::fromLatin1(PJ_TEST_WIDGET_PLUGIN_PATH)).fileName());
  ASSERT_TRUE(QFile::copy(QString::fromLatin1(PJ_TEST_WIDGET_PLUGIN_PATH), target));

  const PJ::PluginCheckRunner runner(helperOptions());
  const PJ::PluginCheckResult result = runner.run(dir.path());

  ASSERT_TRUE(result.ok) << result.parent_error.toStdString();
  ASSERT_EQ(result.plugins.size(), 1);
  EXPECT_EQ(result.plugins[0].family, u"widget"_s);
  EXPECT_EQ(result.plugins[0].id, u"test-widgets"_s);
}
#endif

// A helper path that does not exist on disk is caught by the parent-side
// supervisor, not by the child: the runner sets parent_error and never
// crashes.
TEST(PluginCheckRunnerTest, MissingHelperBinaryFailsGracefully) {
#ifdef PJ_TSAN_BUILD
  // QProcess learns that a child whose exec failed has exited through its SIGCHLD
  // handler. TSan runs a deferred signal handler only when the thread next passes
  // through one of its interceptors, and cmake/sanitizers/tsan.supp makes calls
  // from libQt6Core bypass them, so Qt blocks forever reading a pipe that handler
  // would have written. Measured: hangs with that entry, passes in 5 ms without it.
  GTEST_SKIP() << "QProcess exec-failure path deadlocks under TSan with libQt6Core interceptors ignored";
#endif
  const PJ::PluginCheckOptions bad_options{std::chrono::milliseconds(5'000), u"/tmp/pj-plugin-check-does-not-exist"_s};
  const PJ::PluginCheckRunner runner(bad_options);
  const PJ::PluginCheckResult result = runner.run(u"/tmp"_s);
  EXPECT_FALSE(result.ok);
  EXPECT_FALSE(result.parent_error.isEmpty());
  EXPECT_TRUE(result.plugins.isEmpty());
}

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
