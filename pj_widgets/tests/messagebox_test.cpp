// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Display contract for PJ::MessageBox: a button label that is wider than the
// dialog's bounded width must wrap to multiple lines rather than clip, while a
// short label is left untouched. The dialog must always respect its maximum
// width, and a multi-line word-wrapped body must not clip.

#include "pj_widgets/MessageBox.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QFontMetrics>
#include <QLabel>
#include <QList>
#include <QPoint>
#include <QPushButton>
#include <QTimer>
#include <algorithm>

#include "pj_widgets/FrameworkTokens.h"
using namespace Qt::StringLiterals;

namespace {

// Force the button to be laid out so width()/sizeHint() reflect the final,
// wrapped geometry. show() triggers Polish (label wrapping) then the
// showEvent() resize; the second processEvents() lets that resize's relayout
// settle before we measure.
void realize(PJ::MessageBox& dlg) {
  dlg.show();
  QCoreApplication::processEvents();
  QCoreApplication::processEvents();
}

// Vertical gaps (px) between consecutive stacked buttons, ordered top→bottom.
QList<int> buttonGaps(PJ::MessageBox& dlg) {
  auto buttons = dlg.findChildren<QPushButton*>(u"pjMessageBoxButton"_s);
  std::sort(buttons.begin(), buttons.end(), [&dlg](QPushButton* a, QPushButton* b) {
    return a->mapTo(&dlg, QPoint(0, 0)).y() < b->mapTo(&dlg, QPoint(0, 0)).y();
  });
  QList<int> gaps;
  for (int i = 1; i < buttons.size(); ++i) {
    const int prev_bottom = buttons[i - 1]->mapTo(&dlg, QPoint(0, 0)).y() + buttons[i - 1]->height();
    gaps.push_back(buttons[i]->mapTo(&dlg, QPoint(0, 0)).y() - prev_bottom);
  }
  return gaps;
}

// The body label is reachable by its objectName (no test-only accessor needed).
QLabel* bodyLabelOf(PJ::MessageBox& box) {
  return box.findChild<QLabel*>(u"pjMessageBoxBody"_s);
}

// Regression: the styled card breaks heightForWidth propagation, so a verbose
// question() body (e.g. the destructive dataset-merge confirmation) opened one
// line tall unless the dialog grows to the wrapped height on first show.
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
  realize(dlg);

  QLabel* body = bodyLabelOf(dlg);
  ASSERT_NE(body, nullptr);
  ASSERT_GT(body->width(), 0);
  EXPECT_GE(body->height(), body->heightForWidth(body->width()))
      << "body height " << body->height() << " < wrapped height " << body->heightForWidth(body->width());
  // Sanity: the text genuinely wrapped to several lines.
  const QFontMetrics fm(body->font());
  EXPECT_GT(body->height(), fm.height() * 3);
}

TEST(MessageBoxTest, ShortBodyAlsoFits) {
  PJ::MessageBox dlg;
  dlg.setTitle(u"Heads up"_s);
  dlg.setText(u"All good."_s);
  dlg.addButton(u"OK"_s, PJ::MessageBox::kPrimaryRole);
  realize(dlg);

  QLabel* body = bodyLabelOf(dlg);
  ASSERT_NE(body, nullptr);
  ASSERT_GT(body->width(), 0);
  EXPECT_GE(body->height(), body->heightForWidth(body->width()));
}

// Desktop runs questionAsync modally: the continuation has the clicked index
// and checkbox state before the call returns.
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

TEST(MessageBoxTest, ShortLabelIsNotWrapped) {
  PJ::MessageBox dlg;
  dlg.setTitle(u"Title"_s);
  dlg.setText(u"Body."_s);
  QPushButton* ok = dlg.addButton(u"OK"_s, PJ::MessageBox::kPrimaryRole);
  realize(dlg);
  // No soft break inserted for a label that already fits on one line.
  EXPECT_FALSE(ok->text().contains(QLatin1Char('\n')));
  EXPECT_EQ(ok->text(), u"OK"_s);
}

TEST(MessageBoxTest, AsyncButtonCanKeepDialogOpenUntilCallbackCompletes) {
  PJ::MessageBox dlg;
  QPushButton* select = dlg.addButton(u"Select file"_s, PJ::MessageBox::kPrimaryRole, /*close_on_click=*/false);
  bool finished = false;
  QObject::connect(&dlg, &QDialog::finished, [&finished](int) { finished = true; });
  realize(dlg);

  select->click();
  QCoreApplication::processEvents();

  EXPECT_EQ(dlg.clickedIndex(), 0);
  EXPECT_FALSE(finished);
  EXPECT_TRUE(dlg.isVisible());
  dlg.accept();
}

TEST(MessageBoxTest, LongLabelWrapsInsteadOfClipping) {
  PJ::MessageBox dlg;
  dlg.setTitle(u"Confirm"_s);
  dlg.setText(u"Are you sure?"_s);
  QPushButton* btn = dlg.addButton(
      u"Reload the original file from disk and discard all unsaved local edits permanently"_s,
      PJ::MessageBox::kPrimaryRole);
  dlg.addButton(u"Cancel"_s, PJ::MessageBox::kCancelRole);
  realize(dlg);

  // The dialog never exceeds its width cap.
  EXPECT_LE(dlg.width(), dlg.maximumWidth());
  // The long label is broken across lines...
  EXPECT_TRUE(btn->text().contains(QLatin1Char('\n')));
  // ...so the button no longer demands more width than it is actually given:
  // its minimum width hint fits within its laid-out width (i.e. not clipped).
  EXPECT_LE(btn->minimumSizeHint().width(), btn->width());
}

TEST(MessageBoxTest, WrapsCorrectlyUnderLargerStyledFont) {
  // Regression: the wrap budget must be measured with the button's *rendered*
  // (QSS-applied) font, not the default font. A stylesheet that enlarges the
  // font reproduces the bug where labels were wrapped with stale metrics and
  // the lines then overflowed (clipped) at the real, larger font size.
  const QString prev = qApp->styleSheet();
  qApp->setStyleSheet(u"QWidget { font-size: 16pt; }"_s);
  {
    PJ::MessageBox dlg;
    dlg.setText(u"Body."_s);
    QPushButton* btn = dlg.addButton(
        u"Reload the original file from disk and discard all unsaved local edits permanently"_s,
        PJ::MessageBox::kPrimaryRole);
    dlg.addButton(u"Cancel"_s, PJ::MessageBox::kCancelRole);
    realize(dlg);
    EXPECT_TRUE(btn->text().contains(QLatin1Char('\n')));
    EXPECT_LE(btn->minimumSizeHint().width(), btn->width());
  }
  qApp->setStyleSheet(prev);
}

TEST(MessageBoxTest, WrappedButtonGrowsTaller) {
  PJ::MessageBox dlg;
  dlg.setText(u"Body."_s);
  QPushButton* shortb = dlg.addButton(u"OK"_s, PJ::MessageBox::kPrimaryRole);
  QPushButton* longb = dlg.addButton(
      u"Reload the original file from disk and discard all unsaved local edits permanently"_s,
      PJ::MessageBox::kNeutralRole);
  realize(dlg);
  // Multi-line wrapping makes the long button visibly taller than the 1-line one.
  EXPECT_GT(longb->height(), shortb->height());
}

TEST(MessageBoxTest, ButtonSpacingIsConstantAcrossDialogs) {
  // A wrapping body used to over-constrain the layout, which then compressed
  // the gaps between buttons by a variable amount (measured 5–14 px across
  // dialogs). Every gap must now use Comfortable spacing regardless of button
  // count or body length. The compression only reproduces under the app's QSS
  // metrics (10pt font + the button min-height/padding that make the dialog
  // tall enough to over-constrain), so apply a matching stylesheet here.
  const int kExpectedGap = PJ::theme::space(PJ::theme::Space::Comfortable);
  const QString prev = qApp->styleSheet();
  qApp->setStyleSheet(QStringLiteral(
                          "QWidget { font-size: 10pt; }"
                          "QPushButton#pjMessageBoxButton { min-height: 26px; padding: %1px %2px; border: none; }")
                          .arg(PJ::theme::space(PJ::theme::Space::Comfortable))
                          .arg(PJ::theme::space(PJ::theme::Space::Section)));

  PJ::MessageBox three;
  three.setTitle(u"Load Layout"_s);
  three.setText(QStringLiteral(
      "This layout was saved with 1 data source(s):\n"
      "  testdata/layout-source.mcap\n\n"
      "Reload them, or apply the layout to the currently loaded data?"));
  three.addButton(u"Reload original"_s, PJ::MessageBox::kPrimaryRole);
  three.addButton(u"Use current data"_s, PJ::MessageBox::kNeutralRole);
  three.addButton(u"Cancel"_s, PJ::MessageBox::kCancelRole);
  realize(three);
  const QList<int> three_gaps = buttonGaps(three);
  ASSERT_EQ(three_gaps.size(), 2);
  for (int gap : three_gaps) {
    EXPECT_EQ(gap, kExpectedGap);
  }

  PJ::MessageBox two;
  two.setText(u"Are you sure you want to remove 'layout-source.mcap' and its data?"_s);
  two.addButton(u"Remove"_s, PJ::MessageBox::kDestructiveRole);
  two.addButton(u"Cancel"_s, PJ::MessageBox::kCancelRole);
  realize(two);
  const QList<int> two_gaps = buttonGaps(two);
  ASSERT_EQ(two_gaps.size(), 1);
  EXPECT_EQ(two_gaps.front(), kExpectedGap);

  qApp->setStyleSheet(prev);
}

}  // namespace
