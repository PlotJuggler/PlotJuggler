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
  QGuiApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
