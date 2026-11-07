// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include "FileSelectionService.h"

namespace PJ {
namespace {

TEST(FileSelectionServiceTest, BrowserUsesAggregateFilterOnly) {
  const QString desktop =
      QStringLiteral("All supported files (*.csv *.mcap);;CSV (*.csv);;MCAP (*.mcap);;All files (*)");
  EXPECT_EQ(FileSelectionService::browserContentFilter(desktop), QStringLiteral("All supported files (*.csv *.mcap)"));
}

TEST(FileSelectionServiceTest, BrowserPreservesSingleOrEmptyFilter) {
  EXPECT_EQ(
      FileSelectionService::browserContentFilter(QStringLiteral("All files (*)")), QStringLiteral("All files (*)"));
  EXPECT_TRUE(FileSelectionService::browserContentFilter(QString{}).isEmpty());
}

TEST(FileSelectionServiceTest, BrowserAcceptListsFirstEntryExtensions) {
  EXPECT_EQ(
      FileSelectionService::browserAcceptList(
          QStringLiteral("All supported files (*.csv *.mcap *.csv);;CSV (*.csv);;Archives (*.tar.gz)")),
      QStringLiteral(".csv,.mcap"));
  EXPECT_EQ(FileSelectionService::browserAcceptList(QStringLiteral("Archives (*.tar.gz)")), QStringLiteral(".tar.gz"));
  EXPECT_EQ(FileSelectionService::browserAcceptList(QStringLiteral("*.ulg *.bag")), QStringLiteral(".ulg,.bag"));
}

TEST(FileSelectionServiceTest, BrowserAcceptIsEmptyForWildcardOrNoExtension) {
  EXPECT_TRUE(FileSelectionService::browserAcceptList(QStringLiteral("All files (*)")).isEmpty());
  EXPECT_TRUE(FileSelectionService::browserAcceptList(QStringLiteral("Data (*.csv *)")).isEmpty());
  EXPECT_TRUE(FileSelectionService::browserAcceptList(QStringLiteral("Anything")).isEmpty());
  EXPECT_TRUE(FileSelectionService::browserAcceptList(QString{}).isEmpty());
}

}  // namespace
}  // namespace PJ
