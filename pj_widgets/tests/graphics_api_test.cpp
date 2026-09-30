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

TEST(GraphicsApiTest, RhiWidgetBuildsUseTheNativeApi) {
  EXPECT_EQ(nativeGraphicsApi(GraphicsPlatform::Windows, true), Api::Direct3D11);
  EXPECT_EQ(nativeGraphicsApi(GraphicsPlatform::MacOS, true), Api::Metal);
  EXPECT_EQ(nativeGraphicsApi(GraphicsPlatform::Linux, true), Api::OpenGL);
  EXPECT_EQ(nativeGraphicsApi(GraphicsPlatform::Wasm, true), Api::OpenGL);
}

TEST(GraphicsApiTest, OpenGlWidgetBuildsStayOnOpenGlEverywhere) {
  for (const auto platform :
       {GraphicsPlatform::Windows, GraphicsPlatform::MacOS, GraphicsPlatform::Linux, GraphicsPlatform::Wasm}) {
    EXPECT_EQ(nativeGraphicsApi(platform, false), Api::OpenGL);
  }
}

TEST(GraphicsApiTest, FailureLatchReportsEachOnsetOnce) {
  using Change = RhiFailureLatch::Change;
  RhiFailureLatch latch;
  EXPECT_EQ(latch.set({}), Change::kNone);
  EXPECT_EQ(latch.set(QStringLiteral("no pipeline")), Change::kEntered);
  EXPECT_EQ(latch.set(QStringLiteral("no pipeline")), Change::kNone);
  EXPECT_EQ(latch.set(QStringLiteral("no texture")), Change::kChanged);
  EXPECT_TRUE(latch.failed());
  EXPECT_EQ(latch.reason(), QStringLiteral("no texture"));
  EXPECT_EQ(latch.set({}), Change::kLeft);
  EXPECT_FALSE(latch.failed());
  EXPECT_EQ(latch.set(QStringLiteral("again")), Change::kEntered);
}

TEST(GraphicsApiTest, PreferredApiFollowsTheBuildPolicyOnThisHost) {
  // Each ctest case is its own process, so this is the first (caching) call.
  qunsetenv("PJ_GRAPHICS_API");
  constexpr Api kNative = nativeGraphicsApi(currentGraphicsPlatform(), kRhiWidgetsBuild);
  const Api preferred = preferredGraphicsApi();
  // Metal falls back to OpenGL on a Mac without a Metal device.
  EXPECT_TRUE(preferred == kNative || (kNative == Api::Metal && preferred == Api::OpenGL))
      << static_cast<int>(preferred);
}

}  // namespace
}  // namespace PJ
