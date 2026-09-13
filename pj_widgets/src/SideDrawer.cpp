// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/SideDrawer.h"

#include <QSettings>
#include <QSplitter>
#include <QTimer>

namespace PJ {

namespace {
constexpr int kSaveDebounceMs = 300;
}  // namespace

QSplitter* makeSideDrawerSplitter(QWidget* drawer, QWidget* body, const QString& settings_key) {
  auto* splitter = new QSplitter(Qt::Horizontal);
  splitter->setChildrenCollapsible(false);
  splitter->setHandleWidth(1);
  splitter->addWidget(drawer);
  splitter->addWidget(body);
  splitter->setStretchFactor(0, 0);
  splitter->setStretchFactor(1, 1);

  if (!settings_key.isEmpty()) {
    const QByteArray saved = QSettings().value(settings_key).toByteArray();
    if (!saved.isEmpty()) {
      splitter->restoreState(saved);  // a stale/mismatched blob is silently rejected; natural split stays
    }
    // Debounced because splitterMoved fires per mouse-move: only where the
    // handle comes to rest is worth a write.
    auto* save_timer = new QTimer(splitter);
    save_timer->setSingleShot(true);
    save_timer->setInterval(kSaveDebounceMs);
    QObject::connect(save_timer, &QTimer::timeout, splitter, [splitter, settings_key]() {
      QSettings().setValue(settings_key, splitter->saveState());
    });
    QObject::connect(splitter, &QSplitter::splitterMoved, save_timer, [save_timer](int, int) { save_timer->start(); });
  }
  return splitter;
}

}  // namespace PJ
