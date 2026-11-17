#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>

class QSplitter;
class QWidget;

namespace PJ {

// Docks `drawer` beside `body` as a user-resizable column: the handle is a 1-px hairline, the drawer never collapses
// (its minimumWidth is the drag floor, through setChildrenCollapsible(false)) and the body takes the growth. With a
// non-empty `settings_key` the column's width is remembered under it: restored with QSplitter::restoreState (a stale or
// mismatched blob is rejected, leaving the natural split) and saved with saveState on every drag, debounced so the
// write lands once the handle comes to rest.
QSplitter* makeSideDrawerSplitter(QWidget* drawer, QWidget* body, const QString& settings_key = {});

}  // namespace PJ
