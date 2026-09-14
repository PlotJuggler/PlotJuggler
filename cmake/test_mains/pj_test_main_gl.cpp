// SPDX-License-Identifier: MPL-2.0
// Shared main() for OpenGL test runners (pj_add_test_runner MAIN gl): the default
// surface format must be set before the QGuiApplication exists.
#include <gtest/gtest.h>

#include <QGuiApplication>
#include <QSurfaceFormat>

int main(int argc, char** argv) {
  QSurfaceFormat fmt;
  fmt.setVersion(4, 5);
  fmt.setProfile(QSurfaceFormat::CoreProfile);
  fmt.setDepthBufferSize(24);
  QSurfaceFormat::setDefaultFormat(fmt);
  ::testing::InitGoogleTest(&argc, argv);
  // ctest discovery (--gtest_list_tests) only needs the case names: answer it
  // before Qt starts, because a cold fontconfig cache can push QApplication
  // construction past the 5 s discovery timeout on a fresh CI runner.
  if (::testing::GTEST_FLAG(list_tests)) {
    return RUN_ALL_TESTS();
  }
  QGuiApplication app(argc, argv);
  return RUN_ALL_TESTS();
}
