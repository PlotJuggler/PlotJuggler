// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "support/modal_answerer.h"

#include <gtest/gtest.h>

#include <QDialog>
#include <QLineEdit>
#include <QTimer>
#include <memory>

using namespace Qt::StringLiterals;

namespace {

std::unique_ptr<QDialog> modalDialog(const QString& name) {
  auto dialog = std::make_unique<QDialog>();
  dialog->setObjectName(name);
  dialog->setWindowModality(Qt::ApplicationModal);
  return dialog;
}

// The batch-load race: a box no rule matches opens first and stays open, then the dialog a rule waits for opens over
// it. The box is left alone and listed, the dialog is answered, whichever of the two a tick sees first.
TEST(ModalAnswerer, LeavesAnUnmatchedDialogOpenAndAnswersTheOneItMatches) {
  pj_app_test::ModalAnswerer answerer(1);
  int answered = 0;
  answerer.every(pj_app_test::hasChild<QLineEdit>(u"name_input"_s), [&answered](QWidget* dialog) {
    ++answered;
    QMetaObject::invokeMethod(qobject_cast<QDialog*>(dialog), &QDialog::accept, Qt::QueuedConnection);
  });

  const auto box = modalDialog(u"loadFailed"_s);
  box->show();
  const auto wanted = modalDialog(u"probe"_s);
  new QLineEdit(wanted.get());
  wanted->findChild<QLineEdit*>()->setObjectName(u"name_input"_s);
  // Opened a few ticks after the box; a stuck exec() fails the test instead of hanging it.
  QTimer::singleShot(5000, wanted.get(), &QDialog::reject);
  int result = -1;
  QTimer::singleShot(20, wanted.get(), [&wanted, &result]() { result = wanted->exec(); });
  while (result == -1) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  }

  EXPECT_EQ(result, QDialog::Accepted);
  EXPECT_EQ(answered, 1);
  EXPECT_TRUE(box->isVisible()) << "the unmatched box is left open";
  EXPECT_EQ(answerer.unmatched(), QStringList{u"QDialog#loadFailed "_s});
  EXPECT_EQ(answerer.answered(), 1);
}

// next() answers one dialog and retires: a second matching dialog is left to the test.
TEST(ModalAnswerer, NextAnswersOnlyOneDialog) {
  pj_app_test::ModalAnswerer answerer(1);
  answerer.next(
      [](QWidget* dialog) { return dialog->objectName() == u"question"_s; },
      [](QWidget* dialog) {
        QMetaObject::invokeMethod(qobject_cast<QDialog*>(dialog), &QDialog::accept, Qt::QueuedConnection);
      });

  const auto first = modalDialog(u"question"_s);
  QTimer::singleShot(5000, first.get(), &QDialog::reject);
  EXPECT_EQ(first->exec(), QDialog::Accepted);

  const auto second = modalDialog(u"question"_s);
  QTimer::singleShot(300, second.get(), &QDialog::reject);
  EXPECT_EQ(second->exec(), QDialog::Rejected) << "only the timeout closes the second one";
  EXPECT_EQ(answerer.answered(), 1);
}

}  // namespace
