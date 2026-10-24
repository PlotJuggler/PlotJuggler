// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>
#include <rhi/qrhi.h>

#include <memory>

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

TEST(GraphicsApiTest, EachPlatformUsesItsNativeApi) {
  EXPECT_EQ(nativeGraphicsApi(GraphicsPlatform::Windows), Api::Direct3D11);
  EXPECT_EQ(nativeGraphicsApi(GraphicsPlatform::MacOS), Api::Metal);
  EXPECT_EQ(nativeGraphicsApi(GraphicsPlatform::Linux), Api::OpenGL);
  EXPECT_EQ(nativeGraphicsApi(GraphicsPlatform::Wasm), Api::OpenGL);
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
  constexpr Api kNative = nativeGraphicsApi(currentGraphicsPlatform());
  const Api preferred = preferredGraphicsApi();
  // Metal falls back to OpenGL on a Mac without a Metal device.
  EXPECT_TRUE(preferred == kNative || (kNative == Api::Metal && preferred == Api::OpenGL))
      << static_cast<int>(preferred);
}

TEST(GraphicsApiTest, AnOverrideThatCannotStartFallsBackToTheBuildPolicy) {
  // Each ctest case is its own process, so this is the first (caching) call.
  // Metal has no Qt backend off Apple platforms, D3D11 none off Windows.
  qputenv("PJ_GRAPHICS_API", currentGraphicsPlatform() == GraphicsPlatform::MacOS ? "d3d11" : "metal");
  constexpr Api kNative = nativeGraphicsApi(currentGraphicsPlatform());
  const Api preferred = preferredGraphicsApi();
  qunsetenv("PJ_GRAPHICS_API");
  EXPECT_TRUE(preferred == kNative || (kNative == Api::Metal && preferred == Api::OpenGL))
      << static_cast<int>(preferred);
}

// A static upload issued in pieces lands in the buffer exactly as one upload
// would, on either side of the 64 KiB split, with an offset.
TEST(GraphicsApiTest, UploadStaticBufferInPiecesWritesEveryByte) {
  QRhiNullInitParams params;
  std::unique_ptr<QRhi> rhi(QRhi::create(QRhi::Null, &params));
  ASSERT_NE(rhi, nullptr);
  for (const qsizetype size : {qsizetype{1000}, qsizetype{65536}, qsizetype{3 * 65536 + 12345}}) {
    SCOPED_TRACE(size);
    QByteArray data(size, Qt::Uninitialized);
    for (qsizetype i = 0; i < size; ++i) {
      data[i] = static_cast<char>((i * 7 + i / 251) & 0xff);
    }
    constexpr quint32 kOffset = 16;
    for (const bool shared : {true, false}) {
      std::unique_ptr<QRhiBuffer> buffer(
          rhi->newBuffer(QRhiBuffer::Static, QRhiBuffer::VertexBuffer, static_cast<quint32>(size) + kOffset));
      ASSERT_TRUE(buffer->create());
      QRhiResourceUpdateBatch* updates = rhi->nextResourceUpdateBatch();
      if (shared) {
        uploadStaticBufferInPieces(updates, buffer.get(), kOffset, data);
      } else {
        uploadStaticBufferInPieces(updates, buffer.get(), kOffset, static_cast<quint32>(size), data.constData());
      }
      QRhiReadbackResult readback;
      updates->readBackBuffer(buffer.get(), kOffset, static_cast<quint32>(size), &readback);
      QRhiCommandBuffer* command_buffer = nullptr;
      ASSERT_EQ(rhi->beginOffscreenFrame(&command_buffer), QRhi::FrameOpSuccess);
      command_buffer->resourceUpdate(updates);
      ASSERT_EQ(rhi->endOffscreenFrame(), QRhi::FrameOpSuccess);
      EXPECT_EQ(readback.data, data) << (shared ? "shared" : "copied");
    }
  }
}

// Up to one granule an array keeps its size; above it, it grows to the next
// whole granule, the records kept and the rest zeroed.
TEST(GraphicsApiTest, PadForUploadRoundsPastOneGranuleOnly) {
  EXPECT_EQ(paddedUploadSize(0), 0);
  EXPECT_EQ(paddedUploadSize(kUploadGranule), kUploadGranule);
  EXPECT_EQ(paddedUploadSize(kUploadGranule + 1), 2 * kUploadGranule);
  EXPECT_EQ(paddedUploadSize(3 * kUploadGranule), 3 * kUploadGranule);
  for (const qsizetype used : {qsizetype{100}, kUploadGranule + 100}) {
    SCOPED_TRACE(used);
    QByteArray bytes(paddedUploadSize(used), 'x');
    padForUpload(bytes, used);
    ASSERT_EQ(bytes.size(), paddedUploadSize(used));
    EXPECT_EQ(bytes.count('x'), used);
    EXPECT_EQ(bytes.indexOf('\0'), used == 100 ? -1 : used);
  }
}

}  // namespace
}  // namespace PJ
