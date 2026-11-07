// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// A file dragged over any part of the main window reaches MainWindow's drop
// handling: the widgets that accept drops for curves (plot placeholders, plots,
// docks) leave a file drag ignored, and Qt then offers it to their ancestors.

#include <gtest/gtest.h>

#include <QAbstractSpinBox>
#include <QDragEnterEvent>
#include <QFile>
#include <QLineEdit>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QTemporaryDir>
#include <QTest>
#include <QTextEdit>
#include <QUrl>

#include "MainWindow.h"
#include "pj_widgets/VisualizationPlaceholderWidget.h"
#include "support/gui_test_env.h"

namespace {

// Text inputs take a dropped URL as text, by design.
[[nodiscard]] bool isTextInput(const QWidget* widget) {
  return qobject_cast<const QLineEdit*>(widget) != nullptr || qobject_cast<const QTextEdit*>(widget) != nullptr ||
         qobject_cast<const QPlainTextEdit*>(widget) != nullptr ||
         qobject_cast<const QAbstractSpinBox*>(widget) != nullptr;
}

TEST(MainWindowFileDropTest, FileDragOverAnyDropTargetIsTakenAsACopy) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  QFile file(dir.filePath(QStringLiteral("data.csv")));
  ASSERT_TRUE(file.open(QIODevice::WriteOnly));
  file.write("t,x\n0,1\n");
  file.close();

  PJ::MainWindow window;
  window.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));

  QMimeData mime;
  mime.setUrls({QUrl::fromLocalFile(file.fileName())});

  int placeholders = 0;
  for (QWidget* leaf : window.findChildren<QWidget*>()) {
    if (!leaf->acceptDrops() || !leaf->isVisible() || isTextInput(leaf)) {
      continue;
    }
    if (qobject_cast<PJ::VisualizationPlaceholderWidget*>(leaf) != nullptr) {
      ++placeholders;
    }
    SCOPED_TRACE((leaf->metaObject()->className() + QStringLiteral(" ") + leaf->objectName()).toStdString());

    // Sent to the leaf, as QWidgetWindow does; QApplication::notify walks the ancestors.
    QDragEnterEvent enter(QPointF(1, 1), Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    enter.setDropAction(Qt::MoveAction);
    QApplication::sendEvent(leaf, &enter);
    EXPECT_TRUE(enter.isAccepted());
    EXPECT_EQ(enter.dropAction(), Qt::CopyAction);

    QDragMoveEvent move(QPointF(2, 2), Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    move.setDropAction(Qt::MoveAction);
    QApplication::sendEvent(leaf, &move);
    EXPECT_TRUE(move.isAccepted());
    EXPECT_EQ(move.dropAction(), Qt::CopyAction);

    QDragLeaveEvent leave;
    QApplication::sendEvent(leaf, &leave);
  }
  EXPECT_GT(placeholders, 0) << "the empty window should show a plot placeholder";
}

}  // namespace
