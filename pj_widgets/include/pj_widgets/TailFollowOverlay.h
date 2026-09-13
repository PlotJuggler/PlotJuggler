// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <QAbstractScrollArea>
#include <QPointer>
#include <QWidget>

namespace PJ {

// The "jump to the latest" affordance of a tail-following scroll area: a round
// HUD button pinned to the viewport's bottom-right corner, shown only while the
// reader has scrolled away from the end. Clicking scrolls to the end. Parented
// to the viewport so it paints over the content and moves with it.
class TailFollowOverlay : public QWidget {
  Q_OBJECT
 public:
  explicit TailFollowOverlay(QAbstractScrollArea* view);

  // Visible exactly while there is somewhere newer to go.
  void sync();

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  void place();

  QPointer<QAbstractScrollArea> view_;
};

}  // namespace PJ
