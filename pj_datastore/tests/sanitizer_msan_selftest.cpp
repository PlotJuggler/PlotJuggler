// SPDX-License-Identifier: MPL-2.0
//
// Proves the MemorySanitizer lane can detect an uninitialised read: a clean MSan run is otherwise
// indistinguishable from a lane that checks nothing.
//
// CTest passes this only when MSan printed its report; a clean exit is a FAILURE. Do not "fix" the read below.
//
// No inline-asm escape: an `asm volatile` with a "memory" clobber tells MSan the asm may have written the
// buffer, so the shadow is unpoisoned and the read reports nothing. A heap allocation the compiler cannot
// reason about, plus a branch, is enough.
#include <cstdio>

int main() {
  // Heap, not stack: the optimiser cannot fold the allocation away.
  int* values = new int[16];
  values[7] = 0;  // one element written, so the allocation is genuinely used

  // The BRANCH on an UNWRITTEN element is the trigger. MSan reports when an
  // uninitialised value reaches a conditional, not merely when it is copied.
  if (values[0] != 0) {
    std::printf("branch taken\n");
  }

  // Reached only when MSan is absent or not instrumenting this binary, which
  // CTest treats as a failure because it means the lane proves nothing.
  std::printf("MSan did NOT report an uninitialised read; instrumentation is not active.\n");
  delete[] values;
  return 0;
}
