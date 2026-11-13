// SPDX-License-Identifier: MPL-2.0
// Shared main() for QCoreApplication test runners (pj_add_test_runner MAIN core).
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <clocale>

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  QCoreApplication app(argc, argv);
  std::setlocale(LC_NUMERIC, "C");  // as pj_app's main(): Luau cannot read "0.5" under a decimal-comma locale
  return RUN_ALL_TESTS();
}
