// SPDX-License-Identifier: MPL-2.0
//
// Proves the MemorySanitizer lane can actually detect an uninitialised read.
//
// Without this, a clean MSan run is indistinguishable from a lane that is not
// checking anything: both print "100% tests passed". Every silent-instrumentation
// bug this project has hit — an uninstrumented SDK fetched as instrumented, a
// stale CMake cache re-enabling container annotations, Conan labelled with Clang
// while invoking GCC — looked exactly like success.
//
// CTest passes this only when MSan printed its report; a clean exit is a
// FAILURE. Never "fix" the read below; it is the instrument, not a defect.
//
// Deliberately does NOT use an inline-asm escape to defeat the optimiser. An
// `asm volatile` with a "memory" clobber tells MSan the asm may have WRITTEN the
// buffer, so the shadow is unpoisoned and the read reports nothing — the first
// version of this file did exactly that and passed vacuously, which is the
// failure mode it exists to catch. A heap allocation the compiler cannot prove
// anything about, plus a branch, is enough on its own.
#include <cstdio>

int main() {
  // Heap, not stack: the optimiser cannot fold this away, and no escape hatch
  // is needed to keep the read alive.
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
