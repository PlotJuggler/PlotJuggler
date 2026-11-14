// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "../src/browser_save_target.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "gtest/gtest.h"

namespace {

void writeFile(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  ASSERT_TRUE(file.open(QIODevice::WriteOnly));
  file.write(bytes);
}

TEST(BrowserSaveTarget, ASavePickerGetsItsSuggestedNameInAScratchDirectory) {
  const PJ::BrowserSaveTarget target(QStringLiteral("/home/user/export.csv"));
  ASSERT_TRUE(target.isValid());
  EXPECT_EQ(QFileInfo(target.path()).fileName(), QStringLiteral("export.csv"));

  // The plugin may append its own suffix; whatever it writes there is offered.
  writeFile(target.path() + QStringLiteral(".parquet"), "PAR1");
  const auto files = target.writtenFiles();
  ASSERT_EQ(files.size(), 1U);
  EXPECT_EQ(files[0].first, QStringLiteral("export.csv.parquet"));
  EXPECT_EQ(files[0].second, QByteArray("PAR1"));
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
    const auto files = target.writtenFiles();
    ASSERT_EQ(files.size(), 2U);
    EXPECT_EQ(files[0].first, QStringLiteral("a.csv"));
    EXPECT_EQ(files[1].first, QStringLiteral("b.csv"));
  }
  EXPECT_FALSE(QFileInfo::exists(scratch));
}

}  // namespace
