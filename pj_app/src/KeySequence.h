// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QObject>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace PJ {

// The position salts the hash, so the configured step values do not reveal repeated keys.
constexpr std::uint32_t stepHash(std::size_t pos, int key) {
  constexpr std::uint32_t kOffset = 0x811c9dc5u;
  constexpr std::uint32_t kPrime = 0x01000193u;
  constexpr std::uint32_t kSalt = 0x9e3779b9u;
  std::uint32_t h = kOffset;
  h = (h ^ (static_cast<std::uint32_t>(pos) + kSalt)) * kPrime;
  h = (h ^ static_cast<std::uint32_t>(key)) * kPrime;
  return h;
}

// One stepHash value per position. feed() returns true when the latest keys complete the sequence, then
// resets; a mismatch resets and re-tests the current key as a new start. Qt-free.
class KeySequenceMatcher {
 public:
  explicit KeySequenceMatcher(std::vector<std::uint32_t> steps);

  bool feed(int key);
  void reset() noexcept;
  [[nodiscard]] std::size_t progress() const noexcept {
    return index_;
  }

 private:
  std::vector<std::uint32_t> steps_;
  std::size_t index_ = 0;
};

std::vector<std::uint32_t> unlockSteps();

// Event filter for qApp; never consumes an event. QApplication re-delivers an unaccepted key event to app-level
// filters once per widget up the focus chain, all with one timestamp, so presses are collapsed by timestamp
// (timestamp 0, as in synthetic test events, is never collapsed).
class KeySequenceWatcher : public QObject {
  Q_OBJECT
 public:
  KeySequenceWatcher(std::vector<std::uint32_t> steps, std::function<void()> on_match, QObject* parent = nullptr);

  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  KeySequenceMatcher matcher_;
  std::function<void()> on_match_;
  quint64 last_key_timestamp_ = 0;
};

}  // namespace PJ
