// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <cstdio>
#include <limits>
#include <vector>

namespace PJ {
namespace {

/// Intentionally faulty; never "fix" this access, because it proves ASan instrumentation is active.
TEST(SanitizerSelfTestDeathTest, HeapUseAfterFree) {
  EXPECT_DEATH(
      {
        volatile int* value = new int(7);
        delete const_cast<int*>(value);
        // The escaped address, memory barrier and volatile read keep the optimizer from removing the fault.
        asm volatile("" : : "r"(value) : "memory");
        std::printf("%d\n", *value);
      },
      "AddressSanitizer");
}

/// Intentionally faulty; never "fix" this overflow, because UBSan must terminate instead of recovering.
TEST(SanitizerSelfTestDeathTest, SignedIntegerOverflow) {
  EXPECT_DEATH(
      {
        // Volatile operands prevent constant folding; printing the result keeps the addition observable.
        volatile int maximum = std::numeric_limits<int>::max();
        volatile int increment = 1;
        const int result = maximum + increment;
        std::printf("%d\n", result);
      },
      "runtime error");
}

/// Intentionally faulty; never "fix" this access, because it verifies libstdc++ vector annotations.
/// Gated on the annotations actually being compiled in: they are OFF by default because they are
/// inline weak symbols, so enabling them while prebuilt Conan C++ packages were built without them
/// produces boundary false positives (see PJ_SANITIZE_CONTAINERS in cmake/PjSanitizers.cmake).
#if defined(_GLIBCXX_SANITIZE_VECTOR)
TEST(SanitizerSelfTestDeathTest, VectorContainerOverflow) {
  EXPECT_DEATH(
      {
        std::vector<int> values;
        values.reserve(64);
        values.push_back(1);
        volatile int* data = values.data();
        // Preserve the read while staying within the allocation, so only container annotations can catch it.
        asm volatile("" : : "r"(data) : "memory");
        std::printf("%d\n", data[10]);
      },
      "container-overflow");
}
#endif  // _GLIBCXX_SANITIZE_VECTOR

}  // namespace
}  // namespace PJ
