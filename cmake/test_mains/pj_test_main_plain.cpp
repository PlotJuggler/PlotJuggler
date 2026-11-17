// SPDX-License-Identifier: MPL-2.0
// Shared main() for test runners with no Qt dependency (pj_add_test_runner MAIN plain).
//
// For modules that are Qt-free by design (pj_datastore, pj_scripting): MAIN core links
// Qt6::Core and constructs a QCoreApplication, which would make their test executables
// depend on Qt and keep MemorySanitizer out of reach (MSan needs every writer of memory
// in the process instrumented, and Qt is consumed prebuilt).
#include <gtest/gtest.h>

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
