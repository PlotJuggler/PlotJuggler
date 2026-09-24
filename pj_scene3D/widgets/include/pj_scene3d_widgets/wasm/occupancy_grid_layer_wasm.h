// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QDomDocument>
#include <QMetaObject>
#include <QString>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/occupancy_grid.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_scene3d_core/occupancy_grid_budget.h"
#include "pj_scene3d_core/occupancy_grid_reconstructor.h"
#include "pj_scene_common/scene_layer.h"

namespace PJ {
class SessionManager;
}

namespace pj::scene3d {

// Emscripten-only OccupancyGrid adapter. Timeline reconstruction is shared with
// desktop; this class only adapts ObjectStore decode, browser budgets, UI/XML,
// and a retained upload plan consumed by SceneViewWidget's QRhi texture path.
class WasmOccupancyGridLayer final : public PJ::ISceneLayer {
  Q_OBJECT

 public:
  enum class ColorScheme : std::int32_t { kMap, kCostmap };

  static constexpr std::uint64_t kMaxCellsPerLayer = kSceneLimits.max_occupancy_cells_per_layer;
  static constexpr std::uint64_t kMaxWireBytesPerSample = kSceneLimits.max_occupancy_wire_bytes;
  static constexpr std::uint64_t kMaxUpdateWindowBytes = kSceneLimits.max_occupancy_update_window_bytes;

  WasmOccupancyGridLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent = nullptr);
  ~WasmOccupancyGridLayer() override;

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

  bool prepareForRender(std::uint64_t remaining_view_cells);

  [[nodiscard]] const ReconstructedGrid& grid() const {
    return reconstructor_.grid();
  }
  [[nodiscard]] const std::vector<CellRect>& dirtyRects() const {
    return dirty_rects_;
  }
  [[nodiscard]] bool fullUploadPending() const {
    return full_upload_pending_;
  }
  [[nodiscard]] std::uint64_t textureRevision() const {
    return texture_revision_;
  }
  [[nodiscard]] const std::string& sourceFrame() const {
    return source_frame_;
  }
  [[nodiscard]] bool visible() const {
    return visible_;
  }
  [[nodiscard]] ColorScheme colorScheme() const {
    return color_scheme_;
  }
  [[nodiscard]] float opacity() const {
    return opacity_;
  }
  [[nodiscard]] QString warningReason() const {
    return warning_reason_;
  }
  [[nodiscard]] std::optional<PJ::ObjectTopicId> updatesTopic() const {
    return updates_topic_;
  }

  void setColorScheme(ColorScheme scheme);
  void setOpacity(float opacity);
  void noteRenderFailure(QString warning);
  void noteRenderSuccess();

  [[nodiscard]] static bool validateXml(const QDomElement& element);

 signals:
  void sourceFrameChanged(QString frame);

 private:
  struct ParsedSettings {
    ColorScheme color_scheme = ColorScheme::kMap;
    float opacity = 0.7F;
  };

  [[nodiscard]] static std::optional<ParsedSettings> parseSettings(const QDomElement& element);
  bool bootstrap();
  bool reconstructAt(PJ::Timepoint time);
  void resetStreamingState();
  void stageGrid(const GridUpdate& update);
  void stageEmpty();
  void updateSourceFrame(const std::string& frame);
  void updateWarning();
  void setDataWarning(QString warning);
  void onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id);

  PJ::ObjectTopicId topic_id_;
  QString display_name_;
  PJ::SessionManager* session_ = nullptr;
  QMetaObject::Connection reload_connection_;
  std::optional<PJ::ObjectTopicId> updates_topic_;

  PJ::SequentialUID base_cache_uid_{};
  std::optional<PJ::sdk::OccupancyGrid> base_cache_;
  PJ::SequentialUID updates_cursor_{};
  std::optional<PJ::Timestamp> last_consumed_time_;
  std::optional<PJ::Timestamp> skipped_update_warning_timestamp_;
  QString skipped_update_warning_;

  OccupancyGridReconstructor reconstructor_{kSceneLimits.occupancy_snapshot_budget_bytes};
  std::vector<CellRect> dirty_rects_;
  std::string source_frame_;
  std::uint64_t texture_revision_ = 0;
  std::optional<std::uint64_t> budget_rejected_cell_count_;
  std::optional<PJ::Timepoint> requested_time_;
  bool full_upload_pending_ = true;
  bool grid_staged_ = false;
  bool decode_dirty_ = false;
  bool visible_ = true;

  ColorScheme color_scheme_ = ColorScheme::kMap;
  float opacity_ = 0.7F;
  QString data_warning_;
  QString render_warning_;
  QString warning_reason_;
};

}  // namespace pj::scene3d
