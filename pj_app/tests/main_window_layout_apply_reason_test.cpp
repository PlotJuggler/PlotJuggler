// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A layout that parses but cannot be applied must say WHY in the same
// message the user sees, not only in a separate bell diagnostic: the
// "Layout" diagnostics emitted during the restore are captured and appended
// to the layout-apply-failed report.

#include <gtest/gtest.h>

#include <QApplication>
#include <QFile>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextStream>

#include "pj_marketplace/qt_diagnostic_bridge.hpp"
#include "support/layout_import_gui_support.h"
#include "support/loader_test_support.h"

namespace {

using PJ::MainWindowLayoutImportTestPeer;
using pj_app_test::pumpUntil;

TEST(MainWindowLayoutApplyReasonTest, ApplyFailureReportCarriesTheCapturedReason) {
  QTemporaryDir extensions_dir;
  QTemporaryDir project_dir;
  ASSERT_TRUE(extensions_dir.isValid());
  ASSERT_TRUE(project_dir.isValid());

  PJ::MainWindow window(extensions_dir.path());
  ASSERT_TRUE(window.populateTestData());  // past the catalog-empty guard

  QString failure_message;
  QObject::connect(
      MainWindowLayoutImportTestPeer::diagnosticBridge(window), &PJ::QtDiagnosticBridge::diagnosticReported, &window,
      [&failure_message](int /*level*/, const QString& /*source*/, const QString& id, const QString& message) {
        if (id == QStringLiteral("layout-apply-failed")) {
          failure_message = message;
        }
      });

  // Well-formed XML, valid <root>, but no <tabbed_widget>: parse succeeds,
  // apply fails inside xmlLoadState.
  const QString layout_path = project_dir.filePath(QStringLiteral("broken.pj4.xml"));
  {
    QFile file(layout_path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream out(&file);
    out << "<?xml version='1.0' encoding='UTF-8'?>\n<root binding=\"generic\" pj4_version=\"4\"/>\n";
  }

  MainWindowLayoutImportTestPeer::loadLayout(window, layout_path, /*interactive=*/false);
  ASSERT_TRUE(pumpUntil([&failure_message]() { return !failure_message.isEmpty(); }));
  EXPECT_TRUE(failure_message.contains(QStringLiteral("tabbed_widget"))) << failure_message.toStdString();
}

}  // namespace

PJ_MAIN_WINDOW_TEST_MAIN
