// SPDX-License-Identifier: MPL-2.0
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "KeySequence.h"

namespace {

using PJ::KeySequenceMatcher;
using PJ::stepHash;

// Per-position step values for a key sequence. The key codes are synthetic placeholders: these tests exercise
// the generic engine.
std::vector<std::uint32_t> stepsFor(const std::vector<int>& keys) {
  std::vector<std::uint32_t> steps;
  steps.reserve(keys.size());
  for (std::size_t i = 0; i < keys.size(); ++i) {
    steps.push_back(stepHash(i, keys[i]));
  }
  return steps;
}

// Synthetic sequence; kSeq[0] == kSeq[1] so the restart-at-one case is covered.
const std::vector<int> kSeq = {10, 10, 20, 30};

TEST(KeySequenceMatcher, FiresOnlyOnFullSequence) {
  KeySequenceMatcher m(stepsFor(kSeq));
  for (std::size_t i = 0; i + 1 < kSeq.size(); ++i) {
    EXPECT_FALSE(m.feed(kSeq[i])) << "fired early at index " << i;
  }
  EXPECT_TRUE(m.feed(kSeq.back()));
}

TEST(KeySequenceMatcher, WrongKeyResetsProgress) {
  KeySequenceMatcher m(stepsFor(kSeq));
  EXPECT_FALSE(m.feed(kSeq[0]));
  EXPECT_FALSE(m.feed(kSeq[1]));
  EXPECT_FALSE(m.feed(999));  // not part of the sequence
  EXPECT_EQ(m.progress(), 0u);
}

TEST(KeySequenceMatcher, MismatchThatIsAlsoFirstElementRestartsAtOne) {
  KeySequenceMatcher m(stepsFor(kSeq));
  EXPECT_FALSE(m.feed(kSeq[0]));
  EXPECT_FALSE(m.feed(kSeq[1]));  // index -> 2 (kSeq[1] == kSeq[0])
  EXPECT_EQ(m.progress(), 2u);
  // At index 2 the expected key differs, but kSeq[0] equals the first element, so progress restarts at 1 instead of
  // resetting.
  EXPECT_FALSE(m.feed(kSeq[0]));
  EXPECT_EQ(m.progress(), 1u);
}

TEST(KeySequenceMatcher, FiresAgainAfterReset) {
  KeySequenceMatcher m(stepsFor(kSeq));
  for (int k : kSeq) {
    m.feed(k);
  }
  bool second = false;
  for (int k : kSeq) {
    second = m.feed(k);
  }
  EXPECT_TRUE(second);
}

}  // namespace
