// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Regression: a multi-line, word-wrapped MessageBox body must not clip. The
// dialog used to open one line tall (the body label's heightForWidth flag was
// cleared and the styled card breaks heightForWidth propagation), so verbose
// question() bodies — e.g. the destructive dataset-merge confirmation — were cut
// off. The fix grows the dialog to the wrapped height on first show.

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QFontMetrics>
#include <QLabel>
#include <QPushButton>
#include <QTimer>

#include "pj_widgets/MessageBox.h"
using namespace Qt::StringLiterals;

namespace {

// The body label is reachable by its objectName (no test-only accessor needed).
QLabel* bodyLabelOf(PJ::MessageBox& box) {
  return box.findChild<QLabel*>(u"pjMessageBoxBody"_s);
}

TEST(MessageBoxTest, MultiLineBodyIsNotClipped) {
  PJ::MessageBox dlg;
  dlg.setTitle(u"Merge datasets"_s);
  dlg.setText(QStringLiteral(
      "Merging datasets is a destructive operation. Do you wish to proceed?\n\n"
      "Datasets run1, run2 overlap in time.\n"
      "Datasets run1, run2 have colliding data.\n"
      "Datasets run1, run2 contain object topics that will be dropped."));
  dlg.addButton(u"Merge"_s, PJ::MessageBox::kDestructiveRole);
  dlg.addButton(u"Cancel"_s, PJ::MessageBox::kCancelRole);

  dlg.show();
  QApplication::processEvents();

  QLabel* body = bodyLabelOf(dlg);
  ASSERT_NE(body, nullptr);
  ASSERT_GT(body->width(), 0);
  // The label is at least as tall as its wrapped text needs at this width —
  // i.e. nothing is clipped.
  EXPECT_GE(body->height(), body->heightForWidth(body->width()))
      << "body height " << body->height() << " < wrapped height " << body->heightForWidth(body->width());
  // And it genuinely wrapped to several lines (sanity that we exercised the path).
  const QFontMetrics fm(body->font());
  EXPECT_GT(body->height(), fm.height() * 3);
  dlg.close();
}

TEST(MessageBoxTest, ShortBodyAlsoFits) {
  PJ::MessageBox dlg;
  dlg.setTitle(u"Heads up"_s);
  dlg.setText(u"All good."_s);
  dlg.addButton(u"OK"_s, PJ::MessageBox::kPrimaryRole);

  dlg.show();
  QApplication::processEvents();

  QLabel* body = bodyLabelOf(dlg);
  ASSERT_NE(body, nullptr);
  ASSERT_GT(body->width(), 0);
  EXPECT_GE(body->height(), body->heightForWidth(body->width()));
  dlg.close();
}

// Desktop runs questionAsync modally: the continuation has the clicked index
// and checkbox state before the call returns, so converted call sites keep
// their synchronous ordering there.
TEST(MessageBoxTest, QuestionAsyncAnswersBeforeReturningOnDesktop) {
  QTimer::singleShot(0, []() {
    auto* box = qobject_cast<PJ::MessageBox*>(QApplication::activeModalWidget());
    ASSERT_NE(box, nullptr);
    box->findChild<QCheckBox*>()->setChecked(true);
    const auto buttons = box->findChildren<QPushButton*>(u"pjMessageBoxButton"_s);
    ASSERT_EQ(buttons.size(), 2);
    buttons[1]->click();
  });
  int choice = -2;
  bool dont_show_again = false;
  PJ::MessageBox::questionAsync(
      nullptr, u"Title"_s, u"Body"_s,
      {{u"Remove"_s, PJ::MessageBox::kDestructiveRole}, {u"Keep"_s, PJ::MessageBox::kCancelRole}},
      [&](int answer, bool checked) {
        choice = answer;
        dont_show_again = checked;
      },
      /*show_dont_show_again=*/true);
  EXPECT_EQ(choice, 1);
  EXPECT_TRUE(dont_show_again);
}

}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);  // QWidget construction + layout need a GUI app
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
