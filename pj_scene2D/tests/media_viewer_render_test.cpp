// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Pixel checks of MediaViewerWidget on every QRhi backend of the platform.
//
// Each case shows one viewer in a top-level, hands it one frame and grabs a
// presented frame. Expected colours come from the CPU side of the same math
// (buildYuvMatrix, colorFor), so the checks catch what differs between backends:
// texture formats, plane layout, uniform layout, orientation and scissor origin.
// Needs a real QPA (pjPreferRealDisplayQpa); CI runs it under Xvfb on Mesa
// llvmpipe and lavapipe, on D3D11 WARP and on Metal, and PJ_REQUIRE_GPU_TESTS=1
// turns a missing backend into a failure.

#include <gtest/gtest.h>

#include <QColor>
#include <QImage>
#include <QTest>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "pj_gpu_test_gate.h"
#include "pj_scene2d_core/decoded_frame.h"
#include "pj_scene2d_core/media_frame.h"
#include "pj_scene2d_core/media_source.h"
#include "pj_scene2d_core/scene_frame.h"
#include "pj_scene2d_core/undistort_remap.h"
#include "pj_scene2d_core/video_color.h"
#include "pj_scene2d_widgets/media_viewer_widget.h"
#include "pj_widgets/Colormap.h"
#include "pj_widgets/GraphicsApi.h"

namespace {

// Runs before main(); see pjPreferRealDisplayQpa().
[[maybe_unused]] const bool kRealDisplayPlatform = pjPreferRealDisplayQpa();

// 8-bit textures and float shader math, against a CPU reference.
constexpr int kTolerance = 4;

[[nodiscard]] bool colourNear(QRgb actual, const QColor& expected, int tolerance = kTolerance) {
  return pjColourNear(actual, expected, tolerance);
}

// The viewer's clear colour in every case: what the scissor leaves outside the image.
const QColor kBackdrop(40, 80, 160);

[[nodiscard]] std::string describe(QRgb pixel) {
  return "rgb(" + std::to_string(qRed(pixel)) + ", " + std::to_string(qGreen(pixel)) + ", " +
         std::to_string(qBlue(pixel)) + ")";
}

// Delivers one frame and then nothing, like a paused source.
class OneShotSource final : public PJ::MediaSource {
 public:
  explicit OneShotSource(PJ::MediaFrame frame) : frame_(std::move(frame)) {}
  void setTimestamp(int64_t /*ts_ns*/) override {}
  std::optional<PJ::MediaFrame> takeFrame() override {
    return std::exchange(frame_, std::nullopt);
  }
  /// The next frame takeFrame() delivers.
  void deliver(PJ::MediaFrame frame) {
    frame_ = std::move(frame);
  }

 private:
  std::optional<PJ::MediaFrame> frame_;
};

// `width` x `height` RGBA; `colour_at(column, row)` picks each pixel.
template <typename ColourAt>
[[nodiscard]] PJ::DecodedFrame rgbaImage(int width, int height, ColourAt colour_at) {
  PJ::DecodedFrame image;
  image.width = width;
  image.height = height;
  image.format = PJ::PixelFormat::kRGBA8888;
  image.pixels = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(width * height * 4));
  for (int row = 0; row < height; ++row) {
    for (int column = 0; column < width; ++column) {
      const QColor colour = colour_at(column, row);
      uint8_t* pixel = image.pixels->data() + static_cast<size_t>((row * width + column) * 4);
      pixel[0] = static_cast<uint8_t>(colour.red());
      pixel[1] = static_cast<uint8_t>(colour.green());
      pixel[2] = static_cast<uint8_t>(colour.blue());
      pixel[3] = 255;
    }
  }
  return image;
}

[[nodiscard]] PJ::MediaFrame frameOf(PJ::DecodedFrame image) {
  PJ::MediaFrame frame;
  frame.base = std::move(image);
  return frame;
}

class MediaViewerRenderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    PJ_SKIP_ON_HEADLESS_QPA();
  }

  // Shows `frame` in a viewer of `size`, applies `state` once it is visible and
  // returns a presented frame in device pixels. Null when nothing was presented.
  QImage render(
      PJ::MediaFrame frame, QSize size = QSize(160, 160),
      const std::optional<PJ::MediaViewState>& state = std::nullopt) {
    source_ = std::make_unique<OneShotSource>(std::move(frame));
    window_ = std::make_unique<QWidget>();
    auto* layout = new QVBoxLayout(window_.get());
    layout->setContentsMargins(0, 0, 0, 0);
    viewer_ = new PJ::MediaViewerWidget(window_.get());
    viewer_->setClearColor(kBackdrop);
    layout->addWidget(viewer_);
    QObject::connect(viewer_, &PJ::MediaViewerWidget::renderingWarningChanged, viewer_, [this](const QString& warning) {
      warning_ = warning;
    });
    viewer_->setMediaSource(source_.get());
    window_->resize(size);
    window_->show();
    if (!QTest::qWaitForWindowExposed(window_.get())) {
      return {};
    }
    if (state.has_value()) {
      EXPECT_TRUE(viewer_->setViewState(*state)) << "rejected view state";
    }
    // The frame taken on the first render is uploaded there; the second shows it settled.
    (void)pjPresentAndGrab(viewer_);
    return pjPresentAndGrab(viewer_);
  }

  [[nodiscard]] std::string backend() const {
    return pjRhiBackendName(PJ::preferredGraphicsApi()).toStdString();
  }

  // Declared before the window so the viewer goes first.
  std::unique_ptr<OneShotSource> source_;
  std::unique_ptr<QWidget> window_;
  PJ::MediaViewerWidget* viewer_ = nullptr;
  QString warning_;
};

#define RENDER_OR_SKIP(image, ...)                                                                     \
  const QImage image = render(__VA_ARGS__);                                                            \
  if (image.isNull() || !warning_.isEmpty()) {                                                         \
    PJ_SKIP_WITHOUT_GPU("the viewer drew nothing on " << backend() << ": " << warning_.toStdString()); \
  }

// A 1.5x zoom panned up puts a square image on rows 5%..80% of a 200x400 view
// (clip-space y -0.6..0.9). That is asymmetric, so a scissor computed from the
// wrong origin clips the image's top and opens rows below it, where the image
// pass paints black over the backdrop.
TEST_F(MediaViewerRenderTest, PannedImageDrawsOnItsOwnRows) {
  const PJ::MediaViewState panned{.zoom = 1.5F, .pan_x = 0.0F, .pan_y = 0.2F};
  RENDER_OR_SKIP(
      image, frameOf(rgbaImage(64, 64, [](int, int) { return QColor(Qt::green); })), QSize(200, 400), panned);
  const int column = image.width() / 2;
  const auto row = [&image](double fraction) { return static_cast<int>(fraction * image.height()); };
  EXPECT_TRUE(colourNear(image.pixel(column, row(0.10)), Qt::green))
      << "the image's top rows are clipped: " << describe(image.pixel(column, row(0.10)));
  EXPECT_TRUE(colourNear(image.pixel(column, row(0.50)), Qt::green))
      << "the image's middle is missing: " << describe(image.pixel(column, row(0.50)));
  EXPECT_TRUE(colourNear(image.pixel(column, row(0.02)), kBackdrop))
      << "drawn above the image: " << describe(image.pixel(column, row(0.02)));
  EXPECT_TRUE(colourNear(image.pixel(column, row(0.90)), kBackdrop))
      << "drawn below the image: " << describe(image.pixel(column, row(0.90)));
}

// One uniform YUV colour. Y=145, U=170, V=165 keeps every channel inside 6%..94%
// for both matrices and both ranges, and their four RGB results are at least
// 15 levels apart, so a wrong matrix or range cannot pass within kTolerance.
constexpr uint8_t kLuma = 145;
constexpr uint8_t kCb = 170;
constexpr uint8_t kCr = 165;

struct YuvCase {
  PJ::PixelFormat format;
  PJ::YuvColorSpace space;
  PJ::YuvColorRange range;
  const char* name;
};

// CMake's gtest_discover_tests names each ctest case after the printed
// parameter, not the name generator's suffix; gtest finds this through
// argument-dependent lookup.
[[maybe_unused]] void PrintTo(const YuvCase& yuv, std::ostream* out) {
  *out << yuv.name;
}

[[nodiscard]] PJ::DecodedFrame uniformYuv(const YuvCase& yuv) {
  constexpr int kSize = 32;
  constexpr auto kLumaSamples = static_cast<size_t>(kSize * kSize);
  constexpr auto kChromaSamples = static_cast<size_t>((kSize / 2) * (kSize / 2));
  auto bytes = std::make_shared<std::vector<uint8_t>>(kLumaSamples, kLuma);
  if (yuv.format == PJ::PixelFormat::kYUV420P) {
    bytes->insert(bytes->end(), kChromaSamples, kCb);
    bytes->insert(bytes->end(), kChromaSamples, kCr);
  } else {
    for (size_t sample = 0; sample < kChromaSamples; ++sample) {
      bytes->push_back(kCb);
      bytes->push_back(kCr);
    }
  }
  PJ::DecodedFrame frame;
  frame.width = kSize;
  frame.height = kSize;
  frame.format = yuv.format;
  frame.pixels = std::move(bytes);
  frame.color_space = yuv.space;
  frame.color_range = yuv.range;
  return frame;
}

// The CPU matrix the shader receives, applied to the same samples.
[[nodiscard]] QColor expectedRgb(const YuvCase& yuv, uint8_t cb_sample = kCb, uint8_t cr_sample = kCr) {
  const std::array<float, 16> matrix = PJ::buildYuvMatrix(yuv.space, yuv.range);
  const float luma = kLuma / 255.0F;
  const float cb = cb_sample / 255.0F - 0.5F;
  const float cr = cr_sample / 255.0F - 0.5F;
  // Column-major: element (row, column) is matrix[column * 4 + row].
  const auto channel = [&](std::size_t row) {
    return std::clamp(matrix[row] * luma + matrix[4 + row] * cb + matrix[8 + row] * cr + matrix[12 + row], 0.0F, 1.0F);
  };
  return QColor::fromRgbF(channel(0), channel(1), channel(2));
}

class MediaViewerYuvTest : public MediaViewerRenderTest, public ::testing::WithParamInterface<YuvCase> {};

TEST_P(MediaViewerYuvTest, MatchesTheCpuMatrix) {
  RENDER_OR_SKIP(image, frameOf(uniformYuv(GetParam())));
  const QRgb centre = image.pixel(image.width() / 2, image.height() / 2);
  const QColor expected = expectedRgb(GetParam());
  EXPECT_TRUE(colourNear(centre, expected))
      << "got " << describe(centre) << ", expected " << describe(expected.rgb()) << " on " << backend();
}

INSTANTIATE_TEST_SUITE_P(
    Formats, MediaViewerYuvTest,
    ::testing::Values(
        YuvCase{
            PJ::PixelFormat::kYUV420P, PJ::YuvColorSpace::kBt601, PJ::YuvColorRange::kLimited, "Yuv420pBt601Limited"},
        YuvCase{PJ::PixelFormat::kYUV420P, PJ::YuvColorSpace::kBt601, PJ::YuvColorRange::kFull, "Yuv420pBt601Full"},
        YuvCase{
            PJ::PixelFormat::kYUV420P, PJ::YuvColorSpace::kBt709, PJ::YuvColorRange::kLimited, "Yuv420pBt709Limited"},
        YuvCase{PJ::PixelFormat::kYUV420P, PJ::YuvColorSpace::kBt709, PJ::YuvColorRange::kFull, "Yuv420pBt709Full"},
        YuvCase{PJ::PixelFormat::kNV12, PJ::YuvColorSpace::kBt601, PJ::YuvColorRange::kLimited, "Nv12Bt601Limited"},
        YuvCase{PJ::PixelFormat::kNV12, PJ::YuvColorSpace::kBt601, PJ::YuvColorRange::kFull, "Nv12Bt601Full"},
        YuvCase{PJ::PixelFormat::kNV12, PJ::YuvColorSpace::kBt709, PJ::YuvColorRange::kLimited, "Nv12Bt709Limited"},
        YuvCase{PJ::PixelFormat::kNV12, PJ::YuvColorSpace::kBt709, PJ::YuvColorRange::kFull, "Nv12Bt709Full"}),
    [](const ::testing::TestParamInfo<YuvCase>& case_info) { return std::string(case_info.param.name); });

// An odd-sized 4:2:0 frame (AV1 allows one; H.264 and HEVC crop to even). Luma
// pixel (column, row) must take chroma sample (column / 2, row / 2), not the
// (n + 1) / 2-sample chroma plane stretched over the frame. Cb steps per chroma
// column and Cr per chroma row, 8 levels each, so a neighbouring sample is at
// least 12 RGB levels off. Nearest filtering makes every luma pixel one flat block.
constexpr int kOddSize = 17;
constexpr int kOddChroma = (kOddSize + 1) / 2;

[[nodiscard]] uint8_t oddCb(int chroma_column) {
  return static_cast<uint8_t>(96 + 8 * chroma_column);
}

[[nodiscard]] uint8_t oddCr(int chroma_row) {
  return static_cast<uint8_t>(96 + 8 * chroma_row);
}

[[nodiscard]] PJ::DecodedFrame oddSizedYuv(const YuvCase& yuv) {
  auto bytes = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(kOddSize * kOddSize), kLuma);
  if (yuv.format == PJ::PixelFormat::kYUV420P) {
    for (int row = 0; row < kOddChroma; ++row) {
      for (int column = 0; column < kOddChroma; ++column) {
        bytes->push_back(oddCb(column));
      }
    }
    for (int row = 0; row < kOddChroma; ++row) {
      bytes->insert(bytes->end(), kOddChroma, oddCr(row));
    }
  } else {
    for (int row = 0; row < kOddChroma; ++row) {
      for (int column = 0; column < kOddChroma; ++column) {
        bytes->push_back(oddCb(column));
        bytes->push_back(oddCr(row));
      }
    }
  }
  PJ::DecodedFrame frame;
  frame.width = kOddSize;
  frame.height = kOddSize;
  frame.format = yuv.format;
  frame.pixels = std::move(bytes);
  frame.color_space = yuv.space;
  frame.color_range = yuv.range;
  frame.mag_filter = PJ::MagFilter::kNearest;
  return frame;
}

class MediaViewerOddSizeTest : public MediaViewerRenderTest, public ::testing::WithParamInterface<YuvCase> {};

TEST_P(MediaViewerOddSizeTest, ChromaSampleIsHalfTheLumaIndex) {
  RENDER_OR_SKIP(image, frameOf(oddSizedYuv(GetParam())));
  int mismatches = 0;
  std::string first_mismatches;
  for (int row = 0; row < kOddSize; ++row) {
    for (int column = 0; column < kOddSize; ++column) {
      const int x = (2 * column + 1) * image.width() / (2 * kOddSize);
      const int y = (2 * row + 1) * image.height() / (2 * kOddSize);
      const QRgb pixel = image.pixel(x, y);
      const QColor expected = expectedRgb(GetParam(), oddCb(column / 2), oddCr(row / 2));
      if (!colourNear(pixel, expected) && ++mismatches <= 6) {
        first_mismatches += " (" + std::to_string(column) + "," + std::to_string(row) + "): got " + describe(pixel) +
                            " expected " + describe(expected.rgb()) + ";";
      }
    }
  }
  EXPECT_EQ(mismatches, 0) << "luma pixels (column,row) off their chroma sample on " << backend()
                           << ", first:" << first_mismatches;
}

INSTANTIATE_TEST_SUITE_P(
    Layouts, MediaViewerOddSizeTest,
    ::testing::Values(
        YuvCase{PJ::PixelFormat::kYUV420P, PJ::YuvColorSpace::kBt601, PJ::YuvColorRange::kLimited, "Yuv420p"},
        YuvCase{PJ::PixelFormat::kNV12, PJ::YuvColorSpace::kBt601, PJ::YuvColorRange::kLimited, "Nv12"}),
    [](const ::testing::TestParamInfo<YuvCase>& case_info) { return std::string(case_info.param.name); });

// BGRA bytes are uploaded as RGBA and swizzled in the shader.
TEST_F(MediaViewerRenderTest, BgraIsSwizzled) {
  constexpr int kSize = 16;
  PJ::DecodedFrame image;
  image.width = kSize;
  image.height = kSize;
  image.format = PJ::PixelFormat::kBGRA8888;
  image.pixels = std::make_shared<std::vector<uint8_t>>();
  for (int pixel = 0; pixel < kSize * kSize; ++pixel) {
    image.pixels->insert(image.pixels->end(), {255, 0, 0, 255});  // B, G, R, A
  }
  RENDER_OR_SKIP(grab, frameOf(std::move(image)));
  const QRgb centre = grab.pixel(grab.width() / 2, grab.height() / 2);
  EXPECT_TRUE(colourNear(centre, Qt::blue)) << describe(centre);
}

// A frame with no pixel content (a derived image's tombstone) clears the image: the backdrop shows again.
TEST_F(MediaViewerRenderTest, AFrameWithNoPixelsClearsTheImage) {
  RENDER_OR_SKIP(shown, frameOf(rgbaImage(16, 16, [](int, int) { return QColor(Qt::green); })));
  const QRgb before = shown.pixel(shown.width() / 2, shown.height() / 2);
  ASSERT_TRUE(colourNear(before, Qt::green)) << describe(before);
  source_->deliver(PJ::MediaFrame{});
  viewer_->update();
  const QImage cleared = pjPresentAndGrab(viewer_);
  ASSERT_FALSE(cleared.isNull());
  const QRgb after = cleared.pixel(cleared.width() / 2, cleared.height() / 2);
  EXPECT_TRUE(colourNear(after, kBackdrop)) << describe(after);
}

// Metric depth through the R32F texture and the colormap LUT, against the CPU colormap.
TEST_F(MediaViewerRenderTest, DepthMatchesTheCpuColormap) {
  constexpr int kSize = 32;
  constexpr float kDepth = 2.0F;
  constexpr float kNear = 0.5F;
  constexpr float kFar = 4.0F;
  PJ::DecodedFrame image;
  image.width = kSize;
  image.height = kSize;
  image.format = PJ::PixelFormat::kDepthR32F;
  const std::vector<float> depth(static_cast<size_t>(kSize * kSize), kDepth);
  image.pixels = std::make_shared<std::vector<uint8_t>>(
      reinterpret_cast<const uint8_t*>(depth.data()), reinterpret_cast<const uint8_t*>(depth.data() + depth.size()));
  image.depth = PJ::DepthColorParams{
      .near_m = kNear, .far_m = kFar, .colormap = static_cast<uint8_t>(PJ::Colormap::kTurbo), .active = true};
  RENDER_OR_SKIP(grab, frameOf(std::move(image)));

  const PJ::ColormapRgb reference = PJ::colorFor(PJ::Colormap::kTurbo, (kDepth - kNear) / (kFar - kNear));
  const QColor expected = QColor::fromRgbF(reference.r, reference.g, reference.b);
  const QRgb centre = grab.pixel(grab.width() / 2, grab.height() / 2);
  // The 256-entry LUT quantizes t; allow two steps of the steepest turbo channel.
  EXPECT_TRUE(colourNear(centre, expected, 8))
      << "got " << describe(centre) << ", expected " << describe(expected.rgb());
}

// A rectification map that mirrors the image left to right, through the RG32UI LUT.
TEST_F(MediaViewerRenderTest, RectificationMapIsApplied) {
  constexpr int kSize = 64;
  PJ::DecodedFrame image =
      rgbaImage(kSize, kSize, [](int column, int) { return QColor(column < kSize / 2 ? Qt::red : Qt::blue); });
  auto mirror = std::make_shared<PJ::UndistortMap>();
  mirror->out_width = kSize;
  mirror->out_height = kSize;
  mirror->src_width = kSize;
  mirror->src_height = kSize;
  for (int row = 0; row < kSize; ++row) {
    for (int column = 0; column < kSize; ++column) {
      mirror->src_x.push_back(static_cast<float>(kSize - 1 - column));
      mirror->src_y.push_back(static_cast<float>(row));
    }
  }
  image.rectify_map = std::move(mirror);
  RENDER_OR_SKIP(grab, frameOf(std::move(image)));
  const int row = grab.height() / 2;
  EXPECT_TRUE(colourNear(grab.pixel(grab.width() / 4, row), Qt::blue)) << describe(grab.pixel(grab.width() / 4, row));
  EXPECT_TRUE(colourNear(grab.pixel(grab.width() * 3 / 4, row), Qt::red))
      << describe(grab.pixel(grab.width() * 3 / 4, row));
}

// A black 64x64 image carrying `annotation`.
[[nodiscard]] PJ::MediaFrame annotatedBlack(PJ::ImageAnnotation annotation) {
  PJ::MediaFrame frame = frameOf(rgbaImage(64, 64, [](int, int) { return QColor(Qt::black); }));
  frame.overlays.push_back(PJ::SceneFrame{.timestamp = 0, .annotations = {std::move(annotation)}});
  return frame;
}

// The outline pipeline (a line strip) and the point quads, each where the image puts it.
TEST_F(MediaViewerRenderTest, LinesAndPointsDrawWhereTheImagePutsThem) {
  PJ::ImageAnnotation annotation;
  PJ::PointsAnnotation line;
  line.topology = PJ::AnnotationTopology::kLineStrip;
  line.points = {{4.0, 16.0}, {60.0, 16.0}};
  line.thickness = 4.0;
  line.color = {255, 255, 0, 255};
  annotation.points.push_back(line);
  PJ::PointsAnnotation point;
  point.topology = PJ::AnnotationTopology::kPoints;
  point.points = {{48.0, 48.0}};
  point.thickness = 8.0;
  point.color = {255, 0, 255, 255};
  annotation.points.push_back(point);
  RENDER_OR_SKIP(grab, annotatedBlack(std::move(annotation)));

  const auto at = [&grab](double x_fraction, double y_fraction) {
    return grab.pixel(static_cast<int>(x_fraction * grab.width()), static_cast<int>(y_fraction * grab.height()));
  };
  EXPECT_TRUE(colourNear(at(0.5, 0.25), Qt::yellow)) << "line: " << describe(at(0.5, 0.25));
  EXPECT_TRUE(colourNear(at(0.75, 0.75), Qt::magenta)) << "point: " << describe(at(0.75, 0.75));
  EXPECT_TRUE(colourNear(at(0.25, 0.75), Qt::black)) << "empty area: " << describe(at(0.25, 0.75));
}

// Glyphs rasterized through QPainter into an R8 texture, drawn in the label's colour.
TEST_F(MediaViewerRenderTest, TextLabelDrawsInItsColour) {
  PJ::ImageAnnotation annotation;
  PJ::TextAnnotation label;
  label.position = {8.0, 40.0};
  label.font_size = 16.0;
  label.color = {255, 255, 0, 255};
  label.text = "MMMM";
  annotation.texts.push_back(label);
  RENDER_OR_SKIP(grab, annotatedBlack(std::move(annotation)));

  int yellow = 0;
  for (int row = 0; row < grab.height(); ++row) {
    for (int column = 0; column < grab.width(); ++column) {
      const QRgb pixel = grab.pixel(column, row);
      if (qRed(pixel) > 160 && qGreen(pixel) > 160 && qBlue(pixel) < 96) {
        ++yellow;
      }
    }
  }
  EXPECT_GT(yellow, 20) << "no glyph pixels in the label colour on " << backend();
}

}  // namespace
