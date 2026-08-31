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
#include <QString>
#include <QTemporaryDir>
using namespace Qt::StringLiterals;

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

// A helper path that does not exist on disk is caught by the parent-side
// supervisor, not by the child: the runner sets parent_error and never
// crashes.
TEST(PluginCheckRunnerTest, MissingHelperBinaryFailsGracefully) {
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
