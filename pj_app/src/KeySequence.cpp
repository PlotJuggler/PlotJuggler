// SPDX-License-Identifier: MPL-2.0
#include "KeySequence.h"

#include <QEvent>
#include <QKeyEvent>
#include <utility>

namespace PJ {

KeySequenceMatcher::KeySequenceMatcher(std::vector<std::uint32_t> steps) : steps_(std::move(steps)) {}

void KeySequenceMatcher::reset() noexcept {
  index_ = 0;
}

bool KeySequenceMatcher::feed(int key) {
  if (steps_.empty()) {
    return false;
  }
  if (stepHash(index_, key) == steps_[index_]) {
    ++index_;
  } else {
    index_ = (stepHash(0, key) == steps_[0]) ? 1 : 0;
  }
  if (index_ == steps_.size()) {
    index_ = 0;
    return true;
  }
  return false;
}

std::vector<std::uint32_t> unlockSteps() {
  return {
      0x6adbba65u, 0xd4e358dau, 0xb8e0b25du, 0xd2d44b2au, 0xf1d2018eu,
      0x3fd973efu, 0x39d6f014u, 0x92b4c891u, 0xd5b20c0au, 0xc4ba400cu,
  };
}

KeySequenceWatcher::KeySequenceWatcher(
    std::vector<std::uint32_t> steps, std::function<void()> on_match, QObject* parent)
    : QObject(parent), matcher_(std::move(steps)), on_match_(std::move(on_match)) {}

bool KeySequenceWatcher::eventFilter(QObject* watched, QEvent* event) {
  if (event->type() == QEvent::KeyPress) {
    auto* key_event = static_cast<QKeyEvent*>(event);
    const quint64 ts = key_event->timestamp();
    // One physical press, re-delivered per widget (see header).
    const bool duplicate = (ts != 0 && ts == last_key_timestamp_);
    last_key_timestamp_ = ts;
    if (!duplicate && !key_event->isAutoRepeat()) {
      if (matcher_.feed(key_event->key()) && on_match_) {
        on_match_();
      }
    }
  }
  return QObject::eventFilter(watched, event);  // false: never consume
}

}  // namespace PJ
