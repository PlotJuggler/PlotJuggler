// SPDX-License-Identifier: MPL-2.0
// Shared main() for QCoreApplication test runners (pj_add_test_runner MAIN core).
#include <gtest/gtest.h>

#include <QCoreApplication>

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  QCoreApplication app(argc, argv);
  return RUN_ALL_TESTS();
}
