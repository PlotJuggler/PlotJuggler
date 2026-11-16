// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// A Preferences segmented selector (DualOptionsWidget) paints its segments from
// the width it is given, so a page layout that squeezes it below its sizeHint
// overlaps the option labels ("memes"/"serious" turn into "hemeeriou"). The
// dialog must therefore never be narrower than its pages need.

#include <gtest/gtest.h>

#include <QApplication>
#include <QSize>
#include <QStackedWidget>

#include "PreferencesDialog.h"
#include "Theme.h"
#include "pj_widgets/DualOptionsWidget.h"

using namespace Qt::StringLiterals;

namespace {

void settle() {
  for (int pass = 0; pass < 5; ++pass) {
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
  }
}

// Opens the dialog (resized to `requested` when valid) and checks every selector
// on every page against its own sizeHint.
void expectSelectorsKeepTheirSizeHint(const QSize& requested) {
  PJ::Theme theme;
  PJ::PreferencesDialog dialog(theme);
  if (requested.isValid()) {
    dialog.resize(requested);
  }
  dialog.show();
  auto* pages = dialog.findChild<QStackedWidget*>(u"pagesStack"_s);
  ASSERT_NE(pages, nullptr);

  int selectors = 0;
  for (int index = 0; index < pages->count(); ++index) {
    pages->setCurrentIndex(index);
    settle();
    for (auto* selector : pages->widget(index)->findChildren<PJ::DualOptionsWidget*>()) {
      ++selectors;
      EXPECT_GE(selector->width(), selector->sizeHint().width())
          << selector->objectName().toStdString() << " on page " << index << " is " << selector->width() << "x"
          << selector->height() << " in a " << dialog.width() << "x" << dialog.height() << " dialog, wants "
          << selector->sizeHint().width() << "x" << selector->sizeHint().height();
    }
  }
  EXPECT_GE(selectors, 4);  // splashMode, curveColorMode, derivedBatchMode, clockTimeZone
}

TEST(PreferencesSelectorSizeTest, KeepTheirSizeHintAtTheDefaultDialogSize) {
  expectSelectorsKeepTheirSizeHint(QSize{});
}

TEST(PreferencesSelectorSizeTest, KeepTheirSizeHintWhenTheUserShrinksTheDialogAsFarAsItGoes) {
  expectSelectorsKeepTheirSizeHint(QSize{1, 1});
}

}  // namespace
