// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QMetaObject>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pj_base/builtin/mesh_map.hpp"
#include "pj_datastore/object_store.hpp"  // PJ::ObjectTopicId, PJ::SequentialUID
#include "pj_runtime/SessionManager.h"    // PJ::DatasetId
#include "pj_scene3d_core/mesh_map_view.h"
#include "pj_scene3d_widgets/scene3d_layer.h"
#include "pj_widgets/Colormap.h"

class QFormLayout;
class QWidget;

namespace pj::scene3d {

// Persisted `color_by` tokens beside a channel name: angle brackets keep them
// distinct from any real channel. An empty request means "auto-pick".
inline constexpr std::string_view kMeshMapColorVertex = "<vertex color>";
inline constexpr std::string_view kMeshMapColorSolid = "<solid>";

// Scene3DLayer for an sdk::MeshMap topic, paired by name with an optional
// `<topic>_updates` sibling carrying sdk::MeshMapUpdate samples (the same
// convention OccupancyGridLayer follows). At the tracker time the latest map at
// or before it is decoded once into a MeshMapState, the updates since that map
// are applied in time order, and the result is de-indexed for the view. Moving
// forward applies only the new updates; moving back, or an update arriving with
// an older stamp than one already applied, rebuilds from the map. The requested
// colour source is kept apart from the effective one so a persisted request is
// honoured again once its channel reappears. Placed at the tracker time (no
// renderStamp()): the drawn mesh is a map plus updates with no single stamp.
class MeshMapLayer : public Scene3DLayer {
  Q_OBJECT
 public:
  MeshMapLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent = nullptr);
  ~MeshMapLayer() override;

  [[nodiscard]] PJ::SceneLayerInfo info() const override;
  [[nodiscard]] PJ::Range<PJ::Timepoint> timeRange() const override;
  [[nodiscard]] QStringList fallbackFrames() const override;
  [[nodiscard]] SceneContributions sceneContributions() override {
    return {.mesh_map = this};
  }
  [[nodiscard]] QString sourceFrame() const override;
  [[nodiscard]] uint64_t renderKey(PJ::Timepoint time) const override;
  QDomElement xmlSaveState(QDomDocument& doc) const override;
  bool xmlLoadState(const QDomElement& element) override;

  bool attach(const PJ::SceneLayerContext& ctx) override;
  void detach() override;

  void setFixedFrame(const QString& frame) override;
  void setTrackerTime(PJ::Timepoint time) override;
  void setVisible(bool visible) override;

  [[nodiscard]] std::optional<AABB> worldBounds() const override;

  // QRhi view hook, called once per frame for a visible layer: rebuilds the mesh
  // at the last tracker time when a tracker move or selector change is pending.
  // The result is stagedMesh(); stagedRevision() bumps on every change, and the
  // upload's geometry_revision only when the positions changed.
  void prepareForRender();
  [[nodiscard]] const std::optional<MeshMapUpload>& stagedMesh() const {
    return staged_mesh_;
  }
  [[nodiscard]] std::uint64_t stagedRevision() const {
    return staged_revision_;
  }
  [[nodiscard]] const std::string& renderFrameId() const override {
    return source_frame_;
  }
  [[nodiscard]] PJ::Colormap colormap() const {
    return colormap_;
  }
  [[nodiscard]] bool autoRange() const {
    return auto_range_;
  }
  [[nodiscard]] std::pair<double, double> manualRange() const {
    return {manual_lo_, manual_hi_};
  }
  [[nodiscard]] bool lighting() const {
    return lighting_;
  }
  [[nodiscard]] double opacity() const {
    return opacity_;
  }
  [[nodiscard]] bool showLinks() const {
    return show_links_;
  }
  [[nodiscard]] bool visible() const {
    return visible_;
  }

  QWidget* createConfigWidget(QWidget* parent) override;

  // Display params. setColorBy re-colours on the next render; the rest are
  // uniform-only. Empty = auto; kMeshMapColorVertex / kMeshMapColorSolid.
  void setColorBy(const QString& source);
  void setColormap(PJ::Colormap colormap);
  void setAutoRange(bool on);
  void setManualRange(double lo, double hi);
  void setLighting(bool on);
  void setOpacity(double opacity);
  void setShowLinks(bool on);

#ifdef PJ_SCENE3D_TEST_HOOKS
  void renderAtForTest(int64_t time_ns) {
    renderAt(time_ns);
  }
  [[nodiscard]] QString resolvedColorByForTest() const {
    return QString::fromStdString(resolved_color_);
  }
  [[nodiscard]] const MeshMapUpload* stagedMeshForTest() const {
    return staged_mesh_.has_value() ? &*staged_mesh_ : nullptr;
  }
  [[nodiscard]] int rebuildCountForTest() const {
    return rebuild_count_;
  }
  [[nodiscard]] int appliedUpdateCountForTest() const {
    return applied_update_count_;
  }
  [[nodiscard]] bool hasUpdatesTopicForTest() const {
    return updates_topic_.has_value();
  }
#endif

 signals:
  // The channel list or the effective pick changed: the config panel's selector
  // rebuilds from color_sources_.
  void colorSourcesChanged();

 private:
  // Decode the first map at attach to learn the source frame + channel list.
  bool bootstrap();
  // Bring the state to `time_ns` and stage the upload. Cheap no-op when the map
  // and the latest update at that time are the ones already staged.
  void renderAt(int64_t time_ns);
  // (Re)decodes the map at `entry` into state_; false (warning set) when refused.
  bool loadBase(const PJ::ResolvedObjectEntry& entry);
  // Applies the updates with stamps in (from, to], in time order. Returns the
  // number applied; sets a soft warning for skipped ones.
  int applyUpdates(int64_t from, int64_t to);
  // Picks the effective colour source from the request and the state's channels.
  void resolveColorSource();
  void stage(bool geometry_changed);
  void clearStaged();
  void resetStreamingState();
  void onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id);
  void setBounds(std::optional<AABB> bounds);
  void buildRangeRows(QFormLayout* form, QWidget* parent);

  PJ::ObjectTopicId topic_id_;
  QString display_name_;
  Scene3DLayerContext ctx_;
  QMetaObject::Connection reload_connection_;
  std::optional<PJ::ObjectTopicId> updates_topic_;

  // The decoded map and what has been applied to it. state_ owns its arrays (no
  // anchor into a plugin DSO), so no binding keepalive is needed.
  std::optional<MeshMapState> state_;
  PJ::SequentialUID base_uid_{};
  int64_t base_timestamp_ = 0;
  int64_t applied_until_ = 0;          // updates with stamps <= this are in state_
  std::uint64_t applied_max_uid_ = 0;  // newest update UID at or before applied_until_ when applied
  std::uint64_t staged_update_key_ = 0;
  PJ::SequentialUID staged_base_uid_{};
  bool colors_dirty_ = true;
  std::uint64_t geometry_revision_ = 0;
  std::optional<AABB> bounds_;
  QString update_warning_;
  QString base_warning_;
  QString color_warning_;  // a requested colour source the map lacks

  std::string source_frame_;
  QString fixed_frame_;
  bool tracker_dirty_ = false;
  bool visible_ = true;
  int rebuild_count_ = 0;
  int applied_update_count_ = 0;

  std::string color_request_;
  std::string resolved_color_;              // a channel name, a kMeshMapColor* token, or empty (nothing to show)
  std::vector<std::string> color_sources_;  // selector items: channels then the tokens

  PJ::Colormap colormap_ = PJ::Colormap::kTurbo;
  bool auto_range_ = true;
  double manual_lo_ = 0.0;
  double manual_hi_ = 1.0;
  FiniteRange last_auto_range_;
  bool lighting_ = true;
  double opacity_ = 1.0;
  bool show_links_ = true;

  std::optional<MeshMapUpload> staged_mesh_;
  std::uint64_t staged_revision_ = 0;
  std::optional<PJ::Timepoint> requested_time_;
};

}  // namespace pj::scene3d
