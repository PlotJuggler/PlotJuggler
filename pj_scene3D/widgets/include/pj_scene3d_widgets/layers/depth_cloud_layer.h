#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QColor>
#include <QString>
#include <QStringList>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/depth_image.hpp"
#include "pj_base/builtin/image.hpp"
#include "pj_datastore/object_store.hpp"  // ObjectStore::TimeRangeEntry (SampleId)
#include "pj_scene3d_core/depth_backproject.h"
#include "pj_scene3d_core/scene_limits.h"  // BudgetLatch
#include "pj_scene3d_widgets/latest_wins_worker.h"
#include "pj_scene3d_widgets/layer_style.h"
#include "pj_scene3d_widgets/point_renderable.h"
#include "pj_scene3d_widgets/scene3d_layer.h"
#ifndef PJ_SCENE3D_RHI
#include "pj_scene3d_widgets/passes/pointcloud_render_pass.h"
#endif

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
//
// Sample resolution, the CameraInfo join, kSceneLimits admission (before any
// decode), persistence and the config widget are shared by both renderers. GL
// build: back-projection runs synchronously and feeds the layer's own
// PointcloudRenderPass. QRhi build (PJ_SCENE3D_RHI): the PNG inflate +
// back-projection run on the Qt thread pool (latest wins, stale results dropped),
// and the result is retained as PointVertex data the view reads through
// PointRenderable.
class DepthCloudLayer : public Scene3DLayer, public PointRenderable {
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

#ifndef PJ_SCENE3D_RHI
  void initializeGL() override;
  void render(const ViewParams& view_params, const FrameContext& frame_ctx) override;
  void releaseGL() override;
#endif
  [[nodiscard]] std::optional<AABB> worldBounds() const override {
    return world_bounds_;
  }

  QWidget* createConfigWidget(QWidget* parent) override;

  // Config accessors/mutators used by the settings widget.
  [[nodiscard]] PJ::Colormap colormap() const override {
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
  void setColormap(PJ::Colormap cm);
  void setPointSizePixels(float pixels);
  void setMinDepth(float metres);
  void setMaxDepth(float metres);

  // Full validation of a <depthcloud> payload without mutating any layer.
  [[nodiscard]] static bool validateXml(const QDomElement& element);

  // PointRenderable (read by the QRhi view; the GL build keeps these empty).
  // prepareForRender() drains a pending tracker move (queuing an off-thread
  // back-projection) and enforces the view's aggregate vertex budget.
  bool prepareForRender(std::uint64_t remaining_view_vertices) override;
  [[nodiscard]] const QByteArray& vertexBytes() const override {
    return vertex_bytes_;
  }
  [[nodiscard]] PointVertexFormat vertexFormat() const override {
    return PointVertexFormat::kXyzScalar;  // coloured by depth, always
  }
  [[nodiscard]] std::size_t pointCount() const override {
    return point_count_;
  }
  [[nodiscard]] std::uint64_t geometryRevision() const override {
    return geometry_revision_;
  }
  // The image's frame, or the dock's fixed frame for a frame-less image.
  [[nodiscard]] const std::string& renderFrameId() const override {
    return render_frame_;
  }
  [[nodiscard]] std::optional<AABB> sourceBounds() const override {
    return world_bounds_;
  }
  [[nodiscard]] bool visible() const override {
    return visible_;
  }
  [[nodiscard]] PointShape shape() const override {
    return PointShape::kPoint;
  }
  [[nodiscard]] PointColorType colorType() const override {
    return PointColorType::kField;
  }
  [[nodiscard]] float sizeMeters() const override {
    return 0.02f;  // unused by the kPoint shape
  }
  [[nodiscard]] float sizePixels() const override {
    return point_size_px_;
  }
  [[nodiscard]] QColor solidColor() const override {
    return QColor(Qt::white);
  }
  [[nodiscard]] bool invertLut() const override {
    return false;
  }
  [[nodiscard]] float outsideRangeAlpha() const override {
    return 1.0f;
  }
  [[nodiscard]] int scalarAxis() const override {
    return -1;  // coloured by the depth scalar
  }
  [[nodiscard]] std::pair<float, float> scalarRange(const glm::mat4& /*fixed_from_source*/) const override {
    return scalar_range_;
  }
  void noteRenderedScalarRange(float /*minimum*/, float /*maximum*/) override {}
  [[nodiscard]] bool isDepthCloud() const override {
    return true;
  }

  // Back-projection worker counters, read by the browser acceptance probes.
  [[nodiscard]] bool decodeInFlightForTest() const {
    return backproject_worker_.busy();
  }
  [[nodiscard]] std::uint64_t decodeStartsForTest() const {
    return backproject_worker_.starts();
  }
  [[nodiscard]] std::uint64_t decodeCompletionsForTest() const {
    return backproject_worker_.completions();
  }
  [[nodiscard]] std::uint64_t decodeOffMainCompletionsForTest() const {
    return backproject_worker_.offMainCompletions();
  }

#ifdef PJ_SCENE3D_TEST_HOOKS
  void renderAtForTest(int64_t time_ns) {
    renderAt(time_ns);
  }
  [[nodiscard]] std::optional<int64_t> lastPushedStampForTest() const {
    return pushed_.sample == SampleId{} ? std::nullopt : std::optional<int64_t>{pushed_.sample.timestamp};
  }
  [[nodiscard]] std::size_t lastPointCountForTest() const {
#ifdef PJ_SCENE3D_RHI
    return point_count_;
#else
    return last_point_count_;
#endif
  }
  [[nodiscard]] QString sourceFrameForTest() const {
    return QString::fromStdString(source_frame_);
  }
  // Runs `gate` on the worker before each back-projection (a test holds a job with it).
  void setBackprojectGateForTest(std::function<void()> gate) {
    backproject_worker_.setJob([gate = std::move(gate)](const BackprojectRequest& request) {
      gate();
      return backproject(request);
    });
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

  struct Settings {
    PJ::Colormap colormap = PJ::Colormap::kTurbo;
    float point_size_px = 2.0f;
    float min_depth_m = 0.0f;
    float max_depth_m = 0.0f;
  };
  [[nodiscard]] static std::optional<Settings> parseSettings(const QDomElement& element);

  // What one projection shows: the depth sample under one CameraInfo sample
  // (the calibration). {} is "none".
  struct ProjectionKey {
    SampleId sample;
    SampleId camera;
    bool operator==(const ProjectionKey&) const = default;
  };

  // Everything the worker needs, captured by value: the image's anchor keeps its
  // bytes alive and the task touches no layer state.
  struct BackprojectRequest {
    PJ::sdk::Image image;
    DepthIntrinsics intrinsics;
    ProjectionKey key;
    float min_depth_m = 0.0f;
    float max_depth_m = 0.0f;
  };
  struct BackprojectResult {
    ProjectionKey key;
    QByteArray vertices;          // PointVertex records, padded (PJ::padForUpload()); a fresh array per result
    std::size_t point_count = 0;  // records in vertices
    AABB bounds;
    std::pair<float, float> scalar_range{0.0f, 1.0f};
    QString error;  // non-empty: the sample renders nothing and warns
  };
  // The QRhi sink's worker job (the GL build projects inline in renderAt).
  [[nodiscard]] static BackprojectResult backproject(const BackprojectRequest& request);
  void onBackprojectFinished(BackprojectResult result);
  // kSceneLimits admission of a depth image before any decode; empty when it fits.
  [[nodiscard]] static QString admissionError(const PJ::sdk::Image& image);
  // Drops queued/running back-projections and the pushed-sample memo, so the
  // next drain re-projects (a depth-range edit, a hide).
  void invalidateSample();
  // SessionManager::datasetAboutToBeReplaced: the bytes under the stable topic id
  // are about to change, so every projection, the drawn cloud and the calibration
  // memo go, and the next paint projects the replacement.
  void onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id);
  // The CameraInfo sample of the memoized intrinsics ({} when none).
  [[nodiscard]] SampleId memoCamera() const {
    return intrinsics_cache_.has_value() ? intrinsics_cache_->camera_sample : SampleId{};
  }
  // Drops the drawn cloud (the pass's, or the retained vertices) and its bounds.
  void clearGeometry();
  // The source frame, or the fixed frame for a frame-less image.
  void updateRenderFrame();
  // Pushes the style state into the GL pass (a no-op for QRhi, whose view reads
  // the getters every frame).
  void syncPassStyle();

  void renderAt(int64_t time_ns);
  void refreshNow();
  // Project at the playhead when a tracker move is pending (once per painted
  // frame, by GL render() or prepareForRender()).
  void drainPendingUpdate();
  // budget_ candidates: a depth sample's UID under the current depth range, and
  // the one the playhead asks for.
  [[nodiscard]] std::uint64_t budgetKey(std::uint64_t sample_uid) const;
  [[nodiscard]] std::uint64_t budgetCandidate() const;
  void updateSourceFrame(const std::string& frame_id);

  // Turn a depth-encoded sdk::Image into a raw sdk::DepthImage the core can
  // back-project. Raw encodings (16UC1/32FC1) become a zero-copy view over the
  // image bytes; compressedDepth is PNG-decoded (via QImage) into `scratch` and
  // returned as a 16UC1 view over it. Returns nullopt for a non-depth encoding or
  // an undecodable payload. `scratch` MUST outlive use of the returned view.
  // `error`, when given, receives the reason of a nullopt. A PNG/JPEG container
  // whose header declares more pixels than kSceneLimits allows is refused before
  // it is inflated. Thread-safe (no layer state).
  static std::optional<PJ::sdk::DepthImage> toDepthView(
      const PJ::sdk::Image& image, std::vector<uint8_t>& scratch, QString* error = nullptr);

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
  QMetaObject::Connection reload_connection_;

  std::string source_frame_;  // Image.frame_id (camera optical frame); "" when unknown
  QString fixed_frame_;       // dock's fixed frame; the placement fallback
  std::optional<PJ::Timepoint> decoded_at_ns_;
  std::optional<int64_t> ts_first_;
  bool tracker_dirty_ = false;
  bool visible_ = true;
  std::optional<AABB> world_bounds_;

  PJ::Colormap colormap_ = PJ::Colormap::kTurbo;
  float point_size_px_ = 2.0f;
  float min_depth_m_ = 0.0f;
  float max_depth_m_ = 0.0f;

  ProjectionKey pushed_;  // drawn (or memoized as rejected)
  // What the playhead wants shown: a projection finishing with any other key is
  // obsolete and dropped (QRhi; the GL build projects inline).
  ProjectionKey wanted_;
#ifndef PJ_SCENE3D_RHI
  std::size_t last_point_count_ = 0;  // points of the cloud handed to the GL pass
#endif
  std::optional<IntrinsicsCache> intrinsics_cache_;

  // QRhi sink (empty in the GL build).
  QByteArray vertex_bytes_;      // PointVertex records: finite back-projected points, camera frame
  std::size_t point_count_ = 0;  // records in vertex_bytes_ (the rest is padding)
  std::uint64_t geometry_revision_ = 0;
  std::pair<float, float> scalar_range_{0.0f, 1.0f};
  std::string render_frame_;
  // Point count and candidate of a cloud dropped for the view budget.
  BudgetLatch budget_;
#ifndef PJ_SCENE3D_RHI
  PointcloudRenderPass cloud_pass_;
#endif

  LatestWinsWorker<BackprojectRequest, BackprojectResult> backproject_worker_{
      this, &DepthCloudLayer::backproject,
      [this](BackprojectResult result) { onBackprojectFinished(std::move(result)); }};
};

}  // namespace pj::scene3d
