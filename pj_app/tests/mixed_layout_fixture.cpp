// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "mixed_layout_fixture.h"

#include <qwt_plot_curve.h>

#include <QDomDocument>
#include <QPen>
#include <cstdint>
#include <string>
#include <vector>

#include "pj_base/builtin/image.hpp"
#include "pj_base/builtin/image_codec.hpp"
#include "pj_base/builtin/point_cloud.hpp"
#include "pj_base/builtin/point_cloud_codec.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_scene3d_widgets/scene_view_widget.h"

using namespace Qt::StringLiterals;

namespace pj_mixed_layout {
namespace {

constexpr PJ::Timestamp kObjectTime = 100;

void require(bool ok, const char* what) {
  if (!ok) {
    qFatal("mixed_layout_fixture: %s", what);
  }
}

// A float32 xyz cloud of `side`^3 points spanning [-0.5, 0.5]^3.
std::vector<uint8_t> cubeCloud(int side) {
  std::vector<float> xyz;
  xyz.reserve(static_cast<std::size_t>(side * side * side * 3));
  const float span = side > 1 ? static_cast<float>(side - 1) : 1.0F;
  const auto coord = [span](int index) { return static_cast<float>(index) / span - 0.5F; };
  for (int i = 0; i < side; ++i) {
    for (int j = 0; j < side; ++j) {
      for (int k = 0; k < side; ++k) {
        xyz.insert(xyz.end(), {coord(i), coord(j), coord(k)});
      }
    }
  }
  using DT = PJ::sdk::PointField::Datatype;
  PJ::sdk::PointCloud cloud;
  cloud.frame_id = "map";
  cloud.fields = {{"x", 0, DT::kFloat32, 1}, {"y", 4, DT::kFloat32, 1}, {"z", 8, DT::kFloat32, 1}};
  cloud.point_step = 12;
  cloud.width = static_cast<uint32_t>(xyz.size() / 3);
  cloud.height = 1;
  cloud.row_step = cloud.point_step * cloud.width;
  cloud.data = PJ::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(xyz.data()), xyz.size() * sizeof(float));
  cloud.timestamp_ns = kObjectTime;
  return PJ::serializePointCloud(cloud);
}

}  // namespace

MixedLayout::MixedLayout() : window_(std::make_unique<QMainWindow>()) {
  manager_ = new ads::CDockManager(window_.get());
  // ADS's Wayland stays-on-top emulation can hide a floated dock.
  window_->removeEventFilter(manager_);
}

MixedLayout::~MixedLayout() = default;

ads::CDockWidget* MixedLayout::addDock(const QString& name, QWidget* content, ads::DockWidgetArea area) {
  auto* dock = new ads::CDockWidget(manager_, name);
  dock->setWidget(content, ads::CDockWidget::ForceNoScrollArea);
  manager_->addDockWidget(area, dock);
  return dock;
}

PJ::PlotWidget* MixedLayout::createPlot(int curves, int samples) {
  PJ::PlotWidgetBase::setGpuCanvasDisabledOverride(false);
  const auto dataset = session_.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "plot.mcap"});
  require(dataset.has_value(), "plot dataset");
  PJ::DataWriter writer = session_.dataEngine().createWriter();
  for (int c = 0; c < curves; ++c) {
    auto handle = writer.registerScalarSeries(*dataset, "/curve_" + std::to_string(c), PJ::NumericType::kFloat64);
    require(handle.has_value(), "plot series");
    for (int i = 0; i < samples; ++i) {
      writer.appendScalar(*handle, 1'000'000'000 + i * 100'000'000LL, static_cast<double>(i % 5 + c));
    }
  }
  require(!session_.commitChunks(writer.flushAll()).empty(), "plot samples");
  auto* plot = new PJ::PlotWidget(&session_, &catalog_);
  for (const auto& curve : catalog_.curves()) {
    if (curve.dataset_id != *dataset) {
      continue;
    }
    auto* info = plot->addCurve(curve.name, kCurveColour);
    require(info != nullptr, "plot curve");
    info->curve->setPen(QPen(kCurveColour, 3.0));
  }
  plot->setGridVisible(false);
  plot->setLegendVisible(false);
  plot->zoomOut(/*emit_signal=*/false);
  return plot;
}

PJ::Scene3DDockWidget* MixedLayout::createScene3D(int cloud_side) {
  auto* dock = new PJ::Scene3DDockWidget();
  dock->setSessionManager(&session_);
  dock->setTransformService(&transforms_);
  const auto dataset =
      session_.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "scene", .time_domain_id = 0});
  require(dataset.has_value(), "scene dataset");
  const auto topic = session_.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset,
          .topic_name = "/points",
          .metadata_json = R"({"builtin_object_type":"kPointCloud"})"});
  require(topic.has_value(), "cloud topic");
  require(session_.objectStore().pushOwned(*topic, kObjectTime, cubeCloud(cloud_side)).has_value(), "cloud");
  require(dock->addTopic(*topic, PJ::sdk::BuiltinObjectType::kPointCloud, u"points"_s), "cloud layer");
  // Through the persisted state: the layer's setters are private to its UI.
  auto* layer = dock->layerFor(*topic);
  require(layer != nullptr, "cloud layer lookup");
  QDomDocument doc;
  QDomElement state = layer->xmlSaveState(doc);
  state.setAttribute(u"color_type"_s, u"solid"_s);
  state.setAttribute(u"color_choice_explicit"_s, u"true"_s);
  state.setAttribute(u"solid_color"_s, u"#00ff00"_s);
  state.setAttribute(u"size_meters"_s, u"0.1"_s);
  require(layer->xmlLoadState(state), "cloud layer state");
  dock->setFixedFrame(u"map"_s);
  require(dock->sceneView() != nullptr, "scene view");
  dock->sceneView()->setGridVisible(false);
  dock->sceneView()->setAxesVisible(false);
  return dock;
}

PJ::Scene2DDockWidget* MixedLayout::createMedia(QWidget* parent) {
  constexpr uint32_t kSize = 64;
  std::vector<uint8_t> pixels(static_cast<size_t>(kSize * kSize * 4));
  for (uint32_t y = 0; y < kSize; ++y) {
    for (uint32_t x = 0; x < kSize; ++x) {
      uint8_t* px = pixels.data() + static_cast<size_t>((y * kSize + x) * 4);
      px[0] = y < kSize / 2 ? 255 : 0;
      px[2] = y < kSize / 2 ? 0 : 255;
      px[3] = 255;
    }
  }
  PJ::sdk::Image image;
  image.timestamp_ns = kObjectTime;
  image.width = kSize;
  image.height = kSize;
  image.encoding = "rgba8";
  image.data = PJ::Span<const uint8_t>(pixels.data(), pixels.size());
  const auto dataset =
      session_.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "media", .time_domain_id = 0});
  require(dataset.has_value(), "media dataset");
  const auto topic = session_.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset, .topic_name = "/camera/image", .metadata_json = R"({"image_codec":"pj_image_v1"})"});
  require(topic.has_value(), "image topic");
  require(session_.objectStore().pushOwned(*topic, kObjectTime, PJ::serializeImage(image)).has_value(), "image");

  auto* dock = new PJ::Scene2DDockWidget(parent);
  dock->setSessionManager(&session_);
  require(dock->addTopic(*topic, PJ::sdk::BuiltinObjectType::kImage, u"image"_s), "image layer");
  dock->onTrackerTime(static_cast<double>(kObjectTime) / 1'000'000'000.0);
  return dock;
}

}  // namespace pj_mixed_layout
