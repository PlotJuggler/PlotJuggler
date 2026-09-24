// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QDomDocument>
#include <QMetaObject>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/voxel_grid.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_scene3d_core/voxel_grid_budget.h"
#include "pj_scene3d_core/voxel_grid_value.h"
#include "pj_scene3d_core/voxel_grid_view.h"
#include "pj_scene_common/scene_layer.h"
#include "pj_widgets/Colormap.h"

namespace PJ {
class SessionManager;
}

namespace pj::scene3d {

// Emscripten-only VoxelGrid adapter. It reuses the platform-neutral field
// selection, dense packing, bounds, and draw semantics while SceneViewWidget
// owns the QRhi 3D texture and instanced-cube pipeline. Desktop's layer and
// OpenGL render pass are not compiled through this path.
class WasmVoxelGridLayer final : public PJ::ISceneLayer {
  Q_OBJECT

 public:
  static constexpr std::uint64_t kMaxVoxelsPerLayer = kSceneLimits.max_voxels_per_layer;
  static constexpr std::uint64_t kMaxWireBytesPerSample = kSceneLimits.max_voxel_wire_bytes;

  WasmVoxelGridLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent = nullptr);
  ~WasmVoxelGridLayer() override;

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

  // Resolves and packs the sample requested by the latest tracker tick. The
  // view budget is passed before any upload so rejected layers retain no GPU
  // allocation and can retry when earlier layers disappear.
  bool prepareForRender(std::uint64_t remaining_view_voxels);

  [[nodiscard]] const std::vector<float>& scalarVolume() const {
    return scalar_volume_;
  }
  [[nodiscard]] const std::vector<std::uint8_t>& rgbaVolume() const {
    return rgba_volume_;
  }
  [[nodiscard]] VoxelValueKind valueKind() const {
    return value_kind_;
  }
  [[nodiscard]] const PJ::sdk::Pose& origin() const {
    return origin_;
  }
  [[nodiscard]] glm::vec3 cellSize() const {
    return cell_size_;
  }
  [[nodiscard]] std::uint32_t columnCount() const {
    return columns_;
  }
  [[nodiscard]] std::uint32_t rowCount() const {
    return rows_;
  }
  [[nodiscard]] std::uint32_t sliceCount() const {
    return slices_;
  }
  [[nodiscard]] std::uint64_t voxelCount() const {
    return static_cast<std::uint64_t>(columns_) * rows_ * slices_;
  }
  [[nodiscard]] std::uint64_t textureRevision() const {
    return texture_revision_;
  }
  [[nodiscard]] const std::string& sourceFrame() const {
    return source_frame_;
  }
  [[nodiscard]] std::optional<AABB> sourceBounds() const {
    return source_bounds_.valid ? std::optional<AABB>{source_bounds_} : std::nullopt;
  }
  [[nodiscard]] bool visible() const {
    return visible_;
  }
  [[nodiscard]] bool hasVolume() const {
    return voxelCount() != 0U && (value_kind_ == VoxelValueKind::kScalar ? scalar_volume_.size() == voxelCount()
                                                                         : rgba_volume_.size() == voxelCount() * 4U);
  }
  [[nodiscard]] VoxelDrawMode drawMode() const {
    return draw_mode_;
  }
  [[nodiscard]] float threshold() const {
    return threshold_;
  }
  [[nodiscard]] std::pair<float, float> colorRange() const {
    return auto_range_ ? automatic_range_ : std::pair<float, float>{manual_range_lo_, manual_range_hi_};
  }
  [[nodiscard]] float rangeMinimum() const {
    return manual_range_lo_;
  }
  [[nodiscard]] float rangeMaximum() const {
    return manual_range_hi_;
  }
  [[nodiscard]] bool autoRange() const {
    return auto_range_;
  }
  [[nodiscard]] PJ::Colormap colormap() const {
    return colormap_;
  }
  [[nodiscard]] float opacity() const {
    return opacity_;
  }
  [[nodiscard]] QString activeField() const {
    return QString::fromStdString(active_field_name_);
  }
  [[nodiscard]] QString resolvedField() const {
    return QString::fromStdString(resolved_field_name_);
  }
  [[nodiscard]] QStringList availableFields() const {
    return available_fields_;
  }
  [[nodiscard]] QString warningReason() const {
    return warning_reason_;
  }

  // GPU allocation failures are combined with data warnings on the standard
  // layer-row surface. A later successful allocation clears only the GPU part.
  void noteRenderFailure(QString warning);
  // A deterministic capability rejection cannot succeed on a later frame in
  // the same browser context, so it also releases the packed CPU volume.
  void rejectVolumeForRender(QString warning);
  void noteRenderSuccess();

  [[nodiscard]] static bool validateXml(const QDomElement& element);

 signals:
  void sourceFrameChanged(QString frame);

 private:
  struct ParsedSettings {
    std::string field;
    VoxelDrawMode draw_mode = VoxelDrawMode::kNonZero;
    float threshold = 0.0F;
    bool auto_range = true;
    float range_lo = 0.0F;
    float range_hi = 1.0F;
    PJ::Colormap colormap = PJ::Colormap::kTurbo;
    float opacity = 1.0F;
  };

  [[nodiscard]] static std::optional<ParsedSettings> parseSettings(const QDomElement& element);
  bool bootstrap();
  bool decodeAt(PJ::Timepoint time);
  const PJ::sdk::PointField* resolveField(const PJ::sdk::VoxelGrid& grid);
  void populateFields(const PJ::sdk::VoxelGrid& grid);
  void clearVolume();
  void resetStreamingState();
  void updateSourceFrame(const std::string& frame);
  void updateWarning();
  void setDataWarning(QString warning);
  void setActiveField(const QString& field);
  void setDrawMode(VoxelDrawMode mode);
  void setThreshold(float threshold);
  void setAutoRange(bool automatic);
  void setManualRange(float minimum, float maximum);
  void setColormap(PJ::Colormap colormap);
  void setOpacity(float opacity);
  void onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id);

  PJ::ObjectTopicId topic_id_;
  QString display_name_;
  PJ::SessionManager* session_ = nullptr;
  QMetaObject::Connection reload_connection_;

  PJ::SequentialUID staged_uid_{};
  std::string staged_field_setting_{"\x01"};
  std::optional<PJ::Timepoint> requested_time_;
  std::optional<std::uint64_t> budget_rejected_voxel_count_;
  bool decode_dirty_ = false;
  bool visible_ = true;

  std::vector<float> scalar_volume_;
  std::vector<std::uint8_t> rgba_volume_;
  PJ::sdk::Pose origin_;
  glm::vec3 cell_size_{1.0F};
  std::uint32_t columns_ = 0;
  std::uint32_t rows_ = 0;
  std::uint32_t slices_ = 0;
  std::uint64_t texture_revision_ = 0;
  std::string source_frame_;
  AABB source_bounds_;
  QStringList available_fields_;
  std::string active_field_name_;
  std::string resolved_field_name_;
  VoxelValueKind value_kind_ = VoxelValueKind::kScalar;
  std::pair<float, float> automatic_range_{0.0F, 1.0F};

  VoxelDrawMode draw_mode_ = VoxelDrawMode::kNonZero;
  float threshold_ = 0.0F;
  bool auto_range_ = true;
  float manual_range_lo_ = 0.0F;
  float manual_range_hi_ = 1.0F;
  PJ::Colormap colormap_ = PJ::Colormap::kTurbo;
  float opacity_ = 1.0F;
  QString data_warning_;
  QString render_warning_;
  QString warning_reason_;
};

}  // namespace pj::scene3d
