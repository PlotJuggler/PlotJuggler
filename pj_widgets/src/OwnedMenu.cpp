// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/OwnedMenu.h"

#include <QMenu>

namespace PJ {

void showOwnedMenu(QMenu* menu, const QPoint& global_pos) {
  menu->setAttribute(Qt::WA_DeleteOnClose);
#ifdef PJ_TARGET_WASM
  menu->popup(global_pos);
#else
  menu->exec(global_pos);
#endif
}

}  // namespace PJ
