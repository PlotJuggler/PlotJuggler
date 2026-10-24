#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QPoint>

class QMenu;

namespace PJ {

/// Shows a heap-allocated `menu` at `global_pos` and hands it to Qt, which
/// deletes it once it closes. Desktop runs it modally, so its actions have
/// fired by the time this returns; the browser's Asyncify-free build cannot
/// nest an event loop and pops it up instead, returning at once. Wire every
/// choice through a signal handler and never read results back after the call.
void showOwnedMenu(QMenu* menu, const QPoint& global_pos);

}  // namespace PJ
