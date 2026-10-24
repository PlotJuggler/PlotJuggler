// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The per-backend platform limits the QRhi view reads at initialize(). Qt 6.11's
// QRhi reports no 3D-texture limit, so the view asks each backend itself; this
// holds that answer to the documented or driver-reported value: 2048 on D3D11
// (feature level 10_0 and above: WARP in CI), D3D12 and Metal, and on OpenGL
// and Vulkan what the driver reports (llvmpipe and lavapipe in CI), queried
// here independently of the view. Registered per backend; needs a real display.

#include <gtest/gtest.h>
#include <rhi/qrhi.h>

#include <QApplication>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QRhiWidget>
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
#include <QVulkanFunctions>
#include <QVulkanInstance>
#endif
#include <algorithm>
#include <climits>
#include <cstdint>

#include "pj_bench_probes.h"
#include "pj_scene3d_widgets/rhi_conventions.h"
#include "scene3d_render_test_support.h"
#include "scene_view_test_access.h"

namespace {

using pj::scene3d::SceneViewTestAccess;

// The 3D-texture limit `rhi`'s driver reports; 0 when this build cannot ask.
int driverMax3dTextureSize(QRhi& rhi) {
  switch (rhi.backend()) {
    case QRhi::OpenGLES2: {
      if (!rhi.makeThreadLocalNativeContextCurrent()) {
        return 0;
      }
      GLint limit = 0;
      QOpenGLContext::currentContext()->functions()->glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &limit);
      return limit;
    }
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
    case QRhi::Vulkan: {
      const auto* handles = static_cast<const QRhiVulkanNativeHandles*>(rhi.nativeHandles());
      if (handles == nullptr || handles->inst == nullptr) {
        return 0;
      }
      VkPhysicalDeviceProperties properties{};
      handles->inst->functions()->vkGetPhysicalDeviceProperties(handles->physDev, &properties);
      return static_cast<int>(std::min<uint32_t>(properties.limits.maxImageDimension3D, INT_MAX));
    }
#endif
    default:
      return 0;
  }
}

TEST(Scene3dRhiPlatformTest, Max3dTextureSizeIsTheBackendLimit) {
  PJ_SKIP_ON_HEADLESS_QPA();
  pj::scene3d::test::Scene3DRenderFixture fixture;
  PJ::Scene3DDockWidget& dock = *fixture.dock();
  for (int i = 0; i < 20 && dock.sceneView() == nullptr; ++i) {
    QApplication::processEvents();
  }
  pj::scene3d::SceneViewWidget* view = dock.sceneView();
  ASSERT_NE(view, nullptr);
  fixture.window().resize(320, 240);
  fixture.window().show();
  if (pj::scene3d::test::grab(*view).isNull()) {
    PJ_SKIP_WITHOUT_GPU("no QRhi frame presented");
  }
  QRhi* rhi = pj_bench::rhiOf(*view);
  ASSERT_NE(rhi, nullptr);
  const int limit = SceneViewTestAccess::max3DTextureSize(*view);
  switch (rhi->backend()) {
    case QRhi::D3D11:
    case QRhi::D3D12:
    case QRhi::Metal:
      EXPECT_EQ(limit, pj::scene3d::kDocumentedMax3dTextureSize) << rhi->backendName();
      break;
    case QRhi::OpenGLES2:
    case QRhi::Vulkan: {
      const int reported = driverMax3dTextureSize(*rhi);
      if (reported <= 0) {
        GTEST_SKIP() << "this build cannot query " << rhi->backendName() << " directly";
      }
      EXPECT_EQ(limit, reported) << rhi->backendName();
      break;
    }
    default:
      GTEST_SKIP() << "no documented 3D-texture limit for " << rhi->backendName();
  }
  EXPECT_GE(limit, pj::scene3d::kGuaranteedMax3dTextureSize);
}

}  // namespace
