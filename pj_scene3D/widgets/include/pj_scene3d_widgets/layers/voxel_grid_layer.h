// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QMetaObject>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/voxel_grid.hpp"
#include "pj_datastore/object_store.hpp"  // PJ::ObjectTopicId, PJ::SequentialUID
#include "pj_scene3d_core/scene_limits.h"
#include "pj_scene3d_core/voxel_grid_value.h"
#include "pj_scene3d_core/voxel_grid_view.h"
#include "pj_scene3d_widgets/scene3d_layer.h"
#include "pj_widgets/Colormap.h"

class QWidget;

namespace pj::scene3d {

// Scene3DLayer for an sdk::VoxelGrid topic. On each tracker tick it fetches the
// grid valid at that time and packs the selected field into a dense volume (ONCE
// per new grid / field change — re-scrubbing a seen grid does zero per-voxel CPU
// work) and retains it for SceneViewWidget (prepareForRender(),
// scalarVolume()/rgbaVolume()), which expands it to GPU-instanced cubes. The draw
// predicate, colormap, range and opacity are viewer-side display settings
// persisted in the layout XML.
class VoxelGridLayer : public Scene3DLayer {
  Q_OBJECT
 public:
  VoxelGridLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent = nullptr);
  ~VoxelGridLayer() override;

  [[nodiscard]] PJ::SceneLayerInfo info() const override;
  [[nodiscard]] PJ::Range<PJ::Timepoint> timeRange() const override;
  [[nodiscard]] QStringList fallbackFrames() const override;
  [[nodiscard]] SceneContributions sceneContributions() override {
    return {.voxels = this};
  }
  [[nodiscard]] QString sourceFrame() const override;
  QDomElement xmlSaveState(QDomDocument& doc) const override;
  bool xmlLoadState(const QDomElement& element) override;

  bool attach(const PJ::SceneLayerContext& ctx) override;
  void detach() override;

  void setFixedFrame(const QString& frame) override;
  void setTrackerTime(PJ::Timepoint time) override;
  void setVisible(bool visible) override;

  // Coalesces repaints to sample changes.
  [[nodiscard]] std::uint64_t renderKey(PJ::Timepoint time) const override;

  // QRhi view hook, called once per frame for a visible layer: drains a pending
  // tracker move or field change and applies the view's voxel budget before any
  // upload, so a rejected layer retains no GPU allocation and retries once the
  // budget frees up. Returns true when the volume or the warning changed.
  bool prepareForRender(std::uint64_t remaining_view_voxels);
  // A deterministic capability rejection cannot succeed on a later frame in the
  // same context, so besides the renderer warning it releases the packed volume.
  void rejectVolumeForRender(QString warning);
  [[nodiscard]] std::optional<AABB> worldBounds() const override {
    return sourceBounds();
  }

  // The packed volume of the current grid. textureRevision() bumps on every change.
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
  [[nodiscard]] bool hasVolume() const {
    return voxelCount() != 0U && (value_kind_ == VoxelValueKind::kScalar ? scalar_volume_.size() == voxelCount()
                                                                         : rgba_volume_.size() == voxelCount() * 4U);
  }
  [[nodiscard]] std::uint64_t textureRevision() const {
    return texture_revision_;
  }
  [[nodiscard]] const std::string& renderFrameId() const override {
    return source_frame_;
  }
  [[nodiscard]] std::optional<PJ::Timepoint> renderStamp() const override {
    return render_stamp_;
  }
  [[nodiscard]] std::optional<AABB> sourceBounds() const {
    return source_bounds_.valid ? std::optional<AABB>{source_bounds_} : std::nullopt;
  }
  [[nodiscard]] bool visible() const {
    return visible_;
  }
  [[nodiscard]] VoxelDrawMode drawMode() const {
    return draw_mode_;
  }
  [[nodiscard]] float threshold() const {
    return static_cast<float>(threshold_);
  }
  [[nodiscard]] float rangeMinimum() const {
    return static_cast<float>(manual_lo_);
  }
  [[nodiscard]] float rangeMaximum() const {
    return static_cast<float>(manual_hi_);
  }
  // The colormap range: the scalar volume's finite min/max, or the manual one.
  [[nodiscard]] std::pair<float, float> colorRange() const {
    return auto_range_ ? automatic_range_ : std::pair<float, float>{rangeMinimum(), rangeMaximum()};
  }
  [[nodiscard]] PJ::Colormap colormap() const {
    return colormap_;
  }
  [[nodiscard]] float opacity() const {
    return static_cast<float>(opacity_);
  }
  // Validates a <voxel_grid> payload without a layer (layout pre-flight).
  [[nodiscard]] static bool validateXml(const QDomElement& element);

  QWidget* createConfigWidget(QWidget* parent) override;

  // Display params, driven by the config widget. Field selection re-packs on the
  // next render; the rest are uniform-only (no re-pack).
  void setActiveField(const QString& field_name);  // empty = auto-pick
  void setDrawMode(VoxelDrawMode mode);
  void setThreshold(double threshold);
  void setAutoRange(bool on);
  void setManualRange(double lo, double hi);
  void setColormap(PJ::Colormap colormap);
  void setOpacity(double opacity);

#ifdef PJ_SCENE3D_TEST_HOOKS
  void renderAtForTest(int64_t time_ns) {
    updateAt(time_ns);
  }
  [[nodiscard]] QString resolvedFieldForTest() const {
    return QString::fromStdString(resolved_field_name_);
  }
  [[nodiscard]] bool hasGridForTest() const {
    return hasVolume();
  }
  [[nodiscard]] bool passHasStagedGridForTest() const {
    return hasVolume();
  }
#endif

 private:
  struct ParsedSettings {
    std::string field;
    VoxelDrawMode draw_mode = VoxelDrawMode::kNonZero;
    double threshold = 0.0;
    bool auto_range = true;
    double range_lo = 0.0;
    double range_hi = 1.0;
    PJ::Colormap colormap = PJ::Colormap::kTurbo;
    double opacity = 1.0;
  };
  [[nodiscard]] static std::optional<ParsedSettings> parseSettings(const QDomElement& element);
  // Decode the first sample at attach to learn the source frame + default field.
  bool bootstrap();
  // Fetch the grid at time_ns and pack the selected field into the volume. No-ops
  // cheaply when the same
  // grid+field is already packed (the scrub path). A grid that cannot be shown
  // fails closed: no volume plus a statusWarning(). Returns true when the volume
  // or the warning changed.
  bool updateAt(int64_t time_ns);
  // Drop the parse memo + upload bookkeeping; called on attach/detach (a dataset
  // replace mints fresh UIDs, so a stale grid must never be served).
  void resetStreamingState();
  // Re-pack at requested_time_ when a tracker move or field change is pending
  // (once per painted frame, by prepareForRender()).
  bool drainPendingUpdate();
  // Resolve which field to display for `grid`, honouring active_field_name_ and
  // falling back to chooseDefaultField. Updates resolved_field_name_ + value_kind_.
  const PJ::sdk::PointField* resolveField(const PJ::sdk::VoxelGrid& grid);
  void populateFields(const PJ::sdk::VoxelGrid& grid);
  // Drop the packed volume (bumping textureRevision()).
  void clearVolume();
  void updateSourceFrame(const std::string& frame);
  void onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id);

  PJ::ObjectTopicId topic_id_;
  QString display_name_;
  Scene3DLayerContext ctx_;
  QMetaObject::Connection reload_connection_;

  // What is currently packed: the grid UID and the field *setting*
  // (active_field_name_) that produced it. A tracker move that resolves to the
  // same pair re-packs and re-uploads nothing (req: zero per-voxel CPU work on
  // re-scrub). Reset to invalid to force a re-pack (e.g. on field change).
  PJ::SequentialUID uploaded_uid_{};
  std::string uploaded_field_setting_{"\x01"};  // sentinel != any real "" / name

  std::string source_frame_;
  std::optional<PJ::Timepoint> render_stamp_;  // objectStamp() of the staged sample
  std::string fixed_frame_;                    // the dock's; renderKey folds fixed<-source
  // Set by setTrackerTime / a field change; drained once per painted frame.
  bool tracker_dirty_ = false;
  std::optional<PJ::Timepoint> requested_time_;
  bool visible_ = true;
  // Voxel count and sample that last overflowed the view budget (QRhi).
  BudgetLatch budget_;

  // The packed volume + its placement.
  std::vector<float> scalar_volume_;
  std::vector<std::uint8_t> rgba_volume_;
  PJ::sdk::Pose origin_;
  glm::vec3 cell_size_{1.0f};
  std::uint32_t columns_ = 0;
  std::uint32_t rows_ = 0;
  std::uint32_t slices_ = 0;
  AABB source_bounds_;
  std::pair<float, float> automatic_range_{0.0f, 1.0f};
  std::uint64_t texture_revision_ = 0;
  // Field names of the last decoded grid (config-widget selector).
  QStringList available_fields_;

  // Display settings (persisted). active_field_name_ empty = auto-pick.
  std::string active_field_name_;
  std::string resolved_field_name_;  // concrete field last used (UI display only)
  VoxelValueKind value_kind_ = VoxelValueKind::kScalar;
  VoxelDrawMode draw_mode_ = VoxelDrawMode::kNonZero;
  double threshold_ = 0.0;
  bool auto_range_ = true;
  double manual_lo_ = 0.0;
  double manual_hi_ = 1.0;
  PJ::Colormap colormap_ = PJ::Colormap::kTurbo;
  double opacity_ = 1.0;
};

}  // namespace pj::scene3d
