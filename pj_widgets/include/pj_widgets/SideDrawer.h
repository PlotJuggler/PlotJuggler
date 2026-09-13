#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>

class QSplitter;
class QWidget;

namespace PJ {

// Docks `drawer` beside `body` as a user-resizable column: the handle is the
// 1-px hairline a fixed-width separator used to be, the drawer never
// collapses (its own minimumWidth is the drag floor, via
// setChildrenCollapsible(false)) and the body takes the growth. With a
// non-empty `settings_key` the column's width is remembered under it —
// restored via QSplitter::restoreState (a stale or mismatched blob is
// silently rejected, leaving the natural split) and saved via saveState on
// every drag, debounced so the write lands once the handle comes to rest
// rather than per mouse-move.
QSplitter* makeSideDrawerSplitter(QWidget* drawer, QWidget* body, const QString& settings_key = {});

}  // namespace PJ
