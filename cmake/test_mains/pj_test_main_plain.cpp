// SPDX-License-Identifier: MPL-2.0
// Shared main() for test runners with no Qt dependency (pj_add_test_runner MAIN plain).
//
// Modules that are Qt-free by design (pj_datastore, pj_scripting) previously had
// to select MAIN core, which links Qt6::Core and constructs a QCoreApplication,
// because core was the least-Qt option the macro offered. That made their
// executables depend on Qt even though not one of their test sources references
// it, and it is what keeps MemorySanitizer out of reach: MSan needs every writer
// of memory in the process instrumented, and Qt is consumed prebuilt.
#include <gtest/gtest.h>

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
