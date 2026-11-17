// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QMetaObject>
#include <QString>
#include <QStringList>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/occupancy_grid.hpp"
#include "pj_datastore/object_store.hpp"  // PJ::ObjectTopicId, PJ::SequentialUID
#include "pj_scene3d_core/occupancy_grid_reconstructor.h"
#include "pj_scene3d_core/scene_limits.h"
#include "pj_scene3d_widgets/layer_style.h"
#include "pj_scene3d_widgets/scene3d_layer.h"

class QWidget;

namespace pj::scene3d {

// Scene3DLayer for a nav_msgs/OccupancyGrid base topic plus its optional map_msgs/OccupancyGridUpdate sibling
// ("<base>_updates"). On every tracker tick it asks OccupancyGridReconstructor for the grid as displayed at that time
// (base keyframe + applicable deltas, correct under back-and-forth scrubbing) and stages it as a texture upload plan,
// full or only the dirty rects of an incremental step, which the view reads through
// prepareForRender()/grid()/dirtyRects(). Every sample is admitted against kSceneLimits.
class OccupancyGridLayer : public Scene3DLayer {
  Q_OBJECT
 public:
  OccupancyGridLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent = nullptr);
  ~OccupancyGridLayer() override;

  [[nodiscard]] PJ::SceneLayerInfo info() const override;
  [[nodiscard]] PJ::Range<PJ::Timepoint> timeRange() const override;
  [[nodiscard]] QStringList fallbackFrames() const override;
  [[nodiscard]] SceneContributions sceneContributions() override {
    return {.occupancy = this};
  }
  [[nodiscard]] QString sourceFrame() const override;
  QDomElement xmlSaveState(QDomDocument& doc) const override;
  bool xmlLoadState(const QDomElement& element) override;

  bool attach(const PJ::SceneLayerContext& ctx) override;
  void detach() override;

  void setFixedFrame(const QString& frame) override;
  void setTrackerTime(PJ::Timepoint time) override;
  void setVisible(bool visible) override;

  [[nodiscard]] std::uint64_t renderKey(PJ::Timepoint time) const override;

  // QRhi view hook, once per frame for a visible layer: drains a pending tracker move and applies the view's cell
  // budget. A grid over the remaining budget is dropped with a warning and retried once the budget frees up. True when
  // the staged upload or the warning changed.
  bool prepareForRender(std::uint64_t remaining_view_cells);
  [[nodiscard]] std::optional<AABB> worldBounds() const override;

  // Upload plan of the staged grid: textureRevision() bumps by one on every change, dirtyRects() are the cells it made.
  [[nodiscard]] const ReconstructedGrid& grid() const {
    return reconstructor_.grid();
  }
  [[nodiscard]] const std::vector<CellRect>& dirtyRects() const {
    return dirty_rects_;
  }
  // Whether a texture holding `uploaded_revision` catches up with dirtyRects() alone: only when it holds exactly the
  // revision before textureRevision() and the latest change was a patch. Any other state (a change it skipped, such as
  // a frame the layer was not drawn, or a new epoch or backward seek, even with a patch staged after it) needs the
  // whole grid.
  [[nodiscard]] bool dirtyRectsCatchUp(std::uint64_t uploaded_revision) const {
    return !full_upload_pending_ && uploaded_revision + 1U == texture_revision_;
  }
  [[nodiscard]] std::uint64_t textureRevision() const {
    return texture_revision_;
  }
  [[nodiscard]] const std::string& renderFrameId() const override {
    return source_frame_;
  }
  [[nodiscard]] bool visible() const {
    return visible_;
  }
  [[nodiscard]] static bool validateXml(const QDomElement& element);

  QWidget* createConfigWidget(QWidget* parent) override;

  void setColorScheme(OccupancyColorScheme scheme);
  void setOpacity(float opacity);
  [[nodiscard]] OccupancyColorScheme colorScheme() const {
    return color_scheme_;
  }
  [[nodiscard]] float opacity() const {
    return opacity_;
  }

#ifdef PJ_SCENE3D_TEST_HOOKS
  void renderAtForTest(int64_t time_ns) {
    updateAt(time_ns);
  }
  // The grid as last reconstructed by updateAt; the reference is valid until the next updateAt.
  [[nodiscard]] const ReconstructedGrid& reconstructedGridForTest() const {
    return reconstructor_.grid();
  }
  // Count of retroactive-ingest invalidations issued by updateAt: a test asserts the cursor settles (the count stops
  // rising) instead of re-invalidating a late out-of-order update every frame.
  [[nodiscard]] size_t retroactiveInvalidateCountForTest() const {
    return retroactive_invalidate_count_;
  }
#endif

 private:
  struct ParsedSettings {
    OccupancyColorScheme color_scheme = OccupancyColorScheme::kMap;
    float opacity = 0.7f;
  };
  [[nodiscard]] static std::optional<ParsedSettings> parseSettings(const QDomElement& element);
  // Decodes the first base sample at attach to learn the source frame and time range before render.
  bool bootstrap();
  // Reconstructs the grid at time_ns and stages its upload plan. An undecodable base or an over-limit sample fails
  // closed: no grid plus a statusWarning(). True when the staged plan or the warning changed.
  bool updateAt(int64_t time_ns);
  void stageGrid(const GridUpdate& update);
  void stageEmpty();
  void updateSourceFrame(const std::string& frame);
  bool drainPendingUpdate();
  void onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id);
  // Drops the base-keyframe memo, the updates-topic cursor and all reconstructor state. Called on attach/detach: the
  // previous attachment's epoch identity and snapshots embed a timeline that may no longer exist (dataset replace, new
  // session); same shape as SceneEntitiesLayer::resetReplayState().
  void resetStreamingState();

  PJ::ObjectTopicId topic_id_;
  QString display_name_;
  Scene3DLayerContext ctx_;
  QMetaObject::Connection reload_connection_;
  // Parsers are deliberately NOT cached: every decode resolves a fresh ParserBinding through ctx_.session (see
  // resolveObject()), so a file reload that re-registers the topic's parser slot cannot leave a dangling one.
  std::optional<PJ::ObjectTopicId> updates_topic_;

  // Memo of the last parsed base keyframe, keyed by its store entry's SequentialUID (stable across front-eviction,
  // minted fresh by dataset replace/flush, so a reload never serves a stale grid). The copy's anchor keeps the decoded
  // cell bytes alive. Without it, playback over a static map re-parses and deep-copies the full cell payload every
  // tick.
  PJ::SequentialUID base_cache_uid_{};
  std::optional<PJ::sdk::OccupancyGrid> base_cache_;

  // Retroactive-ingest detector for the '_updates' sibling. The live drive follows the fastest topic's edge, so the
  // reconstructor's forward (last_t_, t] window can advance past the updates topic's ingest, and an update landing
  // later with ts <= the consumed time would be skipped forever. updates_cursor_ is the highest updates-topic UID
  // accounted for at the last updateAt; last_consumed_time_ is the high-water reconstruction time. Both only grow
  // between resets: updateAt() invalidates the reconstructor when an entry appears past the cursor with ts at or before
  // the high-water.
  PJ::SequentialUID updates_cursor_{};
  std::optional<PJ::Timestamp> last_consumed_time_;
  size_t retroactive_invalidate_count_ = 0;
  // A skipped update's soft warning, kept until the tracker moves before that update or a later base keyframe
  // supersedes it: a repeated request at the same time has an empty incremental window and could not rediscover the
  // skip.
  std::optional<PJ::Timestamp> skipped_update_warning_timestamp_;
  QString skipped_update_warning_;

  std::string source_frame_;
  std::string fixed_frame_;  // the dock's; renderKey folds fixed<-source
  // Set by setTrackerTime, consumed once per painted frame (drainPendingUpdate): the reconstruction (base parse +
  // update folds + texture upload) is deferred, and Qt coalesces repaints, so a fast scrub reconstructs only the final
  // grid.
  bool tracker_dirty_ = false;
  std::optional<PJ::Timepoint> requested_time_;
  bool visible_ = true;

  OccupancyColorScheme color_scheme_ = OccupancyColorScheme::kMap;
  float opacity_ = 0.7f;

  std::vector<CellRect> dirty_rects_;
  std::uint64_t texture_revision_ = 0;
  bool full_upload_pending_ = true;
  bool grid_staged_ = false;

  OccupancyGridReconstructor reconstructor_{kSceneLimits.occupancy_snapshot_budget_bytes};
  BudgetLatch budget_;
};

}  // namespace pj::scene3d
