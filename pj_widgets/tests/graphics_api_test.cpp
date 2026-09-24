// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include "pj_widgets/GraphicsApi.h"

namespace PJ {
namespace {

using Api = QRhiWidget::Api;

TEST(GraphicsApiTest, ParsesEveryBackendCaseInsensitively) {
  EXPECT_EQ(parseGraphicsApi(QStringLiteral("opengl")), Api::OpenGL);
  EXPECT_EQ(parseGraphicsApi(QStringLiteral("Vulkan")), Api::Vulkan);
  EXPECT_EQ(parseGraphicsApi(QStringLiteral("METAL")), Api::Metal);
  EXPECT_EQ(parseGraphicsApi(QStringLiteral("d3d11")), Api::Direct3D11);
  EXPECT_EQ(parseGraphicsApi(QStringLiteral("D3D12")), Api::Direct3D12);
}

TEST(GraphicsApiTest, RejectsUnknownNames) {
  EXPECT_EQ(parseGraphicsApi(QStringLiteral("")), std::nullopt);
  EXPECT_EQ(parseGraphicsApi(QStringLiteral("gl")), std::nullopt);
  EXPECT_EQ(parseGraphicsApi(QStringLiteral("null")), std::nullopt);
}

}  // namespace
}  // namespace PJ
