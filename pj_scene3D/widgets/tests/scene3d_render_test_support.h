// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

// Shared scaffolding for the tests that render a real Scene3DDockWidget and
// read back pixels (scene3d_rhi_render_test, pointcloud_golden_test, ...).
// Header-only; each consumer is its own runner.

#include <gtest/gtest.h>

#include <QApplication>
#include <QEventLoop>
#include <QImage>
#include <QPointer>
#include <QRect>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <glm/glm.hpp>
#include <string>
#include <utility>
#include <vector>

#include "../rhi/passes/overlay_rhi_pass.h"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/frame_transforms_codec.hpp"
#include "pj_base/builtin/point_cloud.hpp"
#include "pj_base/builtin/point_cloud_codec.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_gpu_test_gate.h"  // PJ_SKIP_WITHOUT_GPU
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/TransformService.h"
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#include "pj_scene3d_widgets/scene_view_widget.h"
#include "pj_widgets/RhiTestHooks.h"

namespace pj::scene3d::test {

// Runs before main(); see pjPreferRealDisplayQpa().
inline const bool kRealDisplayPlatform = pjPreferRealDisplayQpa();

// Arms the shader-named fault injection (PJ::kRhiTestFailShaderEnv) for `shader`,
// a shader path suffix, until clear() or destruction.
class PipelineFault {
 public:
  explicit PipelineFault(const char* shader) {
    qputenv(PJ::kRhiTestFailShaderEnv, shader);
  }
  ~PipelineFault() {
    clear();
  }
  PipelineFault(const PipelineFault&) = delete;
  PipelineFault& operator=(const PipelineFault&) = delete;
  void clear() {
    qunsetenv(PJ::kRhiTestFailShaderEnv);
  }
};

// One extra per-point field written after xyz; `write` fills point i's bytes.
struct ExtraField {
  std::string name;
  PJ::sdk::PointField::Datatype datatype;
  std::function<void(std::size_t i, uint8_t* dst)> write;
};

// Serialized canonical PointCloud stamped at 100 ns: float32 xyz at offset 0,
// extra fields packed after it at their natural alignment, point_step padded to
// 4 bytes (the zero-copy fast-path layout).
inline std::vector<uint8_t> serializeCloud(
    const std::vector<glm::vec3>& positions, const std::string& frame_id, const std::vector<ExtraField>& fields = {}) {
  using DT = PJ::sdk::PointField::Datatype;
  PJ::sdk::PointCloud cloud;
  cloud.frame_id = frame_id;
  cloud.fields = {{"x", 0, DT::kFloat32, 1}, {"y", 4, DT::kFloat32, 1}, {"z", 8, DT::kFloat32, 1}};
  uint32_t offset = 12;
  std::vector<uint32_t> offsets;
  for (const auto& field : fields) {
    const uint32_t size = PJ::sdk::bytesPerElement(field.datatype);
    offset = (offset + size - 1) / size * size;
    offsets.push_back(offset);
    cloud.fields.push_back({field.name, offset, field.datatype, 1});
    offset += size;
  }
  cloud.point_step = (offset + 3U) / 4U * 4U;
  cloud.width = static_cast<uint32_t>(positions.size());
  cloud.height = 1;
  cloud.row_step = cloud.point_step * cloud.width;
  std::vector<uint8_t> bytes(static_cast<std::size_t>(cloud.row_step), 0);
  for (std::size_t i = 0; i < positions.size(); ++i) {
    uint8_t* point = bytes.data() + i * cloud.point_step;
    std::memcpy(point, &positions[i], 12);
    for (std::size_t f = 0; f < fields.size(); ++f) {
      fields[f].write(i, point + offsets[f]);
    }
  }
  cloud.data = PJ::Span<const uint8_t>(bytes.data(), bytes.size());
  cloud.timestamp_ns = 100;
  return PJ::serializePointCloud(cloud);
}

// Serialized canonical FrameTransforms stamped `t_ns`: each child at its
// translation from `parent`, identity rotation.
inline std::vector<uint8_t> serializeTfEdges(
    const std::string& parent, const std::vector<std::pair<std::string, glm::dvec3>>& children, PJ::Timestamp t_ns) {
  PJ::sdk::FrameTransforms transforms;
  for (const auto& [child, offset] : children) {
    PJ::sdk::FrameTransform edge;
    edge.timestamp = t_ns;
    edge.parent_frame_id = parent;
    edge.child_frame_id = child;
    edge.translation = {offset.x, offset.y, offset.z};
    edge.rotation.w = 1.0;
    transforms.transforms.push_back(edge);
  }
  return PJ::serializeFrameTransforms(transforms);
}

// A Scene3D dock on a real SessionManager/ObjectStore, inside a top-level
// window, fed serialized canonical objects (no parser). The members are
// ordered so the dock dies before the services it points at.
class Scene3DRenderFixture {
 public:
  Scene3DRenderFixture() : transforms_(session_) {
    dataset_ = *session_.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "test", .time_domain_id = 0});
    dock_ = new PJ::Scene3DDockWidget(&window_);
    dock_->setSessionManager(&session_);
    auto* layout = new QVBoxLayout(&window_);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(dock_);
  }
  Scene3DRenderFixture(const Scene3DRenderFixture&) = delete;
  Scene3DRenderFixture& operator=(const Scene3DRenderFixture&) = delete;

  // Registers `topic`, pushes `bytes` as its one object (t = 100 ns) and adds it to the dock.
  PJ::ObjectTopicId add(const std::string& topic, PJ::sdk::BuiltinObjectType type, std::vector<uint8_t> bytes) {
    const PJ::ObjectTopicId id = registerTopic(topic, type);
    push(id, 100, std::move(bytes));
    EXPECT_TRUE(dock_->addTopic(id, type, QString::fromStdString(topic)));
    return id;
  }

  // Registers `topic` in the fixture's dataset without adding it to the dock
  // (a sibling a layer discovers on its own, such as an occupancy grid's _updates).
  PJ::ObjectTopicId registerTopic(const std::string& topic, PJ::sdk::BuiltinObjectType type) {
    const auto id = session_.objectStore().registerTopic(
        PJ::ObjectTopicDescriptor{
            .dataset_id = dataset_,
            .topic_name = topic,
            .metadata_json = R"({"builtin_object_type":")" + std::string(PJ::sdk::name(type)) + "\"}",
        });
    EXPECT_TRUE(id.has_value());
    return *id;
  }

  // Pushes one more object at `t_ns` for a registered topic.
  void push(PJ::ObjectTopicId id, PJ::Timestamp t_ns, std::vector<uint8_t> bytes) {
    EXPECT_TRUE(session_.objectStore().pushOwned(id, t_ns, std::move(bytes)).has_value());
  }

  QWidget& window() {
    return window_;
  }
  // Null once a test reparents the dock into a window it destroys.
  PJ::Scene3DDockWidget* dock() {
    return dock_;
  }
  // The dataset every add()ed topic belongs to.
  [[nodiscard]] PJ::DatasetId dataset() const {
    return dataset_;
  }
  // Not wired by default: connect it with dock()->setTransformService().
  PJ::TransformService& transforms() {
    return transforms_;
  }

 private:
  PJ::SessionManager session_;
  PJ::TransformService transforms_;
  PJ::DatasetId dataset_ = 0;
  QWidget window_;
  QPointer<PJ::Scene3DDockWidget> dock_;
};

// One presented frame as RGB32, after letting queued layer/frame reconciliation
// run. Null when the view could not present (occluded window, no driver).
inline QImage grab(pj::scene3d::SceneViewWidget& view) {
  return pjWaitForGrab([&view](auto done) { view.grabPresentedFrame(std::move(done)); });
}

// Grabs until two consecutive frames match, so a frame still settling (queued
// layer work, adaptive near/far) is not compared.
inline QImage grabStable(pj::scene3d::SceneViewWidget& view) {
  QImage previous = grab(view);
  for (int i = 0; i < 12; ++i) {
    QImage next = grab(view);
    if (next == previous) {
      return next;
    }
    previous = std::move(next);
  }
  return previous;
}

// |dR| + |dG| + |dB|: how visibly a pixel changed between two frames.
inline int channelDeltaSum(QRgb a, QRgb b) {
  return std::abs(qRed(a) - qRed(b)) + std::abs(qGreen(a) - qGreen(b)) + std::abs(qBlue(a) - qBlue(b));
}

// Pixels whose channelDeltaSum() between `a` and `b` exceeds `threshold`; every
// pixel when the sizes differ.
inline int differingPixels(const QImage& a, const QImage& b, int threshold = 0) {
  if (a.size() != b.size()) {
    return a.width() * a.height();
  }
  int count = 0;
  for (int y = 0; y < a.height(); ++y) {
    for (int x = 0; x < a.width(); ++x) {
      count += channelDeltaSum(a.pixel(x, y), b.pixel(x, y)) > threshold ? 1 : 0;
    }
  }
  return count;
}

// max(|dR|, |dG|, |dB|): the per-channel tolerance a golden comparison applies.
inline int maxChannelDelta(QRgb a, QRgb b) {
  return std::max({std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)), std::abs(qBlue(a) - qBlue(b))});
}

// Pixels of `image` in `area` (the whole image when empty) whose colour satisfies `match`.
inline int countPixels(const QImage& image, const std::function<bool(QRgb)>& match, QRect area = {}) {
  area = area.isEmpty() ? image.rect() : area.intersected(image.rect());
  int count = 0;
  for (int y = area.top(); y <= area.bottom(); ++y) {
    for (int x = area.left(); x <= area.right(); ++x) {
      count += match(image.pixel(x, y)) ? 1 : 0;
    }
  }
  return count;
}

// The corner gizmo's rectangle in a frame of `view` grabbed as `image`: its
// logical footprint scaled to the image, bottom-right.
inline QRect gizmoRect(const QWidget& view, const QImage& image) {
  const int size = pj::scene3d::OverlayRhiPass::kGizmoSizePx * image.height() / view.height();
  return {image.width() - size, image.height() - size, size, size};
}

}  // namespace pj::scene3d::test
