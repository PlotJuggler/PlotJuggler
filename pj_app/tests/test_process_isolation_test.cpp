// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The shared GUI test main gives every test process settings and data folders of its own
// (cmake/test_mains/pj_test_main_gui.cpp). The data folder holds the default extension store, which
// a window locks: test processes that shared it failed each other at random.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QString>

TEST(TestProcessIsolationTest, DataAndSettingsBelongToThisProcessAlone) {
  const QString pid = QString::number(QCoreApplication::applicationPid());
  for (const QStandardPaths::StandardLocation location :
       {QStandardPaths::AppDataLocation, QStandardPaths::AppLocalDataLocation, QStandardPaths::AppConfigLocation}) {
    EXPECT_TRUE(QStandardPaths::writableLocation(location).contains(pid))
        << QStandardPaths::writableLocation(location).toStdString();
  }
  EXPECT_TRUE(QSettings().fileName().contains(pid)) << QSettings().fileName().toStdString();
}
