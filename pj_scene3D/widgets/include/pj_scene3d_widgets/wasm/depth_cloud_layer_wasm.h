#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDomDocument>
#include <QFutureWatcher>
#include <QMetaObject>
#include <QString>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pj_base/builtin/image.hpp"
#include "pj_base/time.hpp"
#include "pj_scene3d_core/depth_backproject.h"
#include "pj_scene3d_core/pointcloud_budget.h"
#include "pj_scene3d_widgets/wasm/point_renderable_wasm.h"
#include "pj_scene_common/scene_layer.h"

namespace PJ {
class SessionManager;
}

class QThread;

namespace pj::scene3d {

// Browser DepthCloud adapter. It preserves the native layer's kImage encoding
// gate, CameraInfo-by-frame join, depth filtering, point style, and XML schema,
// while doing PNG inflate/back-projection on Qt's WASM worker pool. QRhi resource
// ownership stays in SceneViewWidget through WasmPointRenderable.
class WasmDepthCloudLayer final : public PJ::ISceneLayer, public WasmPointRenderable {
  Q_OBJECT

 public:
  static constexpr std::uint64_t kMaxPixelsPerImage = kSceneLimits.max_points_per_cloud;
  static constexpr std::uint64_t kMaxWireBytesPerImage = kSceneLimits.max_point_wire_bytes_per_cloud;

  WasmDepthCloudLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent = nullptr);
  ~WasmDepthCloudLayer() override;

  [[nodiscard]] static bool isDepthEncoding(const std::string& encoding);
  [[nodiscard]] static bool validateXml(const QDomElement& element);

  [[nodiscard]] PJ::SceneLayerInfo info() const override;
  [[nodiscard]] PJ::Range<PJ::Timepoint> timeRange() const override;
  bool attach(const PJ::SceneLayerContext& context) override;
  void detach() override;
  void setTrackerTime(PJ::Timepoint time) override;
  [[nodiscard]] std::uint64_t renderKey(PJ::Timepoint time) const override;
  void setVisible(bool visible) override;
  void setFixedFrame(const QString& frame) override;
  QWidget* createConfigWidget(QWidget* parent) override;
  QDomElement xmlSaveState(QDomDocument& document) const override;
  bool xmlLoadState(const QDomElement& element) override;

  bool prepareForRender(std::uint64_t remaining_view_vertices) override;
  [[nodiscard]] const std::vector<WasmPointVertex>& vertices() const override {
    return vertices_;
  }
  [[nodiscard]] std::uint64_t geometryRevision() const override {
    return geometry_revision_;
  }
  [[nodiscard]] const std::string& sourceFrame() const override {
    return render_frame_;
  }
  [[nodiscard]] std::optional<AABB> sourceBounds() const override {
    return source_bounds_.valid ? std::optional<AABB>{source_bounds_} : std::nullopt;
  }
  [[nodiscard]] bool visible() const override {
    return visible_;
  }
  [[nodiscard]] WasmPointShape shape() const override {
    return WasmPointShape::kPoint;
  }
  [[nodiscard]] WasmPointColorType colorType() const override {
    return WasmPointColorType::kField;
  }
  [[nodiscard]] float sizeMeters() const override {
    return 0.02F;
  }
  [[nodiscard]] float sizePixels() const override {
    return point_size_px_;
  }
  [[nodiscard]] QColor solidColor() const override {
    return QColor(Qt::white);
  }
  [[nodiscard]] PJ::Colormap colormap() const override {
    return colormap_;
  }
  [[nodiscard]] bool invertLut() const override {
    return false;
  }
  [[nodiscard]] float outsideRangeAlpha() const override {
    return 1.0F;
  }
  [[nodiscard]] int scalarAxis() const override {
    return -1;
  }
  [[nodiscard]] std::pair<float, float> scalarRange(const glm::mat4&) const override {
    return scalar_range_;
  }
  void noteRenderedScalarRange(float, float) override {}
  [[nodiscard]] bool isDepthCloud() const override {
    return true;
  }

  [[nodiscard]] float minDepth() const {
    return min_depth_m_;
  }
  [[nodiscard]] float maxDepth() const {
    return max_depth_m_;
  }
  [[nodiscard]] QString warningReason() const {
    return warning_reason_;
  }
  [[nodiscard]] bool decodeInFlightForTest() const {
    return inflight_.has_value();
  }
  [[nodiscard]] std::uint64_t decodeStartsForTest() const {
    return decode_starts_;
  }
  [[nodiscard]] std::uint64_t decodeCompletionsForTest() const {
    return decode_completions_;
  }
  [[nodiscard]] std::uint64_t decodeOffMainCompletionsForTest() const {
    return decode_off_main_completions_;
  }
  [[nodiscard]] std::int64_t activeSampleStampForTest() const {
    return active_sample_.stamp;
  }
  [[nodiscard]] std::int64_t wantedSampleStampForTest() const {
    return wanted_sample_.stamp;
  }

 signals:
  void sourceFrameChanged(QString frame);

 private:
  struct SampleId {
    std::int64_t stamp = std::numeric_limits<std::int64_t>::min();
    std::size_t payload_size = 0;
    bool operator==(const SampleId&) const = default;
  };

  struct IntrinsicsCache {
    std::string frame_id;
    PJ::ObjectTopicId camera_topic;
    SampleId camera_sample;
    DepthIntrinsics intrinsics;
  };

  struct DecodeRequest {
    PJ::sdk::Image image;
    DepthIntrinsics intrinsics;
    SampleId sample;
    std::uint64_t generation = 0;
    float min_depth_m = 0.0F;
    float max_depth_m = 0.0F;
  };

  struct DecodeResult {
    SampleId sample;
    std::uint64_t generation = 0;
    std::vector<WasmPointVertex> vertices;
    AABB bounds;
    std::pair<float, float> scalar_range{0.0F, 1.0F};
    QString error;
    bool ran_off_main_thread = false;
  };

  struct ParsedSettings {
    PJ::Colormap colormap = PJ::Colormap::kTurbo;
    float point_size_px = 2.0F;
    float min_depth_m = 0.0F;
    float max_depth_m = 0.0F;
  };

  [[nodiscard]] static std::optional<ParsedSettings> parseSettings(const QDomElement& element);
  [[nodiscard]] static DecodeResult decode(DecodeRequest request, QThread* main_thread);
  bool bootstrap();
  bool decodeAt(PJ::Timepoint time);
  [[nodiscard]] DepthIntrinsics resolveIntrinsics(const std::string& frame_id, std::int64_t time_ns);
  void queueDecode(DecodeRequest request);
  void startDecode(DecodeRequest request);
  void onDecodeFinished();
  void invalidateDecodeState();
  void applyResult(DecodeResult result);
  void updateSourceFrame(const std::string& frame);
  void updateRenderFrame();
  void setWarning(QString warning);
  void clearGeometry();
  void requestDecode();
  void setColormap(PJ::Colormap colormap);
  void setPointSizePixels(float pixels);
  void setMinDepth(float metres);
  void setMaxDepth(float metres);
  void onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id);

  PJ::ObjectTopicId topic_id_;
  QString display_name_;
  PJ::SessionManager* session_ = nullptr;
  QMetaObject::Connection reload_connection_;
  QMetaObject::Connection samples_connection_;

  std::vector<WasmPointVertex> vertices_;
  std::string source_frame_;
  std::string render_frame_;
  std::string fixed_frame_;
  AABB source_bounds_;
  std::pair<float, float> scalar_range_{0.0F, 1.0F};
  std::uint64_t geometry_revision_ = 0;
  SampleId active_sample_;
  SampleId wanted_sample_;
  std::optional<PJ::Timepoint> requested_time_;
  std::optional<std::uint64_t> budget_rejected_vertex_count_;
  std::optional<IntrinsicsCache> intrinsics_cache_;
  bool decode_dirty_ = false;
  bool visible_ = true;
  QString warning_reason_;

  PJ::Colormap colormap_ = PJ::Colormap::kTurbo;
  float point_size_px_ = 2.0F;
  float min_depth_m_ = 0.0F;
  float max_depth_m_ = 0.0F;

  QFutureWatcher<DecodeResult>* decode_watcher_ = nullptr;
  std::optional<DecodeRequest> inflight_;
  std::optional<DecodeRequest> pending_;
  std::uint64_t decode_generation_ = 1;
  std::uint64_t decode_starts_ = 0;
  std::uint64_t decode_completions_ = 0;
  std::uint64_t decode_off_main_completions_ = 0;
};

}  // namespace pj::scene3d
