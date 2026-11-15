// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "../src/browser_save_target.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <pj_plugins/host_qt/widget_binding.hpp>

#include "gtest/gtest.h"

namespace {

void writeFile(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  ASSERT_TRUE(file.open(QIODevice::WriteOnly));
  file.write(bytes);
}

QByteArray readFile(const QString& path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

TEST(BrowserSaveTarget, ASavePickerGetsItsSuggestedNameInAScratchDirectory) {
  const PJ::BrowserSaveTarget target(QStringLiteral("/home/user/export.csv"));
  ASSERT_TRUE(target.isValid());
  EXPECT_EQ(QFileInfo(target.path()).fileName(), QStringLiteral("export.csv"));

  // The plugin may append its own suffix; whatever it writes there is offered.
  writeFile(target.path() + QStringLiteral(".parquet"), "PAR1");
  const QStringList files = target.writtenPaths();
  ASSERT_EQ(files.size(), 1);
  EXPECT_EQ(QFileInfo(files[0]).fileName(), QStringLiteral("export.csv.parquet"));
  EXPECT_EQ(readFile(files[0]), QByteArray("PAR1"));
}

TEST(BrowserSaveTarget, AFolderPickerOffersEveryFileWrittenUnderIt) {
  QString scratch;
  {
    const PJ::BrowserSaveTarget target;
    ASSERT_TRUE(target.isValid());
    scratch = target.path();
    writeFile(scratch + QStringLiteral("/b.csv"), "2");
    ASSERT_TRUE(QDir(scratch).mkdir(QStringLiteral("sub")));
    writeFile(scratch + QStringLiteral("/sub/a.csv"), "1");
    writeFile(scratch + QStringLiteral("/a.csv"), "0");
    const QStringList files = target.writtenPaths();
    ASSERT_EQ(files.size(), 3);
    // Same-named files from different subfolders keep distinct download names.
    EXPECT_EQ(target.downloadName(files[0]), QStringLiteral("a.csv"));
    EXPECT_EQ(target.downloadName(files[1]), QStringLiteral("b.csv"));
    EXPECT_EQ(target.downloadName(files[2]), QStringLiteral("sub_a.csv"));
  }
  EXPECT_FALSE(QFileInfo::exists(scratch));
}

// The desktop save dialog appends the plugin's default suffix to a bare name;
// the browser download name must get it too ("Save rule as..." -> rule.json).
TEST(BrowserSaveTarget, TheNameGetsThePluginsDefaultSuffixWhenItHasNone) {
  EXPECT_EQ(
      PJ::browserSaveName(PJ::FilePickerStart{.dir = {}, .name = QStringLiteral("rule")}, "json"),
      QStringLiteral("rule.json"));
  EXPECT_EQ(
      PJ::browserSaveName(PJ::FilePickerStart{.dir = {}, .name = QStringLiteral("rule.txt")}, "json"),
      QStringLiteral("rule.txt"));
  EXPECT_EQ(PJ::browserSaveName(PJ::FilePickerStart{}, "json"), QStringLiteral("export.json"));
  EXPECT_EQ(PJ::browserSaveName(PJ::FilePickerStart{}, ""), QStringLiteral("export"));
}

}  // namespace
