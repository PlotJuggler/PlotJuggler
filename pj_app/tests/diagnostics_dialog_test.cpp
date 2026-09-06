// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The bell dialog is a pure view over DiagnosticHistory: filters hide rows,
// new records rebuild the table, the cap evicts, Clear empties everything.

#include <gtest/gtest.h>

#include <QApplication>

#include "pj_runtime/DiagnosticHistory.h"
#include "pj_widgets/CheckButton.h"
#include "pj_widgets/ComboBox.h"
#include "pj_widgets/Search.h"
#include "ui/DiagnosticsDialog.h"
using namespace Qt::StringLiterals;

namespace PJ {

struct QtEnvironment : ::testing::Environment {
  void SetUp() override {
    static int argc = 0;
    app_ = new QApplication(argc, nullptr);
  }
  void TearDown() override {
    delete app_;
  }
  QApplication* app_ = nullptr;
};

namespace {

TEST(DiagnosticsDialogTest, FiltersLiveUpdatesAndClear) {
  DiagnosticHistory history;
  history.record(DiagnosticLevel::kInfo, u"loader"_s, u"id-1"_s, u"file opened"_s);
  history.record(DiagnosticLevel::kWarning, u"parser"_s, u"id-2"_s, u"odd field\nsecond line"_s);
  history.record(DiagnosticLevel::kError, u"parser"_s, u"id-3"_s, u"decode failed"_s);

  DiagnosticsDialog dialog(&history);
  EXPECT_EQ(dialog.visibleRowCount(), 0);  // hidden: nothing built yet
  dialog.show();
  EXPECT_EQ(dialog.visibleRowCount(), 3);

  auto* toggle_error = dialog.findChild<CheckButton*>(u"toggleError"_s);
  ASSERT_NE(toggle_error, nullptr);
  toggle_error->setChecked(false);
  EXPECT_EQ(dialog.visibleRowCount(), 2);

  auto* combo = dialog.findChild<ComboBox*>(u"comboSource"_s);
  ASSERT_NE(combo, nullptr);
  ASSERT_EQ(combo->count(), 3);  // "All sources", loader, parser
  combo->setCurrentIndex(combo->findData(u"parser"_s));
  EXPECT_EQ(dialog.visibleRowCount(), 1);  // the warning; error is toggled off

  auto* search = dialog.findChild<Search*>(u"searchBox"_s);
  ASSERT_NE(search, nullptr);
  search->setText(u"ODD"_s);
  EXPECT_EQ(dialog.visibleRowCount(), 1);
  search->setText(u"opened"_s);
  EXPECT_EQ(dialog.visibleRowCount(), 0);  // matches the loader row, filtered out by source
  search->clear();
  combo->setCurrentIndex(0);
  toggle_error->setChecked(true);
  EXPECT_EQ(dialog.visibleRowCount(), 3);

  // Live append rebuilds; the source combo learns the new source.
  history.record(DiagnosticLevel::kInfo, u"stream"_s, u""_s, u"connected"_s);
  EXPECT_EQ(dialog.visibleRowCount(), 4);
  EXPECT_GE(combo->findData(u"stream"_s), 0);

  // Cap eviction mirrors the history.
  history.setMaxRecords(2);
  history.record(DiagnosticLevel::kInfo, u"stream"_s, u""_s, u"one more"_s);
  EXPECT_EQ(dialog.visibleRowCount(), 2);

  history.clear();
  EXPECT_EQ(dialog.visibleRowCount(), 0);
  EXPECT_TRUE(dialog.paneText().isEmpty());

  // Hidden dialogs skip rebuilds and catch up on the next show.
  dialog.hide();
  history.record(DiagnosticLevel::kInfo, u"loader"_s, u""_s, u"while hidden"_s);
  EXPECT_EQ(dialog.visibleRowCount(), 0);
  dialog.show();
  EXPECT_EQ(dialog.visibleRowCount(), 1);
}

}  // namespace
}  // namespace PJ

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  ::testing::AddGlobalTestEnvironment(new PJ::QtEnvironment);
  return RUN_ALL_TESTS();
}
