#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QColor>
#include <QString>
#include <QStringList>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/depth_image.hpp"
#include "pj_base/builtin/image.hpp"
#include "pj_datastore/object_store.hpp"  // ObjectStore::TimeRangeEntry (SampleId)
#include "pj_scene3d_core/depth_backproject.h"
#include "pj_scene3d_widgets/passes/pointcloud_render_pass.h"
#include "pj_scene3d_widgets/scene3d_layer.h"

class QWidget;

namespace pj::scene3d {

// True for the canonical depth `encoding` strings this layer back-projects. The
// scene3D dock uses this (peeking a topic's first sample) to offer ONLY depth
// images as DepthClouds — color images share the kImage type but are excluded.
[[nodiscard]] bool isDepthEncoding(const std::string& encoding);

// Scene3D layer that renders a depth image as a back-projected point cloud (the
// "DepthCloud" visualization). Each depth pixel is unprojected through the pinhole
// intrinsics into a 3D point in the camera optical frame; the cloud is drawn
// through the SAME PointcloudRenderPass the PointCloudLayer uses.
//
// Input type: kImage. In PJ4 depth arrives as sdk::Image with a depth `encoding`
// (16UC1 / 32FC1 / compressedDepth) — there is no kDepthImage producer (the parser
// can't distinguish depth from color at schema-classification time). The Image
// carries `frame_id` (placement); intrinsics come from a CameraInfo topic matched
// by frame_id. compressedDepth payloads are PNG-decoded via QImage in the layer.
//
// POC scope: CPU back-projection via depthToPoints() feeding the existing pass,
// colored by depth. A GPU attributeless pass + aligned RGB-D coloring are planned
// follow-ups behind this same layer.
class DepthCloudLayer : public Scene3DLayer {
  Q_OBJECT
 public:
  DepthCloudLayer(
      PJ::ObjectTopicId topic_id, QString display_name, PJ::sdk::BuiltinObjectType object_type,
      QObject* parent = nullptr);
  ~DepthCloudLayer() override;

  // Scene3DLayer / ISceneLayer
  [[nodiscard]] PJ::SceneLayerInfo info() const override;
  [[nodiscard]] PJ::Range<PJ::Timepoint> timeRange() const override;
  [[nodiscard]] QStringList fallbackFrames() const override;
  [[nodiscard]] QString sourceFrame() const override;
  QDomElement xmlSaveState(QDomDocument& doc) const override;
  bool xmlLoadState(const QDomElement& element) override;

  bool attach(const PJ::SceneLayerContext& ctx) override;
  void detach() override;

  void setFixedFrame(const QString& frame) override;
  void setTrackerTime(PJ::Timepoint time) override;
  void setVisible(bool visible) override;

  void initializeGL() override;
  void render(const ViewParams& view_params, const FrameContext& frame_ctx) override;
  void releaseGL() override;
  [[nodiscard]] std::optional<AABB> worldBounds() const override {
    return world_bounds_;
  }

  QWidget* createConfigWidget(QWidget* parent) override;

  // Config accessors/mutators used by the settings widget.
  [[nodiscard]] PointcloudRenderPass::Colormap colormap() const {
    return colormap_;
  }
  [[nodiscard]] float pointSizePixels() const {
    return point_size_px_;
  }
  [[nodiscard]] float minDepth() const {
    return min_depth_m_;
  }
  [[nodiscard]] float maxDepth() const {
    return max_depth_m_;
  }
  void setColormap(PointcloudRenderPass::Colormap cm);
  void setPointSizePixels(float pixels);
  void setMinDepth(float metres);
  void setMaxDepth(float metres);

#ifdef PJ_SCENE3D_TEST_HOOKS
  void renderAtForTest(int64_t time_ns) {
    renderAt(time_ns);
  }
  [[nodiscard]] std::optional<int64_t> lastPushedStampForTest() const {
    return last_pushed_id_ == SampleId{} ? std::nullopt : std::optional<int64_t>{last_pushed_id_.timestamp};
  }
  [[nodiscard]] std::size_t lastPointCountForTest() const {
    return last_point_count_;
  }
  [[nodiscard]] QString sourceFrameForTest() const {
    return QString::fromStdString(source_frame_);
  }
#endif

 private:
  // Sample identity computable WITHOUT resolving the payload: the entry's
  // SequentialUID (never reused, so no aliasing across duplicate stamps or
  // reloads) plus its store timestamp for display/tests. Exactly what
  // latestEntryIdAt() returns; value-initialized {} is the "none" sentinel.
  using SampleId = PJ::ObjectStore::TimeRangeEntry;

  // Resolved-intrinsics memo: skips re-parsing every CameraInfo on every depth
  // frame (calibration is latched/constant). A cache hit requires `frame_id` to
  // match the current image AND the supplying CameraInfo topic to still yield
  // `camera_sample` at the playhead — a metadata-only latestEntryIdAt() identity
  // check, no parse and no payload resolve.
  // Reset on detach and whenever no camera resolves; see resolveIntrinsics().
  struct IntrinsicsCache {
    std::string frame_id;
    PJ::ObjectTopicId camera_topic;
    SampleId camera_sample;
    DepthIntrinsics intr;
  };

  void renderAt(int64_t time_ns);
  void refreshNow();
  void updateSourceFrame(const std::string& frame_id);

  // Turn a depth-encoded sdk::Image into a raw sdk::DepthImage the core can
  // back-project. Raw encodings (16UC1/32FC1) become a zero-copy view over the
  // image bytes; compressedDepth is PNG-decoded (via QImage) into `scratch` and
  // returned as a 16UC1 view over it. Returns nullopt for a non-depth encoding or
  // an undecodable payload. `scratch` MUST outlive use of the returned view.
  std::optional<PJ::sdk::DepthImage> toDepthView(const PJ::sdk::Image& image, std::vector<uint8_t>& scratch);

  // Intrinsics for `frame_id` from a CameraInfo topic. `frame_id` is the
  // authoritative join key (never the topic name): a CameraInfo whose own
  // frame_id matches wins. With no match, falls back to a lone CameraInfo ONLY
  // when unambiguous (a single camera in the dataset, or a frame-less image);
  // with multiple cameras and no match it returns an invalid result rather than
  // risk the wrong camera. Invalid also when no CameraInfo yields usable focals.
  DepthIntrinsics resolveIntrinsics(const std::string& frame_id, int64_t time_ns);

  // True when the memoized intrinsics (if any) still describe the CameraInfo
  // sample at the playhead — metadata-only, no parse and no payload resolve.
  // False when the camera topic has a new sample: the unchanged depth image must
  // then re-project under the new calibration, so renderAt's same-depth-sample
  // skip is gated on this too. No memoized intrinsics counts as current (nothing
  // can go stale; the resolve path scans and warns as usual).
  [[nodiscard]] bool intrinsicsCurrentAt(int64_t time_ns) const;

  PJ::ObjectTopicId topic_id_;
  QString display_name_;
  PJ::sdk::BuiltinObjectType object_type_ = PJ::sdk::BuiltinObjectType::kImage;
  Scene3DLayerContext ctx_;

  std::string source_frame_;  // Image.frame_id (camera optical frame); "" when unknown
  QString fixed_frame_;       // dock's fixed frame; the placement fallback
  std::optional<PJ::Timepoint> decoded_at_ns_;
  std::optional<int64_t> ts_first_;
  bool tracker_dirty_ = false;
  bool visible_ = true;
  std::optional<AABB> world_bounds_;

  PointcloudRenderPass::Colormap colormap_ = PointcloudRenderPass::Colormap::kTurbo;
  float point_size_px_ = 2.0f;
  float min_depth_m_ = 0.0f;
  float max_depth_m_ = 0.0f;

  SampleId last_pushed_id_;
  std::size_t last_point_count_ = 0;
  std::optional<IntrinsicsCache> intrinsics_cache_;

  PointcloudRenderPass cloud_pass_;
};

}  // namespace pj::scene3d
