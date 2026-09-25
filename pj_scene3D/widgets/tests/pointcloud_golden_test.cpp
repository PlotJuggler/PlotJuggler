// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Point-cloud rendering regression corpus: small deterministic scenes rendered
// through the real Scene3DDockWidget and compared against golden PNGs captured
// from the OpenGL renderer (fixtures/golden/pointcloud/, see its README).
//
// The same source builds against both renderers (PJ_SCENE3D_RHI selects the
// QRhi one) and touches only the API both docks share: canonical PointCloud
// blobs in the ObjectStore, addTopic, the layer's <pointcloud> XML payload,
// onTrackerTime, sceneView()->camera() and grabPresentedFrame(). In the GL build
// it is a regression test; in the QRhi build it holds that renderer to the same
// goldens on each backend (PJ_GRAPHICS_API=opengl|vulkan|d3d11|metal).
//
// PJ_UPDATE_POINTCLOUD_GOLDENS=1 (GL build only) rewrites the goldens instead of
// comparing. Failing cases write <case>_actual.png / <case>_diff.png into
// PJ_POINTCLOUD_GOLDEN_OUT (default: the working directory);
// PJ_POINTCLOUD_GOLDEN_DUMP_ALL=1 writes them for passing cases too, for
// pixel-exact before/after comparisons of a renderer refactor.
//
// Needs a real windowing platform (xcb under xvfb in CI) with GL >= 4.5 for the
// GL renderer; skips through PJ_SKIP_WITHOUT_GPU otherwise.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QDomDocument>
#include <QImage>
#include <QPalette>
#include <QString>
#include <QSurfaceFormat>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <glm/glm.hpp>
#include <map>
#include <ostream>
#include <string>
#include <vector>

#include "pj_base/sdk/platform.hpp"
#include "pj_scene3d_core/camera/camera.h"
#include "scene3d_render_test_support.h"
#ifndef PJ_SCENE3D_RHI
#include "gl_scene_test_support.h"  // haveGl, liveGlVersion
#endif

using namespace Qt::StringLiterals;

namespace {

using DT = PJ::sdk::PointField::Datatype;
using pj::scene3d::test::ExtraField;
using pj::scene3d::test::maxChannelDelta;

// Before QApplication exists (the shared gui main creates it): goldens are
// pixel-exact at DPR 1, and the GL view expects a 4.5 core context.
[[maybe_unused]] const bool kGoldenPlatformSetup = [] {
  qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");
#ifndef PJ_SCENE3D_RHI
  QSurfaceFormat fmt;
  fmt.setVersion(4, 5);
  fmt.setProfile(QSurfaceFormat::CoreProfile);
  fmt.setDepthBufferSize(24);
  QSurfaceFormat::setDefaultFormat(fmt);
#endif
  return true;
}();

// Light theme: the backdrop and anything theme-derived follow the application
// palette's Window lightness.
struct LightPalette : ::testing::Environment {
  void SetUp() override {
    QPalette pal = QApplication::palette();
    pal.setColor(QPalette::Window, QColor(255, 255, 255));
    QApplication::setPalette(pal);
  }
};
[[maybe_unused]] const auto* const kLightPalette = ::testing::AddGlobalTestEnvironment(new LightPalette);

// Both renderers draw the 80x80 corner axis gizmo (bottom-right); it is part
// of every comparison.
constexpr int kViewSize = 256;

// ---- Cloud construction --------------------------------------------------

struct CloudSpec {
  std::vector<glm::vec3> positions;
  std::vector<ExtraField> fields;
};

// nx*ny lattice centred on the origin, `spacing` apart, height z(u, v) with
// u, v in [0, 1].
std::vector<glm::vec3> lattice(int nx, int ny, float spacing, const std::function<float(float, float)>& z) {
  std::vector<glm::vec3> out;
  out.reserve(static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny));
  for (int j = 0; j < ny; ++j) {
    for (int i = 0; i < nx; ++i) {
      const float u = nx > 1 ? static_cast<float>(i) / static_cast<float>(nx - 1) : 0.0f;
      const float v = ny > 1 ? static_cast<float>(j) / static_cast<float>(ny - 1) : 0.0f;
      out.emplace_back(
          (static_cast<float>(i) - 0.5f * static_cast<float>(nx - 1)) * spacing,
          (static_cast<float>(j) - 0.5f * static_cast<float>(ny - 1)) * spacing, z(u, v));
    }
  }
  return out;
}

float flat(float /*u*/, float /*v*/) {
  return 0.0f;
}

float wave(float u, float v) {
  return 0.15f * std::sin(6.0f * u) * std::cos(4.0f * v);
}

// A field whose value follows the lattice x coordinate: point i of an nx-wide
// lattice gets lo + (hi - lo) * column / (nx - 1).
template <typename T>
ExtraField rampField(std::string name, DT datatype, int nx, double lo, double hi) {
  return {std::move(name), datatype, [nx, lo, hi](std::size_t i, uint8_t* dst) {
            const double t = static_cast<double>(i % static_cast<std::size_t>(nx)) / static_cast<double>(nx - 1);
            const T value = static_cast<T>(std::lround(lo + (hi - lo) * t));
            std::memcpy(dst, &value, sizeof(T));
          }};
}

ExtraField floatRamp(std::string name, int nx) {
  return {std::move(name), DT::kFloat32, [nx](std::size_t i, uint8_t* dst) {
            const float value = static_cast<float>(i % static_cast<std::size_t>(nx)) / static_cast<float>(nx - 1);
            std::memcpy(dst, &value, sizeof(float));
          }};
}

// Per-point colour bytes derived from the lattice position (R from column, G
// from row, B fixed).
std::array<uint8_t, 4> latticeColor(std::size_t i, int nx, int ny) {
  const auto col = static_cast<double>(i % static_cast<std::size_t>(nx)) / (nx - 1);
  const auto row = static_cast<double>(i / static_cast<std::size_t>(nx)) / (ny - 1);
  return {static_cast<uint8_t>(std::lround(255 * col)), static_cast<uint8_t>(std::lround(255 * row)), 96, 255};
}

ExtraField packedColor(std::string name, int nx, int ny) {
  return {std::move(name), DT::kUint32, [nx, ny](std::size_t i, uint8_t* dst) {
            const auto rgba = latticeColor(i, nx, ny);
            std::memcpy(dst, rgba.data(), 4);
          }};
}

ExtraField channel(std::string name, int index, int nx, int ny) {
  return {std::move(name), DT::kUint8, [index, nx, ny](std::size_t i, uint8_t* dst) {
            *dst = latticeColor(i, nx, ny)[static_cast<std::size_t>(index)];
          }};
}

// ---- Corpus --------------------------------------------------------------

using Attrs = std::map<QString, QString>;

struct LayerSpec {
  std::function<CloudSpec()> cloud;
  Attrs attrs;  // <pointcloud> attributes; unset ones keep the layer default
};

struct Scene {
  std::string name;  // described in the fixtures README
  std::vector<LayerSpec> layers;
  pj::scene3d::CameraState camera;
  // Which comparisons apply. The weaker ones are for scenes whose pixels depend
  // on the driver's rasterization rules, not on our rendering.
  enum class Check {
    kFull,            // per-pixel, coverage and mean colour
    kCoverageColour,  // coverage and mean colour
    kCoverage,        // coverage only
  };
  Check check = Check::kFull;
};

pj::scene3d::CameraState defaultCamera() {
  pj::scene3d::CameraState cam;
  cam.focal = {0.0f, 0.0f, 0.0f};
  cam.radius = 4.0f;
  cam.azimuth = 0.6f;
  cam.elevation = 0.6f;
  cam.fov_y = glm::radians(45.0f);
  cam.perspective = true;
  return cam;
}

Attrs solid(const QString& shape, const QString& color) {
  return {
      {u"shape"_s, shape},
      {u"color_type"_s, u"solid"_s},
      {u"color_choice_explicit"_s, u"true"_s},
      {u"solid_color"_s, color}};
}

Attrs field(const QString& shape, const QString& field_name, const QString& colormap) {
  return {
      {u"shape"_s, shape},
      {u"color_type"_s, u"field"_s},
      {u"color_choice_explicit"_s, u"true"_s},
      {u"color_field"_s, field_name},
      {u"colormap"_s, colormap},
      {u"auto_range"_s, u"true"_s}};
}

Attrs with(Attrs base, const Attrs& extra) {
  for (const auto& [key, value] : extra) {
    base[key] = value;
  }
  return base;
}

constexpr int kFieldGrid = 24;
constexpr float kFieldSpacing = 0.08f;

std::function<CloudSpec()> fieldCloud(ExtraField extra) {
  return [extra]() { return CloudSpec{lattice(kFieldGrid, kFieldGrid, kFieldSpacing, flat), {extra}}; };
}

std::vector<Scene> corpus() {
  std::vector<Scene> scenes;
  const auto cam = defaultCamera();
  const auto big_lattice = [] { return CloudSpec{lattice(8, 8, 0.25f, wave), {}}; };
  const auto dense_lattice = [] { return CloudSpec{lattice(120, 120, 0.016f, wave), {}}; };

  // Shapes, large and sub-pixel.
  scenes.push_back(
      {"shape_points_large",
       {{big_lattice, with(solid(u"point"_s, u"#d02020"_s), {{u"size_pixels"_s, u"12"_s}})}},
       cam});
  scenes.push_back(
      {"shape_spheres_large",
       {{big_lattice, with(solid(u"sphere"_s, u"#2060d0"_s), {{u"size_meters"_s, u"0.16"_s}})}},
       cam});
  scenes.push_back(
      {"shape_cubes_large",
       {{big_lattice, with(solid(u"cube"_s, u"#20a040"_s), {{u"size_meters"_s, u"0.16"_s}})}},
       cam});
  scenes.push_back(
      {"shape_points_1px",
       {{dense_lattice, with(solid(u"point"_s, u"#202020"_s), {{u"size_pixels"_s, u"1"_s}})}},
       cam});
  scenes.push_back(
      {"shape_spheres_small",
       {{dense_lattice, with(solid(u"sphere"_s, u"#2060d0"_s), {{u"size_meters"_s, u"0.02"_s}})}},
       cam,
       // ponytail: the ~1.5 px flat discs rasterize at a different size per
       // driver (Mesa 23 draws them visibly smaller than Mesa 26, shifting the
       // mean colour by ~77), so only coverage is stable across CI and local runs.
       Scene::Check::kCoverage});
  scenes.push_back(
      {"shape_cubes_subpixel",
       {{dense_lattice, with(solid(u"cube"_s, u"#20a040"_s), {{u"size_meters"_s, u"0.003"_s}})}},
       cam});

  // Direct colour.
  constexpr int kColorGrid = 16;
  const auto color_lattice = lattice(kColorGrid, kColorGrid, 0.12f, flat);
  const Attrs rgb_points = {
      {u"shape"_s, u"point"_s},
      {u"size_pixels"_s, u"10"_s},
      {u"color_type"_s, u"rgb"_s},
      {u"color_choice_explicit"_s, u"true"_s}};
  scenes.push_back(
      {"color_rgb_packed",
       {{[=] { return CloudSpec{color_lattice, {packedColor("rgb", kColorGrid, kColorGrid)}}; }, rgb_points}},
       cam});
  scenes.push_back(
      {"color_rgba_packed",
       {{[=] { return CloudSpec{color_lattice, {packedColor("rgba", kColorGrid, kColorGrid)}}; }, rgb_points}},
       cam});
  scenes.push_back(
      {"color_rgb_channels",
       {{[=] {
           return CloudSpec{
               color_lattice,
               {channel("red", 0, kColorGrid, kColorGrid), channel("green", 1, kColorGrid, kColorGrid),
                channel("blue", 2, kColorGrid, kColorGrid)}};
         },
         rgb_points}},
       cam});
  scenes.push_back(
      {"color_rgb_cubes",
       {{[=] { return CloudSpec{color_lattice, {packedColor("rgb", kColorGrid, kColorGrid)}}; },
         with(rgb_points, {{u"shape"_s, u"cube"_s}, {u"size_meters"_s, u"0.1"_s}})}},
       cam});

  // Scalar field through each colormap.
  for (const QString& colormap : {u"turbo"_s, u"viridis"_s, u"plasma"_s, u"grayscale"_s}) {
    scenes.push_back(
        {"field_" + colormap.toStdString(),
         {{fieldCloud(floatRamp("intensity", kFieldGrid)),
           with(field(u"point"_s, u"intensity"_s, colormap), {{u"size_pixels"_s, u"8"_s}})}},
         cam});
  }
  scenes.push_back(
      {"field_turbo_spheres",
       {{fieldCloud(floatRamp("intensity", kFieldGrid)),
         with(field(u"sphere"_s, u"intensity"_s, u"turbo"_s), {{u"size_meters"_s, u"0.07"_s}})}},
       cam,
       // Sphere impostor shading follows the sprite's rounded point size, which
       // differs between Mesa releases (CI's 23.x vs 26.x: ~10% of pixels).
       Scene::Check::kCoverageColour});
  scenes.push_back(
      {"field_viridis_cubes",
       {{fieldCloud(floatRamp("intensity", kFieldGrid)),
         with(field(u"cube"_s, u"intensity"_s, u"viridis"_s), {{u"size_meters"_s, u"0.07"_s}})}},
       cam});
  scenes.push_back(
      {"field_axis_z",
       {{big_lattice, with(field(u"sphere"_s, u"z"_s, u"turbo"_s), {{u"size_meters"_s, u"0.16"_s}})}},
       cam});
  const Attrs manual_range = {
      {u"auto_range"_s, u"false"_s},
      {u"range_min"_s, u"0.3"_s},
      {u"range_max"_s, u"0.7"_s},
      {u"size_pixels"_s, u"8"_s}};
  scenes.push_back(
      {"field_outside_range_translucent",
       {{fieldCloud(floatRamp("intensity", kFieldGrid)),
         with(
             with(field(u"point"_s, u"intensity"_s, u"turbo"_s), manual_range),
             {{u"outside_range_opacity"_s, u"0.25"_s}})}},
       cam});
  scenes.push_back(
      {"field_outside_range_hidden",
       {{fieldCloud(floatRamp("intensity", kFieldGrid)),
         with(
             with(field(u"point"_s, u"intensity"_s, u"turbo"_s), manual_range),
             {{u"outside_range_visible"_s, u"false"_s}})}},
       cam});
  scenes.push_back(
      {"field_outside_range_cubes",
       {{fieldCloud(floatRamp("intensity", kFieldGrid)),
         with(
             with(field(u"cube"_s, u"intensity"_s, u"turbo"_s), manual_range),
             {{u"outside_range_opacity"_s, u"0.25"_s}, {u"size_meters"_s, u"0.07"_s}})}},
       cam});

  // Non-float scalar fields (zero-copy integer attributes), auto range, turbo.
  const auto int_scene = [&](const std::string& type, ExtraField extra) {
    scenes.push_back(
        {"field_" + type,
         {{fieldCloud(std::move(extra)),
           with(field(u"point"_s, u"intensity"_s, u"turbo"_s), {{u"size_pixels"_s, u"8"_s}})}},
         cam});
  };
  int_scene("int8", rampField<int8_t>("intensity", DT::kInt8, kFieldGrid, -120, 120));
  int_scene("uint8", rampField<uint8_t>("intensity", DT::kUint8, kFieldGrid, 0, 250));
  int_scene("int16", rampField<int16_t>("intensity", DT::kInt16, kFieldGrid, -30000, 30000));
  int_scene("uint16", rampField<uint16_t>("intensity", DT::kUint16, kFieldGrid, 0, 60000));
  int_scene("int32", rampField<int32_t>("intensity", DT::kInt32, kFieldGrid, -2000000, 2000000));
  int_scene("uint32", rampField<uint32_t>("intensity", DT::kUint32, kFieldGrid, 0, 4000000));

  // Depth: two clouds crossing each other.
  scenes.push_back(
      {"depth_two_clouds",
       {{[] { return CloudSpec{lattice(12, 12, 0.16f, flat), {}}; },
         with(solid(u"cube"_s, u"#d02020"_s), {{u"size_meters"_s, u"0.14"_s}})},
        {[] { return CloudSpec{lattice(12, 12, 0.16f, [](float u, float /*v*/) { return (u - 0.5f) * 1.2f; }), {}}; },
         with(solid(u"sphere"_s, u"#2040e0"_s), {{u"size_meters"_s, u"0.12"_s}})}},
       cam});

  // Large cloud: coverage only.
  scenes.push_back(
      {"large_1m_points",
       {{[] { return CloudSpec{lattice(1000, 1000, 0.002f, wave), {}}; },
         with(field(u"point"_s, u"z"_s, u"turbo"_s), {{u"size_pixels"_s, u"2"_s}})}},
       cam,
       Scene::Check::kCoverageColour});
  return scenes;
}

// ---- Comparison ----------------------------------------------------------

// Calibrated llvmpipe (goldens) vs an NVIDIA GPU: every differing pixel sits on
// a silhouette (different MSAA sample patterns and sub-pixel point rounding),
// never inside a surface. Worst case: 1.8% of pixels off by > 16 (2 cm spheres),
// coverage within 1.3%. The bounds leave ~1.7x headroom; a colormap, shading or
// size regression moves far more pixels than that.
//
// The GL build (the goldens' own renderer) checks plain per-pixel differences.
// The QRhi build counts a pixel as differing only when no golden pixel in its
// 3x3 neighbourhood is within `channel` of it: edge and sub-pixel coverage that
// moved by less than a pixel is rasterization, not rendering. It matters for the
// QRhi backends whose framebuffer is y-down (Vulkan on lavapipe, D3D11, Metal)
// where llvmpipe's GL target is y-up: the same 4x MSAA pattern lands mirrored on
// the scene, so diagonal silhouettes, cube edge bands and 1 px points resolve
// differently (up to 8% of pixels per-pixel on lavapipe, at most 2% with this
// rule), while every surface interior is identical.
#ifdef PJ_SCENE3D_RHI
constexpr bool kNeighbourhoodRule = true;
#else
constexpr bool kNeighbourhoodRule = false;
#endif

struct Tolerance {
  int channel = 16;             // per-channel |delta| a pixel may differ by before it counts
  double max_diff_frac = 0.03;  // allowed fraction of differing pixels
  // QRhi build only: ceiling on the plain per-pixel fraction, so the neighbourhood
  // rule cannot hide a gross regression (a one-pixel shift of the whole scene,
  // say): max(3 x the limit above, 0.10). The worst legitimate value measured is
  // 0.08 (Vulkan on lavapipe, `shape_cubes_subpixel`).
  double max_plain_diff_frac = std::max(3.0 * 0.03, 0.10);
  double coverage = 0.05;  // allowed relative change in non-background pixel count
  // Allowed per-channel change in the mean colour of covered pixels. Drivers move
  // it by <= 9 (tiny spheres under NVIDIA MSAA); a swapped colormap, shape or
  // colour mode moves it by 17-229.
  int mean_color = 12;
  // The differing fraction of the foreground (non-background in the golden or
  // the actual image), so a small cloud cannot hide behind the frame's empty
  // pixels: the frame-relative limit above lets a scene whose foreground is 10%
  // of the frame differ on 30% of it. It targets colour inside surfaces, so it
  // always uses the 3x3 neighbourhood rule, in the GL build too: sphere impostor
  // silhouettes move by a pixel between Mesa releases (13-17% of the foreground
  // per pixel on CI's Mesa 23 vs the Mesa 26 goldens, <= 3% with the rule), and
  // silhouettes are what the frame-relative checks above bound. Worst legitimate
  // value 0.086 (`field_viridis_cubes` on Vulkan/lavapipe). A 5% shift of a
  // colormap range fails it (0.21-0.27) while every frame-relative check still passes.
  double max_fg_diff_frac = 0.12;
  // Pixels farther than kBackgroundMargin from the golden's foreground that are
  // not background: stray geometry. An absolute count, not a fraction; 0 on
  // every scene measured.
  int max_background_strays = 4;
};

// Distance (pixels) from the golden's foreground within which the background
// check leaves a pixel alone: silhouettes and MSAA edges move by less.
constexpr int kBackgroundMargin = 2;

struct Stats {
  double diff_frac = 0.0;        // differing pixels (neighbourhood rule in the QRhi build)
  double plain_diff_frac = 0.0;  // pixels off their own golden pixel, for information
  double fg_diff_frac = 0.0;     // neighbourhood-rule differing pixels among the foreground of either image
  int background_strays = 0;     // non-background pixels far from the golden's foreground
  double coverage_ratio = 1.0;   // actual / golden non-background pixel counts
  int mean_color_delta = 0;      // max per-channel |mean(actual) - mean(golden)| over covered pixels
  QImage diff;
};

Stats compare(const QImage& actual, const QImage& golden, const Tolerance& tol) {
  Stats stats;
  stats.diff = QImage(golden.size(), QImage::Format_RGB32);
  // Every scene leaves the top-left corner empty, so it is the backdrop colour.
  const QRgb background = golden.pixel(0, 0);
  const auto covered = [background](QRgb pixel) { return maxChannelDelta(pixel, background) > 12; };
  const auto near_golden_cover = [&](int x, int y) {
    for (int dy = -kBackgroundMargin; dy <= kBackgroundMargin; ++dy) {
      for (int dx = -kBackgroundMargin; dx <= kBackgroundMargin; ++dx) {
        if (golden.valid(x + dx, y + dy) && covered(golden.pixel(x + dx, y + dy))) {
          return true;
        }
      }
    }
    return false;
  };
  long compared = 0;
  long differing = 0;
  long plain_differing = 0;
  long foreground = 0;
  long fg_differing = 0;
  long golden_cover = 0;
  long actual_cover = 0;
  std::array<double, 3> golden_sum{};
  std::array<double, 3> actual_sum{};
  const auto accumulate = [](std::array<double, 3>& sum, QRgb pixel) {
    sum[0] += qRed(pixel);
    sum[1] += qGreen(pixel);
    sum[2] += qBlue(pixel);
  };
  for (int y = 0; y < golden.height(); ++y) {
    for (int x = 0; x < golden.width(); ++x) {
      const QRgb a = actual.pixel(x, y);
      const QRgb g = golden.pixel(x, y);
      ++compared;
      if (covered(g)) {
        ++golden_cover;
        accumulate(golden_sum, g);
      }
      if (covered(a)) {
        ++actual_cover;
        accumulate(actual_sum, a);
        if (!near_golden_cover(x, y)) {
          ++stats.background_strays;
        }
      }
      const bool plain_differs = maxChannelDelta(a, g) > tol.channel;
      bool neighbourhood_differs = plain_differs;
      for (int dy = -1; neighbourhood_differs && dy <= 1; ++dy) {
        for (int dx = -1; neighbourhood_differs && dx <= 1; ++dx) {
          if (golden.valid(x + dx, y + dy)) {
            neighbourhood_differs = maxChannelDelta(a, golden.pixel(x + dx, y + dy)) > tol.channel;
          }
        }
      }
      const bool differs = kNeighbourhoodRule ? neighbourhood_differs : plain_differs;
      plain_differing += plain_differs ? 1 : 0;
      differing += differs ? 1 : 0;
      if (covered(g) || covered(a)) {
        ++foreground;
        fg_differing += neighbourhood_differs ? 1 : 0;
      }
      const int shade = qGray(g) / 3;
      stats.diff.setPixel(x, y, differs ? qRgb(255, 0, 255) : qRgb(shade, shade, shade));
    }
  }
  stats.diff_frac = compared > 0 ? static_cast<double>(differing) / static_cast<double>(compared) : 0.0;
  stats.plain_diff_frac = compared > 0 ? static_cast<double>(plain_differing) / static_cast<double>(compared) : 0.0;
  stats.fg_diff_frac = foreground > 0 ? static_cast<double>(fg_differing) / static_cast<double>(foreground) : 0.0;
  stats.coverage_ratio = golden_cover > 0 ? static_cast<double>(actual_cover) / static_cast<double>(golden_cover)
                                          : (actual_cover == 0 ? 1.0 : 0.0);
  if (golden_cover > 0 && actual_cover > 0) {
    for (std::size_t c = 0; c < 3; ++c) {
      const double delta =
          actual_sum[c] / static_cast<double>(actual_cover) - golden_sum[c] / static_cast<double>(golden_cover);
      stats.mean_color_delta = std::max(stats.mean_color_delta, static_cast<int>(std::lround(std::abs(delta))));
    }
  }
  return stats;
}

QString goldenPath(const std::string& name) {
  return QStringLiteral(PJ_POINTCLOUD_GOLDEN_DIR) + u'/' + QString::fromStdString(name) + u".png"_s;
}

QString outPath(const std::string& name, const char* suffix) {
  const std::string dir = PJ::sdk::getEnv("PJ_POINTCLOUD_GOLDEN_OUT").value_or(std::string{});
  const QString base = dir.empty() ? QDir::currentPath() : QString::fromStdString(dir);
  QDir().mkpath(base);
  return base + u'/' + QString::fromStdString(name) + QString::fromLatin1(suffix);
}

// ---- Rendering -----------------------------------------------------------

// Keeps gtest from hex-dumping the whole Scene in failure messages.
[[maybe_unused]] void PrintTo(const Scene& scene, std::ostream* os) {
  *os << scene.name;
}

class PointCloudGolden : public ::testing::TestWithParam<Scene> {};

TEST_P(PointCloudGolden, MatchesGolden) {
  const Scene& scene = GetParam();
  PJ_SKIP_ON_HEADLESS_QPA();

  pj::scene3d::test::Scene3DRenderFixture fixture;
  PJ::Scene3DDockWidget& dock = *fixture.dock();
  dock.setTransformService(&fixture.transforms());
  // The view is created by a queued call; pin its size before the first show so
  // the window manager never has to resize a mapped window. The dock itself
  // has a larger minimum width (its overlay controls).
  for (int i = 0; i < 20 && dock.sceneView() == nullptr; ++i) {
    QApplication::processEvents();
  }
  auto* view = dock.sceneView();
  ASSERT_NE(view, nullptr);
  view->setFixedSize(kViewSize, kViewSize);
  fixture.window().show();
  QApplication::processEvents();
  ASSERT_EQ(view->size(), QSize(kViewSize, kViewSize));
  ASSERT_EQ(view->devicePixelRatioF(), 1.0) << "goldens are captured at DPR 1";
#ifndef PJ_SCENE3D_RHI
  if (!pj::scene3d::test::haveGl(*view)) {
    PJ_SKIP_WITHOUT_GPU("no usable OpenGL context");
  }
  const auto gl_version = pj::scene3d::test::liveGlVersion(*view);
  if (gl_version < std::pair<int, int>(4, 5)) {
    PJ_SKIP_WITHOUT_GPU("GL " << gl_version.first << "." << gl_version.second << " below 4.5");
  }
#endif
  view->setGridVisible(false);
  view->setAxesVisible(false);
  view->setTfConnectionsVisible(false);

  for (std::size_t i = 0; i < scene.layers.size(); ++i) {
    const CloudSpec cloud = scene.layers[i].cloud();
    const auto topic = fixture.add(
        "/cloud" + std::to_string(i), PJ::sdk::BuiltinObjectType::kPointCloud,
        pj::scene3d::test::serializeCloud(cloud.positions, "map", cloud.fields));
    PJ::ISceneLayer* layer = dock.layerFor(topic);
    ASSERT_NE(layer, nullptr);
    QDomDocument doc;
    QDomElement payload = doc.createElement(u"pointcloud"_s);
    for (const auto& [key, value] : scene.layers[i].attrs) {
      payload.setAttribute(key, value);
    }
    ASSERT_TRUE(layer->xmlLoadState(payload)) << "invalid <pointcloud> attributes";
  }
  dock.setFixedFrame(u"map"_s);
  dock.onTrackerTime(0.0);

  // The QRhi view fits the camera to each new layer on its first frames; let
  // that happen, then pin the camera.
  for (int i = 0; i < 3; ++i) {
    pj::scene3d::test::grab(*view);
  }
  view->camera().adoptState(scene.camera);
  const QImage actual = pj::scene3d::test::grabStable(*view);
  ASSERT_EQ(actual.size(), QSize(kViewSize, kViewSize));

  const QString golden_file = goldenPath(scene.name);
  if (PJ::sdk::getEnv("PJ_UPDATE_POINTCLOUD_GOLDENS") == "1") {
#ifdef PJ_SCENE3D_RHI
    FAIL() << "goldens come from the OpenGL renderer; regenerate them in the default build";
#else
    ASSERT_TRUE(actual.save(golden_file)) << golden_file.toStdString();
    GTEST_SKIP() << "golden written: " << golden_file.toStdString();
#endif
  }

  const QImage golden = QImage(golden_file).convertToFormat(QImage::Format_RGB32);
  ASSERT_FALSE(golden.isNull()) << "missing golden " << golden_file.toStdString()
                                << " (run with PJ_UPDATE_POINTCLOUD_GOLDENS=1 in the GL build)";
  ASSERT_EQ(golden.size(), actual.size());

  const Tolerance tol;
  const Stats stats = compare(actual, golden, tol);
  std::printf(
      "[golden] %-34s diff %.4f (per-pixel %.4f)  coverage %.4f  mean colour %d  fg-diff %.4f (<= %.2f)  strays %d "
      "(<= %d)\n",
      scene.name.c_str(), stats.diff_frac, stats.plain_diff_frac, stats.coverage_ratio, stats.mean_color_delta,
      stats.fg_diff_frac, tol.max_fg_diff_frac, stats.background_strays, tol.max_background_strays);
  const bool check_colour = scene.check != Scene::Check::kCoverage;
  const bool check_pixels = scene.check == Scene::Check::kFull;
  const bool coverage_ok = std::abs(stats.coverage_ratio - 1.0) <= tol.coverage &&
                           (!check_colour || stats.mean_color_delta <= tol.mean_color);
  const bool pixels_ok = !check_pixels || (stats.diff_frac <= tol.max_diff_frac &&
                                           (!kNeighbourhoodRule || stats.plain_diff_frac <= tol.max_plain_diff_frac) &&
                                           stats.fg_diff_frac <= tol.max_fg_diff_frac &&
                                           stats.background_strays <= tol.max_background_strays);
  if (!coverage_ok || !pixels_ok || PJ::sdk::getEnv("PJ_POINTCLOUD_GOLDEN_DUMP_ALL") == "1") {
    actual.save(outPath(scene.name, "_actual.png"));
    stats.diff.save(outPath(scene.name, "_diff.png"));
  }
  EXPECT_LE(std::abs(stats.coverage_ratio - 1.0), tol.coverage) << "non-background pixel count vs golden";
  if (check_colour) {
    EXPECT_LE(stats.mean_color_delta, tol.mean_color) << "mean colour of covered pixels vs golden";
  }
  if (check_pixels) {
    EXPECT_LE(stats.diff_frac, tol.max_diff_frac) << "fraction of pixels off by more than " << tol.channel;
    if constexpr (kNeighbourhoodRule) {
      EXPECT_LE(stats.plain_diff_frac, tol.max_plain_diff_frac)
          << "per-pixel fraction off by more than " << tol.channel << " (no neighbourhood allowance)";
    }
    EXPECT_LE(stats.fg_diff_frac, tol.max_fg_diff_frac)
        << "fraction of the foreground off by more than " << tol.channel;
    EXPECT_LE(stats.background_strays, tol.max_background_strays)
        << "non-background pixels more than " << kBackgroundMargin << " px from the golden's foreground";
  }
}

INSTANTIATE_TEST_SUITE_P(
    Corpus, PointCloudGolden, ::testing::ValuesIn(corpus()),
    [](const ::testing::TestParamInfo<Scene>& param_info) { return param_info.param.name; });

}  // namespace
