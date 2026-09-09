// SPDX-License-Identifier: MPL-2.0
#include <gtest/gtest.h>

#include <QGuiApplication>
#include <QKeyEvent>
#include <Qt>
#include <vector>

#include "KeySequence.h"

namespace {

using PJ::KeySequenceWatcher;
using PJ::unlockSteps;

// The real activation keys. This is the single place in the tree that spells
// them out; the shipped code carries only the precomputed step values. Driving
// these keys through unlockSteps() end-to-end also verifies those constants.
std::vector<int> activationKeys() {
  return {Qt::Key_Up,    Qt::Key_Up,   Qt::Key_Down,  Qt::Key_Down, Qt::Key_Left,
          Qt::Key_Right, Qt::Key_Left, Qt::Key_Right, Qt::Key_B,    Qt::Key_A};
}

QKeyEvent makePress(int key, bool autorepeat = false) {
  return QKeyEvent(QEvent::KeyPress, key, Qt::NoModifier, QString(), autorepeat);
}

TEST(KeySequenceWatcher, FiresOnceOnFullSequenceAndNeverConsumes) {
  int fired = 0;
  KeySequenceWatcher w(unlockSteps(), [&] { ++fired; });
  for (int key : activationKeys()) {
    QKeyEvent e = makePress(key);
    EXPECT_FALSE(w.eventFilter(nullptr, &e));  // returns false => not consumed
  }
  EXPECT_EQ(fired, 1);
}

TEST(KeySequenceWatcher, IgnoresAutoRepeat) {
  int fired = 0;
  KeySequenceWatcher w(unlockSteps(), [&] { ++fired; });
  const auto seq = activationKeys();
  QKeyEvent first = makePress(seq[0]);
  w.eventFilter(nullptr, &first);
  QKeyEvent repeat = makePress(seq[0], /*autorepeat=*/true);  // ignored
  w.eventFilter(nullptr, &repeat);
  for (std::size_t i = 1; i < seq.size(); ++i) {
    QKeyEvent e = makePress(seq[i]);
    w.eventFilter(nullptr, &e);
  }
  EXPECT_EQ(fired, 1);
}

// A single physical key press is delivered to an application-level event filter
// once per widget as it propagates up the focus parent chain; those re-deliveries
// share one timestamp. The watcher must collapse them so the matcher advances
// only once per press (regression test for the propagation-duplication bug).
TEST(KeySequenceWatcher, CollapsesSameTimestampReDeliveries) {
  const auto seq = activationKeys();
  int fired = 0;
  KeySequenceWatcher w(unlockSteps(), [&] { ++fired; });
  quint64 ts = 1000;
  for (int key : seq) {
    for (int hop = 0; hop < 5; ++hop) {  // 5 propagation re-deliveries of one press
      QKeyEvent e(QEvent::KeyPress, key, Qt::NoModifier);
      e.setTimestamp(ts);
      w.eventFilter(nullptr, &e);
    }
    ts += 10;  // next physical press: a distinct timestamp
  }
  EXPECT_EQ(fired, 1);
}

}  // namespace
