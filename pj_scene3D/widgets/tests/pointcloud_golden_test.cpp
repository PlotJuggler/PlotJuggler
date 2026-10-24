// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Rendering regression corpus: small deterministic scenes rendered through the
// real Scene3DDockWidget and compared against golden PNGs (fixtures/golden/pointcloud/,
// see its README): point clouds, plus a scene corpus (`scene_*`) for the ground grid in both themes and styles,
// SceneEntities markers and a URDF robot.
//
// It drives only the dock's public API: canonical PointCloud blobs in the
// ObjectStore, addTopic, the layer's <pointcloud> XML payload, onTrackerTime,
// sceneView()->camera() and grabPresentedFrame() (and, for the scene corpus,
// SceneEntities blobs, local URDF files and the view's grid setters), and holds
// the view to the goldens on each backend (PJ_GRAPHICS_API=opengl|vulkan|d3d11|metal).
//
// PJ_UPDATE_POINTCLOUD_GOLDENS=1 writes the goldens instead of comparing, and only
// from the pinned writer, QRhi OpenGL on Mesa llvmpipe (any other backend or
// driver fails the case), recording its device in writer.txt: that stack must
// then match them exactly, every other one within tolerances. PJ_POINTCLOUD_GOLDEN_DIR overrides the golden directory
// for both, so a regeneration can land in a scratch copy for review. Failing cases
// write <case>_actual.png / <case>_diff.png into
// PJ_POINTCLOUD_GOLDEN_OUT (default: the working directory);
// PJ_POINTCLOUD_GOLDEN_DUMP_ALL=1 writes them for passing cases too, for
// pixel-exact before/after comparisons of a renderer refactor.
//
// Needs a real windowing platform (xcb under xvfb in CI); skips through
// PJ_SKIP_WITHOUT_GPU otherwise.

#include <gtest/gtest.h>
#include <rhi/qrhi.h>

#include <QApplication>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QImage>
#include <QPalette>
#include <QString>
#include <QTemporaryDir>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <glm/glm.hpp>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "pj_base/builtin/scene_entities_codec.hpp"
#include "pj_base/sdk/platform.hpp"
#include "pj_bench_probes.h"
#include "pj_scene3d_core/camera/camera.h"
#include "scene3d_render_test_support.h"

using namespace Qt::StringLiterals;

namespace {

using DT = PJ::sdk::PointField::Datatype;
using pj::scene3d::test::ExtraField;
using pj::scene3d::test::maxChannelDelta;

// Before QApplication exists (the shared gui main creates it): goldens are
// pixel-exact at DPR 1.
[[maybe_unused]] const bool kGoldenPlatformSetup = [] {
  qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");
  return true;
}();

// Light theme: the backdrop and anything theme-derived follow the application
// palette's Window lightness.
void setWindowColour(const QColor& colour) {
  QPalette pal = QApplication::palette();
  pal.setColor(QPalette::Window, colour);
  QApplication::setPalette(pal);
}

struct LightPalette : ::testing::Environment {
  void SetUp() override {
    setWindowColour(QColor(255, 255, 255));
  }
};
[[maybe_unused]] const auto* const kLightPalette = ::testing::AddGlobalTestEnvironment(new LightPalette);

// The view draws the 80x80 corner axis gizmo (bottom-right); it is part
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
    kCoverageColour,  // coverage, mean colour and coarse spatial (tiles, tile colour, centroid, bbox, strays)
    kCoverage,        // coverage and coarse spatial, without colour
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
       // mean colour by ~77), so only coverage and position are stable across CI
       // and local runs.
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

  // Large cloud: a million 2 px points overlapping many to a pixel, so which one
  // wins each pixel is the driver's rasterization order. Coverage, mean colour
  // and the coarse spatial checks.
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
// A pixel counts as differing only when no golden pixel in its 3x3
// neighbourhood is within `channel` of it: edge and sub-pixel coverage that
// moved by less than a pixel is rasterization, not rendering. It matters for the
// backends whose framebuffer is y-down (Vulkan on lavapipe, D3D11, Metal)
// where llvmpipe's GL target is y-up: the same 4x MSAA pattern lands mirrored on
// the scene, so diagonal silhouettes, cube edge bands and 1 px points resolve
// differently (up to 8% of pixels per-pixel on lavapipe, at most 2% with this
// rule), while every surface interior is identical. The plain per-pixel
// fraction is bounded separately (max_plain_diff_frac).

struct Tolerance {
  int channel = 16;             // per-channel |delta| a pixel may differ by before it counts
  double max_diff_frac = 0.03;  // allowed fraction of differing pixels
  // Ceiling on the plain per-pixel fraction, so the neighbourhood
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
  // uses the 3x3 neighbourhood rule: sphere impostor
  // silhouettes move by a pixel between Mesa releases (13-17% of the foreground
  // per pixel on CI's Mesa 23 vs the Mesa 26 goldens, <= 3% with the rule), and
  // silhouettes are what the frame-relative checks above bound. Worst legitimate
  // value 0.086 (`field_viridis_cubes` on Vulkan/lavapipe). A 5% shift of a
  // colormap range fails it (0.21-0.27) while every frame-relative check still passes.
  double max_fg_diff_frac = 0.12;
  // Pixels farther than `background_margin` from the golden's foreground that are
  // not background: stray geometry. An absolute count, not a fraction; 0 on
  // every scene measured.
  int max_background_strays = 4;
  // Distance (pixels) from the golden's foreground within which the background
  // check leaves a pixel alone: silhouettes and MSAA edges move by less. The
  // coverage modes widen it (kCoarseBackgroundMargin).
  int background_margin = 2;
  // The coverage modes' coarse spatial checks, which the per-pixel ones of the
  // full mode subsume. Tile coverage: sum over kTile x kTile tiles of
  // |actual - golden| covered pixels, over the golden's covered pixels. A uniform
  // rasterization change moves it by about |coverage - 1|, so the bound is
  // `coverage` plus this slack; measured <= 0.053 (line grids on lavapipe).
  double tile_coverage_slack = 0.10;
  // Tile colour: the golden-coverage-weighted mean, over tiles both images cover,
  // of the tile's max per-channel |mean(actual) - mean(golden)|. Catches a
  // colormap reversed in place, which keeps coverage and the frame's mean colour.
  // Measured <= 6 on the coverage-colour scenes, 10 on the tiny spheres.
  int max_tile_colour = 20;
  // Distance (pixels) between the covered pixels' centroids. Clouds measure
  // <= 0.4 on every Mesa backend (llvmpipe, lavapipe, Iris GL and Vulkan).
  double max_centroid_shift = 2.0;
  // Largest move (pixels) of an edge of the covered pixels' bounding box; <= 1 measured.
  int max_bbox_shift = 4;
};

// The coverage modes' background margin: sub-pixel sprites and 1 px lines land
// a pixel or two apart per driver, which the default margin counts as strays.
constexpr int kCoarseBackgroundMargin = 4;
// Side (pixels) of the tiles of the coarse spatial checks: 16 x 16 tiles on a 256 px view.
constexpr int kTile = 16;

struct Stats {
  double diff_frac = 0.0;          // differing pixels (neighbourhood rule)
  double plain_diff_frac = 0.0;    // pixels off their own golden pixel
  double fg_diff_frac = 0.0;       // neighbourhood-rule differing pixels among the foreground of either image
  int background_strays = 0;       // non-background pixels far from the golden's foreground
  double coverage_ratio = 1.0;     // actual / golden non-background pixel counts
  int mean_color_delta = 0;        // max per-channel |mean(actual) - mean(golden)| over covered pixels
  double tile_coverage_delta = 0;  // see Tolerance::tile_coverage_slack
  int tile_colour_delta = 0;       // see Tolerance::max_tile_colour
  double centroid_shift = 0.0;     // pixels between the covered pixels' centroids
  int bbox_shift = 0;              // largest move (pixels) of a bounding-box edge of the covered pixels
  QImage diff;
};

Stats compare(const QImage& actual, const QImage& golden, const Tolerance& tol) {
  Stats stats;
  stats.diff = QImage(golden.size(), QImage::Format_RGB32);
  // Every scene leaves the top-left corner empty, so it is the backdrop colour.
  const QRgb background = golden.pixel(0, 0);
  const auto covered = [background](QRgb pixel) { return maxChannelDelta(pixel, background) > 12; };
  const int margin = tol.background_margin;
  const auto near_golden_cover = [&](int x, int y) {
    for (int dy = -margin; dy <= margin; ++dy) {
      for (int dx = -margin; dx <= margin; ++dx) {
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
  // Per image (0 golden, 1 actual): covered pixels and their colour sums per
  // tile, and the covered pixels' coordinate sums and bounding box.
  const int tiles_x = (golden.width() + kTile - 1) / kTile;
  const int tiles_y = (golden.height() + kTile - 1) / kTile;
  const auto tile_count = static_cast<std::size_t>(tiles_x) * static_cast<std::size_t>(tiles_y);
  std::array<std::vector<long>, 2> tile_cover{std::vector<long>(tile_count), std::vector<long>(tile_count)};
  std::array<std::vector<std::array<double, 3>>, 2> tile_sum{
      std::vector<std::array<double, 3>>(tile_count), std::vector<std::array<double, 3>>(tile_count)};
  std::array<std::array<double, 2>, 2> position_sum{};
  std::array<std::array<int, 4>, 2> bbox{};  // min x, min y, max x, max y
  bbox.fill({golden.width(), golden.height(), -1, -1});
  const auto note_cover = [&](int image, int x, int y, QRgb pixel) {
    const auto tile = static_cast<std::size_t>((y / kTile) * tiles_x + x / kTile);
    ++tile_cover[image][tile];
    accumulate(tile_sum[image][tile], pixel);
    position_sum[image][0] += x;
    position_sum[image][1] += y;
    bbox[image] = {
        std::min(bbox[image][0], x), std::min(bbox[image][1], y), std::max(bbox[image][2], x),
        std::max(bbox[image][3], y)};
  };
  for (int y = 0; y < golden.height(); ++y) {
    for (int x = 0; x < golden.width(); ++x) {
      const QRgb a = actual.pixel(x, y);
      const QRgb g = golden.pixel(x, y);
      ++compared;
      if (covered(g)) {
        ++golden_cover;
        accumulate(golden_sum, g);
        note_cover(0, x, y, g);
      }
      if (covered(a)) {
        ++actual_cover;
        accumulate(actual_sum, a);
        note_cover(1, x, y, a);
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
      plain_differing += plain_differs ? 1 : 0;
      differing += neighbourhood_differs ? 1 : 0;
      if (covered(g) || covered(a)) {
        ++foreground;
        fg_differing += neighbourhood_differs ? 1 : 0;
      }
      const int shade = qGray(g) / 3;
      stats.diff.setPixel(x, y, neighbourhood_differs ? qRgb(255, 0, 255) : qRgb(shade, shade, shade));
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
    long tile_moved = 0;
    double colour_weighted = 0.0;
    double colour_weight = 0.0;
    for (std::size_t t = 0; t < tile_count; ++t) {
      const long g = tile_cover[0][t];
      const long a = tile_cover[1][t];
      tile_moved += std::abs(a - g);
      if (g > 0 && a > 0) {
        double worst = 0.0;
        for (std::size_t c = 0; c < 3; ++c) {
          worst = std::max(
              worst, std::abs(tile_sum[1][t][c] / static_cast<double>(a) - tile_sum[0][t][c] / static_cast<double>(g)));
        }
        colour_weighted += worst * static_cast<double>(g);
        colour_weight += static_cast<double>(g);
      }
    }
    stats.tile_coverage_delta = static_cast<double>(tile_moved) / static_cast<double>(golden_cover);
    stats.tile_colour_delta = colour_weight > 0.0 ? static_cast<int>(std::lround(colour_weighted / colour_weight)) : 0;
    stats.centroid_shift = std::hypot(
        position_sum[1][0] / static_cast<double>(actual_cover) - position_sum[0][0] / static_cast<double>(golden_cover),
        position_sum[1][1] / static_cast<double>(actual_cover) -
            position_sum[0][1] / static_cast<double>(golden_cover));
    for (std::size_t edge = 0; edge < 4; ++edge) {
      stats.bbox_shift = std::max(stats.bbox_shift, std::abs(bbox[1][edge] - bbox[0][edge]));
    }
  } else if (golden_cover != actual_cover) {
    // One image empty, the other not: every spatial measure is off the scale.
    stats.tile_coverage_delta = 1.0;
    stats.centroid_shift = golden.width();
    stats.bbox_shift = golden.width();
  }
  return stats;
}

// Which of a check mode's comparisons hold; expectMatchesGolden() reports each,
// the comparator self-test below asserts the verdict.
struct Verdict {
  bool coverage = true;  // non-background pixel count
  bool colour = true;    // mean colour (not in kCoverage)
  bool pixels = true;    // per-pixel, foreground and stray checks (kFull)
  bool spatial = true;   // tile coverage and colour, centroid, bounding box, wide strays (the coverage modes)
  [[nodiscard]] bool ok() const {
    return coverage && colour && pixels && spatial;
  }
};

Verdict judge(const Stats& stats, const Tolerance& tol, Scene::Check check) {
  Verdict verdict;
  verdict.coverage = std::abs(stats.coverage_ratio - 1.0) <= tol.coverage;
  if (check != Scene::Check::kCoverage) {
    verdict.colour = stats.mean_color_delta <= tol.mean_color;
  }
  if (check == Scene::Check::kFull) {
    verdict.pixels = stats.diff_frac <= tol.max_diff_frac && stats.plain_diff_frac <= tol.max_plain_diff_frac &&
                     stats.fg_diff_frac <= tol.max_fg_diff_frac && stats.background_strays <= tol.max_background_strays;
  } else {
    verdict.spatial = stats.tile_coverage_delta <= tol.coverage + tol.tile_coverage_slack &&
                      (check == Scene::Check::kCoverage || stats.tile_colour_delta <= tol.max_tile_colour) &&
                      stats.centroid_shift <= tol.max_centroid_shift && stats.bbox_shift <= tol.max_bbox_shift &&
                      stats.background_strays <= tol.max_background_strays;
  }
  return verdict;
}

// The coverage modes compare strays outside the wider margin.
Tolerance forCheck(Tolerance tol, Scene::Check check) {
  if (check != Scene::Check::kFull) {
    tol.background_margin = std::max(tol.background_margin, kCoarseBackgroundMargin);
  }
  return tol;
}

QString goldenDir() {
  return QString::fromStdString(
      PJ::sdk::getEnv("PJ_POINTCLOUD_GOLDEN_DIR").value_or(std::string{PJ_POINTCLOUD_GOLDEN_DIR}));
}

QString goldenPath(const std::string& name) {
  return goldenDir() + u'/' + QString::fromStdString(name) + u".png"_s;
}

// The file naming the device that wrote the goldens.
QString writerRecordPath() {
  return goldenDir() + u"/writer.txt"_s;
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

// A shown 256x256 view on its own fixture, ready for a scene.
class GoldenView {
 public:
  GoldenView() : dock_(*fixture_.dock()) {
    dock_.setTransformService(&fixture_.transforms());
    // The view is created by a queued call; pin its size before the first show so
    // the window manager never has to resize a mapped window. The dock itself
    // has a larger minimum width (its overlay controls).
    for (int i = 0; i < 20 && dock_.sceneView() == nullptr; ++i) {
      QApplication::processEvents();
    }
    view_ = dock_.sceneView();
  }

  // Shows the view with grid, axes and TF lines off. False (with a failure or a
  // skip recorded) when it cannot render.
  [[nodiscard]] bool show() {
    if (view_ == nullptr) {
      ADD_FAILURE() << "no scene view";
      return false;
    }
    view_->setFixedSize(kViewSize, kViewSize);
    fixture_.window().show();
    QApplication::processEvents();
    EXPECT_EQ(view_->size(), QSize(kViewSize, kViewSize));
    EXPECT_EQ(view_->devicePixelRatioF(), 1.0) << "goldens are captured at DPR 1";
    view_->setGridVisible(false);
    view_->setAxesVisible(false);
    view_->setTfConnectionsVisible(false);
    return true;
  }
  [[nodiscard]] const std::string& skipReason() const {
    return skip_reason_;
  }

  // Warm-up frames land the decodes and uploads; then pin the camera and grab.
  QImage render(const pj::scene3d::CameraState& camera) {
    for (int i = 0; i < 3; ++i) {
      pj::scene3d::test::grab(*view_);
    }
    view_->camera().adoptState(camera);
    return pj::scene3d::test::grabStable(*view_);
  }

  pj::scene3d::test::Scene3DRenderFixture& fixture() {
    return fixture_;
  }
  PJ::Scene3DDockWidget& dock() {
    return dock_;
  }
  pj::scene3d::SceneViewWidget& view() {
    return *view_;
  }

 private:
  pj::scene3d::test::Scene3DRenderFixture fixture_;
  PJ::Scene3DDockWidget& dock_;
  pj::scene3d::SceneViewWidget* view_ = nullptr;
  std::string skip_reason_;
};

#define SHOW_OR_SKIP(golden_view)                      \
  if (!(golden_view).show()) {                         \
    if (!(golden_view).skipReason().empty()) {         \
      PJ_SKIP_WITHOUT_GPU((golden_view).skipReason()); \
    }                                                  \
    return;                                            \
  }

// The defaults, widened on D3D11 for scenes with 1 px lines (grid, marker edges,
// line strips): WARP rasterizes them wider than Mesa (fills match). OpenGL, Vulkan and
// Metal stay within the defaults.
Tolerance lineSceneTolerance() {
  Tolerance tol;
#ifdef Q_OS_WIN
  tol.coverage = 0.12;
  tol.max_fg_diff_frac = 0.2;
#endif
  return tol;
}

// Why `view` may not write goldens (empty when it may): the references come from
// one pinned software renderer, QRhi OpenGL on Mesa llvmpipe, never from a
// hardware driver or another backend, whose rasterization the tolerances absorb
// only on the comparison side.
std::string goldenWriterRefusal(const QRhiWidget& view) {
  const QRhi* rhi = pj_bench::rhiOf(view);
  if (rhi == nullptr) {
    return "the view has no QRhi";
  }
  const QByteArray& device = rhi->driverInfo().deviceName;
  if (rhi->backend() == QRhi::OpenGLES2 && device.contains("llvmpipe")) {
    return {};
  }
  return "goldens are written only by QRhi OpenGL on Mesa llvmpipe, not by " + std::string(rhi->backendName()) +
         " on '" + device.toStdString() +
         "' (PJ_GRAPHICS_API=opengl LIBGL_ALWAYS_SOFTWARE=1 and Mesa's EGL/GLX vendor, see the golden README)";
}

// The writer on the very stack that wrote the goldens (its device string, which
// names llvmpipe's LLVM version and vector width, matches writer.txt): it
// reproduces them bit for bit, so no tolerance applies there.
bool isGoldenWriterStack(const QRhiWidget& view) {
  QFile record(writerRecordPath());
  return goldenWriterRefusal(view).empty() && record.open(QIODevice::ReadOnly) &&
         record.readAll().trimmed() == pj_bench::rhiOf(view)->driverInfo().deviceName;
}

// Renders `view` from `camera` and compares the frame with the golden: exactly
// on the writer's own stack, else under the scene's checks and tolerances. Or
// writes it as the golden (PJ_UPDATE_POINTCLOUD_GOLDENS=1, pinned writer only).
void expectMatchesGolden(
    const std::string& name, GoldenView& view, const pj::scene3d::CameraState& camera, Scene::Check check,
    const Tolerance& scene_tol = {}) {
  const QImage actual = view.render(camera);
  const Tolerance tol = forCheck(scene_tol, check);
  ASSERT_EQ(actual.size(), QSize(kViewSize, kViewSize));
  const QString golden_file = goldenPath(name);
  if (PJ::sdk::getEnv("PJ_UPDATE_POINTCLOUD_GOLDENS") == "1") {
    const std::string refusal = goldenWriterRefusal(view.view());
    ASSERT_TRUE(refusal.empty()) << refusal;
    ASSERT_TRUE(actual.save(golden_file)) << golden_file.toStdString();
    QFile record(writerRecordPath());
    ASSERT_TRUE(record.open(QIODevice::WriteOnly | QIODevice::Truncate));
    record.write(pj_bench::rhiOf(view.view())->driverInfo().deviceName + '\n');
    GTEST_SKIP() << "golden written: " << golden_file.toStdString();
  }

  const QImage golden = QImage(golden_file).convertToFormat(QImage::Format_RGB32);
  ASSERT_FALSE(golden.isNull()) << "missing golden " << golden_file.toStdString() << " (see the golden README)";
  ASSERT_EQ(golden.size(), actual.size());

  const Stats stats = compare(actual, golden, tol);
  std::printf(
      "[golden] %-34s diff %.4f (per-pixel %.4f)  coverage %.4f  mean colour %d  fg-diff %.4f (<= %.2f)  strays %d "
      "(<= %d, margin %d)  tiles %.4f  tile colour %d  centroid %.2f  bbox %d\n",
      name.c_str(), stats.diff_frac, stats.plain_diff_frac, stats.coverage_ratio, stats.mean_color_delta,
      stats.fg_diff_frac, tol.max_fg_diff_frac, stats.background_strays, tol.max_background_strays,
      tol.background_margin, stats.tile_coverage_delta, stats.tile_colour_delta, stats.centroid_shift,
      stats.bbox_shift);
  const bool check_colour = check != Scene::Check::kCoverage;
  const bool check_pixels = check == Scene::Check::kFull;
  const bool exact = isGoldenWriterStack(view.view());
  const int off_golden = pj::scene3d::test::differingPixels(actual, golden);
  if ((exact ? off_golden != 0 : !judge(stats, tol, check).ok()) ||
      PJ::sdk::getEnv("PJ_POINTCLOUD_GOLDEN_DUMP_ALL") == "1") {
    actual.save(outPath(name, "_actual.png"));
    stats.diff.save(outPath(name, "_diff.png"));
  }
  if (exact) {
    EXPECT_EQ(off_golden, 0) << "pixels off the golden on the stack that wrote it (see the golden README)";
    return;
  }
  EXPECT_LE(std::abs(stats.coverage_ratio - 1.0), tol.coverage) << "non-background pixel count vs golden";
  if (check_colour) {
    EXPECT_LE(stats.mean_color_delta, tol.mean_color) << "mean colour of covered pixels vs golden";
  }
  if (!check_pixels) {
    EXPECT_LE(stats.tile_coverage_delta, tol.coverage + tol.tile_coverage_slack)
        << "covered pixels moved between " << kTile << " px tiles";
    if (check_colour) {
      EXPECT_LE(stats.tile_colour_delta, tol.max_tile_colour) << "mean colour per " << kTile << " px tile vs golden";
    }
    EXPECT_LE(stats.centroid_shift, tol.max_centroid_shift) << "centroid of the covered pixels moved (px)";
    EXPECT_LE(stats.bbox_shift, tol.max_bbox_shift) << "bounding box of the covered pixels moved (px)";
    EXPECT_LE(stats.background_strays, tol.max_background_strays)
        << "non-background pixels more than " << tol.background_margin << " px from the golden's foreground";
  }
  if (check_pixels) {
    EXPECT_LE(stats.diff_frac, tol.max_diff_frac) << "fraction of pixels off by more than " << tol.channel;
    EXPECT_LE(stats.plain_diff_frac, tol.max_plain_diff_frac)
        << "per-pixel fraction off by more than " << tol.channel << " (no neighbourhood allowance)";
    EXPECT_LE(stats.fg_diff_frac, tol.max_fg_diff_frac)
        << "fraction of the foreground off by more than " << tol.channel;
    EXPECT_LE(stats.background_strays, tol.max_background_strays)
        << "non-background pixels more than " << tol.background_margin << " px from the golden's foreground";
  }
}

// ---- Comparator self-test ------------------------------------------------

// A synthetic "render": on white, a lattice of 2 px dots whose colour ramps
// with x (reversed with `reversed`), offset by `dx`, plus a solid 20 px blob
// at (blob_x, blob_y). No GPU: it pins what the coverage modes can see.
QImage syntheticFrame(int dx, int blob_x, int blob_y, bool reversed = false) {
  QImage image(kViewSize, kViewSize, QImage::Format_RGB32);
  image.fill(qRgb(255, 255, 255));
  for (int y = 70; y < 170; y += 4) {
    for (int x = 60; x < 200; x += 4) {
      const int ramp = (x - 60) * 255 / 140;
      const QRgb colour = qRgb(reversed ? 255 - ramp : ramp, 40, reversed ? ramp : 255 - ramp);
      for (int py = 0; py < 2; ++py) {
        for (int px = 0; px < 2; ++px) {
          image.setPixel(x + dx + px, y + py, colour);
        }
      }
    }
  }
  for (int y = blob_y; y < blob_y + 20; ++y) {
    for (int x = blob_x; x < blob_x + 20; ++x) {
      image.setPixel(x, y, qRgb(30, 160, 60));
    }
  }
  return image;
}

// The coverage modes keep a coarse spatial check: a foreground translated by a
// few pixels or rearranged (same pixel count, same mean colour) fails them,
// while a one-pixel jitter (driver rasterization) passes.
TEST(GoldenComparator, CoverageModesRejectMovedForeground) {
  const QImage golden = syntheticFrame(0, 30, 200);
  const Tolerance tol;
  for (const auto check : {Scene::Check::kCoverage, Scene::Check::kCoverageColour}) {
    SCOPED_TRACE(check == Scene::Check::kCoverage ? "kCoverage" : "kCoverageColour");
    const Tolerance mode_tol = forCheck(tol, check);
    const auto verdict = [&](const QImage& actual) {
      return judge(compare(actual, golden, mode_tol), mode_tol, check);
    };
    EXPECT_TRUE(verdict(golden).ok());
    EXPECT_TRUE(verdict(syntheticFrame(1, 31, 200)).ok()) << "a one-pixel jitter";

    // Every case below keeps the pixel count and the mean colour, which is all
    // the coverage modes compared before; only the spatial checks catch them.
    for (const auto& [what, actual] : {
             std::pair{"lattice and blob translated by 12 px", syntheticFrame(12, 42, 200)},
             std::pair{"blob moved across the frame", syntheticFrame(0, 200, 30)},
             std::pair{"blob moved by 16 px", syntheticFrame(0, 46, 200)},
         }) {
      const Verdict result = verdict(actual);
      EXPECT_TRUE(result.coverage && result.colour) << what;
      EXPECT_FALSE(result.spatial) << what;
    }
  }
  // A colour ramp reversed in place: same pixels, same mean colour. Only the
  // per-tile colour sees it; kCoverage does not compare colour at all.
  const QImage reversed = syntheticFrame(0, 30, 200, true);
  const Tolerance colour_tol = forCheck(tol, Scene::Check::kCoverageColour);
  const Verdict result = judge(compare(reversed, golden, colour_tol), colour_tol, Scene::Check::kCoverageColour);
  EXPECT_TRUE(result.coverage && result.colour);
  EXPECT_FALSE(result.spatial);
}

class PointCloudGolden : public ::testing::TestWithParam<Scene> {};

TEST_P(PointCloudGolden, MatchesGolden) {
  const Scene& scene = GetParam();
  PJ_SKIP_ON_HEADLESS_QPA();

  GoldenView golden_view;
  SHOW_OR_SKIP(golden_view);
  PJ::Scene3DDockWidget& dock = golden_view.dock();
  for (std::size_t i = 0; i < scene.layers.size(); ++i) {
    const CloudSpec cloud = scene.layers[i].cloud();
    const auto topic = golden_view.fixture().add(
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
  expectMatchesGolden(scene.name, golden_view, scene.camera, scene.check);
}

INSTANTIATE_TEST_SUITE_P(
    Corpus, PointCloudGolden, ::testing::ValuesIn(corpus()),
    [](const ::testing::TestParamInfo<Scene>& param_info) { return param_info.param.name; });

// ---- Scene corpus --------------------------------------------------------

// The camera of the scene corpus: lower than the clouds' so the far grid edge
// stays below the top-left corner, which every comparison takes as background.
pj::scene3d::CameraState sceneCamera() {
  pj::scene3d::CameraState cam = defaultCamera();
  cam.radius = 6.0f;
  cam.elevation = 0.45f;
  return cam;
}

// Restores the light palette when a dark-theme scene ends.
struct DarkTheme {
  DarkTheme() {
    setWindowColour(QColor(32, 32, 32));
  }
  ~DarkTheme() {
    setWindowColour(QColor(255, 255, 255));
  }
  DarkTheme(const DarkTheme&) = delete;
  DarkTheme& operator=(const DarkTheme&) = delete;
};

struct GridScene {
  std::string name;
  pj::scene3d::SceneViewWidget::GridStyle style;
  bool dark;
  int divisions = 10;
};

class GridGolden : public ::testing::TestWithParam<GridScene> {};

[[maybe_unused]] void PrintTo(const GridScene& scene, std::ostream* os) {
  *os << scene.name;
}

// The ground grid alone (default 10 m, 10 divisions, 5 for the odd case: its
// cells start half a cell off the origin), its colours from the theme. The line
// grid is all 1 px edges,
// which a y-down backend's mirrored MSAA pattern resolves differently (up to 18%
// of its foreground on lavapipe), so it checks coverage, mean colour and the
// coarse spatial checks.
TEST_P(GridGolden, MatchesGolden) {
  const GridScene& scene = GetParam();
  PJ_SKIP_ON_HEADLESS_QPA();
  std::optional<DarkTheme> dark;
  if (scene.dark) {
    dark.emplace();
  }
  GoldenView golden_view;
  SHOW_OR_SKIP(golden_view);
  golden_view.view().setGridStyle(scene.style);
  golden_view.view().setGridDivisions(scene.divisions);
  golden_view.view().setGridVisible(true);
  const bool lines = scene.style == pj::scene3d::SceneViewWidget::GridStyle::kLines;
  // 1 px lines: each backend rasterizes its own share of them, so the
  // coverage tolerance below is per backend.
  Tolerance tol;
  if (lines) {
#ifdef Q_OS_WIN
    tol.coverage = 0.25;
#else
    tol.coverage = 0.08;
#endif
    // A y-down backend's mirrored MSAA pattern keeps more of the lines of one
    // slope than of the other: lavapipe and Iris Vulkan move the centroid by 2.8 px.
    tol.max_centroid_shift = 6.0;
  }
  expectMatchesGolden(
      scene.name, golden_view, sceneCamera(), lines ? Scene::Check::kCoverageColour : Scene::Check::kFull, tol);
}

INSTANTIATE_TEST_SUITE_P(
    Scenes, GridGolden,
    ::testing::Values(
        GridScene{"scene_grid_lines_light", pj::scene3d::SceneViewWidget::GridStyle::kLines, false},
        GridScene{"scene_grid_lines_dark", pj::scene3d::SceneViewWidget::GridStyle::kLines, true},
        GridScene{"scene_grid_filled_light", pj::scene3d::SceneViewWidget::GridStyle::kFilledCells, false},
        GridScene{"scene_grid_filled_dark", pj::scene3d::SceneViewWidget::GridStyle::kFilledCells, true},
        GridScene{"scene_grid_filled_odd", pj::scene3d::SceneViewWidget::GridStyle::kFilledCells, false, 5}),
    [](const ::testing::TestParamInfo<GridScene>& param_info) { return param_info.param.name; });

PJ::sdk::SceneEntities markerScene() {
  PJ::sdk::SceneEntity entity;
  entity.frame_id = "map";
  entity.id = "markers";
  const std::array<PJ::sdk::ColorRGBA, 3> colours = {{{220, 60, 40, 255}, {40, 160, 220, 255}, {240, 200, 40, 255}}};
  for (int i = 0; i < 3; ++i) {
    PJ::sdk::CubePrimitive cube;
    cube.pose.position = {-1.0 + static_cast<double>(i), 0.2 * static_cast<double>(i), 0.4};
    const double half_angle = 0.3 * static_cast<double>(i);
    cube.pose.orientation = {.x = 0.0, .y = 0.0, .z = std::sin(half_angle), .w = std::cos(half_angle)};
    cube.size = {0.6, 0.6, 0.8};
    cube.color = colours[static_cast<std::size_t>(i)];
    entity.cubes.push_back(cube);
  }
  for (int strip = 0; strip < 2; ++strip) {
    PJ::sdk::LinePrimitive line;
    line.thickness = 0.02;
    line.color = strip == 0 ? PJ::sdk::ColorRGBA{30, 140, 60, 255} : PJ::sdk::ColorRGBA{160, 40, 200, 255};
    for (int v = 0; v <= 24; ++v) {
      const double angle = 0.25 * static_cast<double>(v);
      const double radius = 1.4 + 0.3 * static_cast<double>(strip);
      line.points.push_back({radius * std::cos(angle), radius * std::sin(angle), 0.05 * static_cast<double>(v)});
    }
    entity.lines.push_back(std::move(line));
  }
  PJ::sdk::SceneEntities scene;
  scene.entities.push_back(std::move(entity));
  return scene;
}

struct MarkerScene {
  std::string name;
  const char* opacity;  // nullptr: the layer default
};

[[maybe_unused]] void PrintTo(const MarkerScene& scene, std::ostream* os) {
  *os << scene.name;
}

class MarkerGolden : public ::testing::TestWithParam<MarkerScene> {};

// SceneEntities cubes (fill + edge overlay) and line strips, at the layer's
// default opacity and fully opaque.
TEST_P(MarkerGolden, MatchesGolden) {
  const MarkerScene& scene = GetParam();
  PJ_SKIP_ON_HEADLESS_QPA();
  GoldenView golden_view;
  SHOW_OR_SKIP(golden_view);
  const auto topic = golden_view.fixture().add(
      "/markers", PJ::sdk::BuiltinObjectType::kSceneEntities, PJ::serializeSceneEntities(markerScene()));
  if (scene.opacity != nullptr) {
    PJ::ISceneLayer* layer = golden_view.dock().layerFor(topic);
    ASSERT_NE(layer, nullptr);
    QDomDocument doc;
    QDomElement state = layer->xmlSaveState(doc);
    state.setAttribute(u"opacity"_s, QString::fromLatin1(scene.opacity));
    ASSERT_TRUE(layer->xmlLoadState(state));
  }
  golden_view.dock().setFixedFrame(u"map"_s);
  golden_view.dock().onTrackerTime(0.0);
  expectMatchesGolden(scene.name, golden_view, sceneCamera(), Scene::Check::kFull, lineSceneTolerance());
}

INSTANTIATE_TEST_SUITE_P(
    Scenes, MarkerGolden,
    ::testing::Values(MarkerScene{"scene_markers_default", nullptr}, MarkerScene{"scene_markers_opaque", "1"}),
    [](const ::testing::TestParamInfo<MarkerScene>& param_info) { return param_info.param.name; });

// A 1 m ASCII STL cube centred on its origin.
QByteArray stlCube() {
  QByteArray stl = "solid cube\n";
  const auto facet = [&stl](const glm::vec3& normal, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    stl += QByteArray("facet normal ") + QByteArray::number(normal.x) + ' ' + QByteArray::number(normal.y) + ' ' +
           QByteArray::number(normal.z) + "\n outer loop\n";
    for (const glm::vec3& v : {a, b, c}) {
      stl += QByteArray("  vertex ") + QByteArray::number(v.x) + ' ' + QByteArray::number(v.y) + ' ' +
             QByteArray::number(v.z) + '\n';
    }
    stl += " endloop\nendfacet\n";
  };
  for (int axis = 0; axis < 3; ++axis) {
    for (const float sign : {-1.0F, 1.0F}) {
      glm::vec3 normal(0.0F);
      normal[axis] = sign;
      glm::vec3 u(0.0F);
      glm::vec3 v(0.0F);
      u[(axis + 1) % 3] = 0.5F;
      v[(axis + 2) % 3] = 0.5F;
      if (sign < 0.0F) {
        std::swap(u, v);  // counter-clockwise seen from outside
      }
      const glm::vec3 centre = 0.5F * normal;
      facet(normal, centre - u - v, centre + u - v, centre + u + v);
      facet(normal, centre - u - v, centre + u + v, centre - u + v);
    }
  }
  return stl + "endsolid cube\n";
}

// A URDF robot of the three primitives without a material plus an STL link,
// lit, over the line grid.
TEST(SceneGolden, RobotPrimitivesAndMesh) {
  PJ_SKIP_ON_HEADLESS_QPA();
  GoldenView golden_view;
  SHOW_OR_SKIP(golden_view);
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  QFile mesh(dir.filePath(u"cube.stl"_s));
  ASSERT_TRUE(mesh.open(QIODevice::WriteOnly));
  mesh.write(stlCube());
  mesh.close();
  const QByteArray urdf = R"(<robot name="golden">
  <link name="base_link">
    <visual><origin xyz="-1.1 0 0.3"/><geometry><box size="0.6 0.6 0.6"/></geometry></visual>
    <visual><origin xyz="-0.2 0.6 0.4"/><geometry><cylinder radius="0.3" length="0.8"/></geometry></visual>
    <visual><origin xyz="0.6 -0.4 0.35"/><geometry><sphere radius="0.35"/></geometry></visual>
  </link>
  <link name="mesh_link">
    <visual><geometry><mesh filename="cube.stl" scale="0.5 0.5 0.5"/></geometry></visual>
  </link>
  <joint name="mesh_joint" type="fixed">
    <parent link="base_link"/><child link="mesh_link"/><origin xyz="1.2 0.6 0.25" rpy="0 0 0.5"/>
  </joint>
</robot>)";
  const QString path = dir.filePath(u"golden.urdf"_s);
  QFile file(path);
  ASSERT_TRUE(file.open(QIODevice::WriteOnly));
  ASSERT_EQ(file.write(urdf), urdf.size());
  file.close();
  ASSERT_NE(golden_view.dock().addRobotModelLayer(path).id, 0U);
  golden_view.view().setGridVisible(true);
  golden_view.dock().setFixedFrame(u"base_link"_s);
  golden_view.dock().onTrackerTime(0.0);
  expectMatchesGolden("scene_robot_primitives", golden_view, sceneCamera(), Scene::Check::kFull, lineSceneTolerance());
}

}  // namespace
