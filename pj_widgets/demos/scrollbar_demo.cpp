// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Standalone visual demo for PJ::Scrollbar in reserved-gutter placement (the attachPillScrollbars default). Left: a
// table whose trailing column holds per-row action buttons, content an overlay pill would cover and click-steal; its
// pill is pinned visible (pjScrollbarAutoHide=false) so a screenshot shows it inside the reserved gutter, clear of the
// buttons. Middle: the curve-list shape (two-column tree and HeaderResizePolicy), showing the gutter's header band
// painted as the header's continuation, with the groove inset below it. Right: a scroll page with auto-hide left on:
// its gutter reads as an empty strip until hovered or scrolled. All under the real app theme.
//
// Interactive: ./build/pj_widgets/demos/pj_widgets_scrollbar_demo Screenshot :
// ./build/pj_widgets/demos/pj_widgets_scrollbar_demo --screenshot out.png [--theme dark|light]

#include <QApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMainWindow>
#include <QScrollArea>
#include <QScrollBar>
#include <QString>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

#include "pj_widgets/HeaderResizePolicy.h"
#include "pj_widgets/Scrollbar.h"
#include "pj_widgets/Style.h"
#include "qss_preprocessor.h"
using namespace Qt::StringLiterals;

namespace {
using pj_widgets_demos::applyTheme;

QTableWidget* makeTopicsTable(QWidget* parent) {
  auto* table = new QTableWidget(40, 3, parent);
  table->setHorizontalHeaderLabels({u"Topic"_s, u"Value"_s, u""_s});
  table->verticalHeader()->setVisible(false);
  for (int row = 0; row < table->rowCount(); ++row) {
    table->setItem(row, 0, new QTableWidgetItem(u"/robot/joint_%1/position"_s.arg(row)));
    table->setItem(row, 1, new QTableWidgetItem(QString::number(0.017 * row, 'f', 4)));
    auto* action = new QToolButton(table);
    action->setText(u"✕"_s);
    action->setAutoRaise(true);
    table->setCellWidget(row, 2, action);
  }
  table->horizontalHeader()->setStretchLastSection(false);
  table->setColumnWidth(0, 180);
  table->setColumnWidth(1, 80);
  table->setColumnWidth(2, 28);
  // Pin the pill visible so a static screenshot shows it sitting in the
  // reserved gutter, clear of the trailing action buttons.
  table->setProperty("pjScrollbarAutoHide", false);
  return table;
}

// The curve-list shape: a two-column QTreeWidget under HeaderResizePolicy
// (Name fills, Value trails), nested groups, pill pinned visible.
QTreeWidget* makeCurveTree(QWidget* parent) {
  auto* tree = new QTreeWidget(parent);
  tree->setColumnCount(2);
  tree->setHeaderLabels({u"Name"_s, u"Value"_s});
  for (int topic = 0; topic < 8; ++topic) {
    auto* group = new QTreeWidgetItem(tree, QStringList{u"/odometry/topic_%1"_s.arg(topic)});
    for (int field = 0; field < 6; ++field) {
      auto* leaf = new QTreeWidgetItem(group, QStringList{u"field_%1"_s.arg(field), u"-"_s});
      leaf->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
    }
    group->setExpanded(true);
  }
  PJ::HeaderResizePolicy::install(tree->header(), 0, 20);
  tree->setProperty("pjScrollbarAutoHide", false);
  return tree;
}

QScrollArea* makeTextPage(QWidget* parent) {
  auto* area = new QScrollArea(parent);
  auto* page = new QWidget;
  auto* column = new QVBoxLayout(page);
  for (int i = 0; i < 60; ++i) {
    column->addWidget(new QLabel(u"Row %1 — auto-hidden pill: the gutter stays an empty strip"_s.arg(i), page));
  }
  area->setWidget(page);
  area->setWidgetResizable(true);
  return area;
}

}  // namespace

int main(int argc, char** argv) {
  QApplication::setOrganizationName(u"PlotJuggler"_s);
  QApplication::setApplicationName(u"ScrollbarDemo"_s);
  QApplication app(argc, argv);
  QApplication::setStyle(new PJ::Style(u"Fusion"_s));  // same base style as the app

  QString screenshot;
  QString theme = u"dark"_s;
  for (int i = 1; i < argc; ++i) {
    const QString arg = QString::fromLocal8Bit(argv[i]);
    if (arg == "--screenshot"_L1 && i + 1 < argc) {
      screenshot = QString::fromLocal8Bit(argv[++i]);
    } else if (arg == "--theme"_L1 && i + 1 < argc) {
      theme = QString::fromLocal8Bit(argv[++i]);
    }
  }
  applyTheme(theme);

  QMainWindow win;
  win.setWindowTitle(u"PJ::Scrollbar — reserved gutter"_s);

  auto* root = new QWidget(&win);
  auto* row = new QHBoxLayout(root);
  QTableWidget* table = makeTopicsTable(root);
  QTreeWidget* tree = makeCurveTree(root);
  QScrollArea* page = makeTextPage(root);
  row->addWidget(table, 1);
  row->addWidget(tree, 1);
  row->addWidget(page, 1);
  win.setCentralWidget(root);
  win.resize(1000, 360);
  win.show();

  PJ::attachPillScrollbars(&win);

  // Numeric proof of the gutter split: viewport + bar widths must partition the
  // table width (frame aside) — content never extends under the pill.
  auto report = [table]() {
    QScrollBar* bar = table->verticalScrollBar();
    const QPoint bar_tl = bar->mapTo(table, QPoint(0, 0));
    qInfo(
        "GUTTER  table=%d  viewport=%d  bar_w=%d  bar_visible=%d  policy_asneeded=%d  bar_top=%d  header_h=%d",
        table->width(), table->viewport()->width(), bar->width(), bar->isVisible() ? 1 : 0,
        table->verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded ? 1 : 0, bar_tl.y(),
        table->horizontalHeader()->height());
  };

  if (!screenshot.isEmpty()) {
    QTimer::singleShot(300, &win, [&win, screenshot, &app, report] {
      report();
      const QPixmap pix = win.grab();
      if (pix.save(screenshot)) {
        qInfo("[scrollbar_demo] screenshot saved: %s (%dx%d)", qPrintable(screenshot), pix.width(), pix.height());
      } else {
        qWarning("[scrollbar_demo] screenshot FAILED");
      }
      app.quit();
    });
  } else {
    QTimer::singleShot(300, root, report);
  }

  return app.exec();
}
