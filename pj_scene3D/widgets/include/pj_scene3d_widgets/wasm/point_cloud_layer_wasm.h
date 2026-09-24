#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QColor>
#include <QDomDocument>
#if defined(PJ_WASM_WITH_COMPRESSED_POINTCLOUDS)
#include <QFutureWatcher>
#endif
#include <QMetaObject>
#include <QString>
#include <QStringList>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#if defined(PJ_WASM_WITH_COMPRESSED_POINTCLOUDS)
#include "pj_base/builtin/compressed_point_cloud.hpp"
#endif
#include "pj_base/builtin/point_cloud.hpp"
#include "pj_base/time.hpp"
#include "pj_scene3d_core/pointcloud_budget.h"
#include "pj_scene3d_widgets/wasm/point_renderable_wasm.h"
#include "pj_scene_common/scene_layer.h"

namespace PJ {
class SessionManager;
}

namespace pj::scene3d {

// Canonical PointCloud adapter for the WebAssembly Scene3D renderer.
//
// The desktop PointCloudLayer and raw-OpenGL pass are deliberately untouched.
// This platform-selected adapter reuses resolveObject() and convertCanonical(),
// retains only interleaved source-frame vertices, and leaves every QRhi resource
// in SceneViewWidget. When the explicit codec package is enabled, Draco/Cloudini
// CompressedPointCloud samples decode on Qt's worker pool and enter the exact same
// conversion path as raw samples.
class WasmPointCloudLayer final : public PJ::ISceneLayer, public WasmPointRenderable {
  Q_OBJECT

 public:
  using Shape = WasmPointShape;
  using ColorType = WasmPointColorType;

  // A rejected cloud leaves the prior sample neither painted nor retained. The
  // two independent limits bound conversion peak memory under the app's 256 MiB
  // initial heap while still admitting ordinary 1M-point lidar frames.
  static constexpr std::uint64_t kMaxPointsPerCloud = kSceneLimits.max_points_per_cloud;
  static constexpr std::uint64_t kMaxWireBytesPerCloud = kSceneLimits.max_point_wire_bytes_per_cloud;

  WasmPointCloudLayer(
      PJ::ObjectTopicId topic_id, QString display_name, PJ::sdk::BuiltinObjectType object_type,
      QObject* parent = nullptr);
  ~WasmPointCloudLayer() override;

  [[nodiscard]] PJ::SceneLayerInfo info() const override;
  [[nodiscard]] PJ::Range<PJ::Timepoint> timeRange() const override;
  bool attach(const PJ::SceneLayerContext& context) override;
  void detach() override;
  void setTrackerTime(PJ::Timepoint time) override;
  [[nodiscard]] std::uint64_t renderKey(PJ::Timepoint time) const override;
  void setVisible(bool visible) override;
  QWidget* createConfigWidget(QWidget* parent) override;
  QDomElement xmlSaveState(QDomDocument& document) const override;
  bool xmlLoadState(const QDomElement& element) override;

  // Called from SceneViewWidget::render(), after Qt has coalesced tracker update
  // requests. Returns true when the retained geometry changed this frame.
  bool prepareForRender(std::uint64_t remaining_view_vertices) override;

  [[nodiscard]] const std::vector<WasmPointVertex>& vertices() const override {
    return vertices_;
  }
  [[nodiscard]] std::uint64_t geometryRevision() const override {
    return geometry_revision_;
  }
  [[nodiscard]] const std::string& sourceFrame() const override {
    return source_frame_;
  }
  [[nodiscard]] std::optional<AABB> sourceBounds() const override {
    return source_bounds_.valid ? std::optional<AABB>{source_bounds_} : std::nullopt;
  }
  [[nodiscard]] bool visible() const override {
    return visible_;
  }

  [[nodiscard]] Shape shape() const override {
    return shape_;
  }
  [[nodiscard]] ColorType colorType() const override {
    return color_type_;
  }
  [[nodiscard]] float sizeMeters() const override {
    return size_meters_;
  }
  [[nodiscard]] float sizePixels() const override {
    return size_pixels_;
  }
  [[nodiscard]] QColor solidColor() const override {
    return solid_color_;
  }
  [[nodiscard]] PJ::Colormap colormap() const override {
    return colormap_;
  }
  [[nodiscard]] bool invertLut() const override {
    return invert_lut_;
  }
  [[nodiscard]] float outsideRangeAlpha() const override {
    return outside_range_visible_ ? outside_range_opacity_ : 0.0F;
  }
  [[nodiscard]] int scalarAxis() const override;
  [[nodiscard]] std::pair<float, float> scalarRange(const glm::mat4& fixed_from_source) const override;
  // The view owns fixed<-source, so it reports the effective auto range after
  // applying TF. This lets the interactive auto->manual toggle freeze exactly
  // the colors currently on screen, matching the native layer.
  void noteRenderedScalarRange(float minimum, float maximum) override;
  [[nodiscard]] bool isDepthCloud() const override {
    return false;
  }
  [[nodiscard]] QString warningReason() const {
    return warning_reason_;
  }
  [[nodiscard]] QStringList availableColorFields() const {
    return available_color_fields_;
  }
  [[nodiscard]] QString colorField() const {
    return QString::fromStdString(color_field_);
  }
  [[nodiscard]] bool isCompressedForTest() const {
    return object_type_ == PJ::sdk::BuiltinObjectType::kCompressedPointCloud;
  }
  [[nodiscard]] bool compressedDecodeInFlightForTest() const {
#if defined(PJ_WASM_WITH_COMPRESSED_POINTCLOUDS)
    return inflight_ != SampleId{};
#else
    return false;
#endif
  }
  [[nodiscard]] std::uint64_t compressedDecodeStartsForTest() const {
#if defined(PJ_WASM_WITH_COMPRESSED_POINTCLOUDS)
    return decode_starts_;
#else
    return 0;
#endif
  }
  [[nodiscard]] std::uint64_t compressedDecodeCompletionsForTest() const {
#if defined(PJ_WASM_WITH_COMPRESSED_POINTCLOUDS)
    return decode_completions_;
#else
    return 0;
#endif
  }
  [[nodiscard]] std::uint64_t compressedDecodeOffMainCompletionsForTest() const {
#if defined(PJ_WASM_WITH_COMPRESSED_POINTCLOUDS)
    return decode_off_main_completions_;
#else
    return 0;
#endif
  }
  [[nodiscard]] std::int64_t activeSampleStampForTest() const {
    return active_sample_.stamp;
  }
  [[nodiscard]] std::int64_t wantedCompressedSampleStampForTest() const {
#if defined(PJ_WASM_WITH_COMPRESSED_POINTCLOUDS)
    return wanted_.stamp;
#else
    return std::numeric_limits<std::int64_t>::min();
#endif
  }

  // Full validation without mutation, used by the dock's two-phase workspace
  // restore before it clears any live layers.
  [[nodiscard]] static bool validateXml(const QDomElement& element);

 signals:
  void sourceFrameChanged(QString frame);
  void colorFieldsChanged(QStringList fields);
  void autoRangeComputed(float minimum, float maximum);

 private:
  struct SampleId {
    std::int64_t stamp = std::numeric_limits<std::int64_t>::min();
    std::size_t payload_size = 0;
    bool operator==(const SampleId&) const = default;
  };

#if defined(PJ_WASM_WITH_COMPRESSED_POINTCLOUDS)
  struct DecodeResult {
    SampleId id;
    std::uint64_t generation = 0;
    std::shared_ptr<PJ::sdk::PointCloud> cloud;
    QString error;
    bool ran_off_main_thread = false;
  };

  struct PendingDecode {
    PJ::sdk::CompressedPointCloud cloud;
    SampleId id;
    std::uint64_t generation = 0;
  };
#endif

  struct ParsedSettings {
    Shape shape = Shape::kSphere;
    float size_meters = 0.02F;
    float size_pixels = 2.0F;
    ColorType color_type = ColorType::kField;
    bool color_choice_explicit = false;
    std::string color_field;
    QColor solid_color{255, 0, 0};
    PJ::Colormap colormap = PJ::Colormap::kTurbo;
    bool auto_range = true;
    bool invert_lut = false;
    float outside_range_opacity = 1.0F;
    bool outside_range_visible = true;
    float range_min = 0.0F;
    float range_max = 1.0F;
  };

  [[nodiscard]] static std::optional<ParsedSettings> parseSettings(const QDomElement& element);
  bool bootstrap();
  bool decodeAt(PJ::Timepoint time);
  bool pushCloud(const PJ::sdk::PointCloud& cloud, SampleId sample);
#if defined(PJ_WASM_WITH_COMPRESSED_POINTCLOUDS)
  void ensureDecodeWorker();
  void requestCompressedDecode(const PJ::sdk::CompressedPointCloud& cloud, SampleId sample);
  void startCompressedDecode(const PJ::sdk::CompressedPointCloud& cloud, SampleId sample, std::uint64_t generation);
  void onCompressedDecodeFinished();
  void invalidateCompressedState();
#endif
  void populateColorFields(const PJ::sdk::PointCloud& cloud);
  void updateSourceFrame(const std::string& frame);
  void setWarning(QString warning);
  void clearGeometry();
  void requestDecode();
  void setShape(Shape shape);
  void setSizeMeters(float metres);
  void setSizePixels(float pixels);
  void setColorType(ColorType type);
  void setColorField(const QString& field);
  void setSolidColor(QColor color);
  void setColormap(PJ::Colormap colormap);
  void setInvertLut(bool invert);
  void setAutoRange(bool automatic);
  void setManualRange(float minimum, float maximum);
  void setOutsideRangeOpacity(float opacity);
  void setOutsideRangeVisible(bool visible);
  void onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id);

  PJ::ObjectTopicId topic_id_;
  QString display_name_;
  PJ::sdk::BuiltinObjectType object_type_ = PJ::sdk::BuiltinObjectType::kPointCloud;
  PJ::SessionManager* session_ = nullptr;
  QMetaObject::Connection reload_connection_;

  std::vector<WasmPointVertex> vertices_;
  std::string source_frame_;
  AABB source_bounds_;
  std::pair<float, float> automatic_scalar_range_{0.0F, 1.0F};
  std::pair<float, float> last_rendered_scalar_range_{0.0F, 1.0F};
  std::uint64_t geometry_revision_ = 0;
  SampleId active_sample_;
  std::optional<PJ::Timepoint> requested_time_;
  std::optional<std::uint64_t> budget_rejected_vertex_count_;
  bool decode_dirty_ = false;
  bool visible_ = true;
  QString warning_reason_;

  QStringList available_color_fields_;
  std::string color_field_;
  bool has_color_ = false;
  bool color_choice_explicit_ = false;
  Shape shape_ = Shape::kSphere;
  float size_meters_ = 0.02F;
  float size_pixels_ = 2.0F;
  ColorType color_type_ = ColorType::kField;
  QColor solid_color_{255, 0, 0};
  PJ::Colormap colormap_ = PJ::Colormap::kTurbo;
  bool invert_lut_ = false;
  bool auto_range_ = true;
  float manual_range_min_ = 0.0F;
  float manual_range_max_ = 1.0F;
  float outside_range_opacity_ = 1.0F;
  bool outside_range_visible_ = true;

#if defined(PJ_WASM_WITH_COMPRESSED_POINTCLOUDS)
  QFutureWatcher<DecodeResult>* decode_watcher_ = nullptr;
  SampleId inflight_;
  std::uint64_t inflight_generation_ = 0;
  std::optional<PendingDecode> pending_;
  std::shared_ptr<PJ::sdk::PointCloud> decoded_cache_;
  SampleId decoded_cache_id_;
  SampleId wanted_;
  SampleId failed_id_;
  QString failed_warning_;
  std::uint64_t decode_generation_ = 1;
  std::uint64_t decode_starts_ = 0;
  std::uint64_t decode_completions_ = 0;
  std::uint64_t decode_off_main_completions_ = 0;
#endif
};

}  // namespace pj::scene3d
