// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QMetaObject>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pj_base/builtin/mesh_map.hpp"
#include "pj_datastore/object_store.hpp"  // PJ::ObjectTopicId, PJ::SequentialUID
#include "pj_runtime/SessionManager.h"    // PJ::DatasetId
#include "pj_scene3d_core/mesh_map_reconstructor.h"
#include "pj_scene3d_core/mesh_map_view.h"
#include "pj_scene3d_widgets/channel_coloring.h"
#include "pj_scene3d_widgets/scene3d_layer.h"
#include "pj_widgets/Colormap.h"

class QWidget;

namespace pj::scene3d {

// Scene3DLayer for an sdk::MeshMap topic and the sdk::MeshMapUpdate topics that
// patch it. The layer owns the store side (which entries exist at the tracker
// time, fetching and decoding them, spotting an update that arrived late) and
// MeshMapReconstructor the mesh: which update belongs to the map, applying them
// forward, starting over from the map on a rewind. Every MeshMapUpdate topic of
// the dataset is a candidate stream; the `<topic>_updates` sibling (the
// convention OccupancyGridLayer follows) is the one that also applies updates
// with no geometry_id. The result is de-indexed for the view; an update that
// only rewrote faces of the coloured channel re-colours that span alone
// (stagedColorDelta()). The requested colour source is kept apart from the
// effective one so a persisted request is honoured again once its channel
// reappears. Placed at the tracker time (no renderStamp()): the drawn mesh is a
// map plus updates with no single stamp.
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
  // Set when the step from revision stagedRevision() - 1 to stagedRevision()
  // rewrote only this (non-empty) span of colour corners: a view holding the
  // previous revision uploads just the span.
  [[nodiscard]] const std::optional<ElementRange>& stagedColorDelta() const {
    return staged_color_delta_;
  }
  [[nodiscard]] const std::string& renderFrameId() const override {
    return source_frame_;
  }
  [[nodiscard]] const ChannelColoring& coloring() const {
    return coloring_;
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
  // uniform-only. A channel key ("face:<name>" / "vertex:<name>"),
  // kMeshMapSourceVertexColor, kMeshMapSourceSolid, or empty = auto.
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
  // Maps decoded to start over from, refused ones included.
  [[nodiscard]] int rebaseCountForTest() const {
    return rebase_count_;
  }
  [[nodiscard]] int appliedUpdateCountForTest() const {
    return applied_update_count_;
  }
  [[nodiscard]] size_t updateTopicCountForTest() const {
    return update_topics_.size();
  }
#endif

 signals:
  // The channel list or the effective pick changed: the config panel's selector
  // rebuilds from color_sources_.
  void colorSourcesChanged();

 private:
  // Decode the first map at attach to learn the source frame + channel list.
  bool bootstrap();
  // Bring the mesh to `time_ns` and stage the upload. Cheap no-op when the map
  // and the updates at that time are the ones already staged, or that map was
  // refused. Re-stages only when the geometry or the colours changed.
  void renderAt(int64_t time_ns);
  // Decodes the map at `entry` and restarts the reconstruction from it; false
  // (base_warning_ set, nothing changed) when the map is refused.
  bool rebase(const PJ::ResolvedObjectEntry& entry);
  // The reconstructor's UpdatesProvider: the entries of every update topic with
  // stamps in (lo, hi], merged in time order, fetched and decoded only when the
  // sink wants them.
  void feedUpdates(PJ::Timestamp lo, PJ::Timestamp hi, MeshMapReconstructor::Sink& sink);
  // Turns what a replay skipped into update_warning_.
  void noteReplay(const MeshMapReplay& replay);
  // Re-lists the dataset's MeshMapUpdate topics when its topic list changed.
  void refreshUpdateTopics();
  // Identifies the set of updates at or before `time_ns`: per topic, the highest
  // arrival UID and the latest-by-stamp entry's UID, folded.
  [[nodiscard]] std::uint64_t updateKey(int64_t time_ns, std::vector<std::uint64_t>* max_uids = nullptr) const;
  // True when an update for this map arrived stamped at or before what was
  // already applied: any late one on a paired topic (the sibling or `matched`),
  // one naming this geometry on another topic (decoded to tell; the topic's
  // cursor then moves past those that do not).
  [[nodiscard]] bool lateUpdateArrived();
  // Picks the effective colour source from the request and the state's channels.
  void resolveColorSource();
  // Stages the reconstructed mesh: the positions when `geometry_changed`, and
  // the colours whole, or, when only the last replay changed them
  // (`values_changed`), the span it rewrote. No-op when neither changed.
  void stage(bool geometry_changed, bool values_changed);
  // A colormap or range edit: persist and repaint (uniforms only, no re-stage).
  void coloringChanged();
  void clearStaged();
  void resetStreamingState();
  void onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id);
  void setBounds(std::optional<AABB> bounds);

  PJ::ObjectTopicId topic_id_;
  QString display_name_;
  Scene3DLayerContext ctx_;
  QMetaObject::Connection reload_connection_;
  // A MeshMapUpdate topic of the dataset, a stream of reconstructor_ keyed by
  // its topic id.
  struct UpdateTopic {
    PJ::ObjectTopicId id;
    // Newest arrival at or before the reconstructor's appliedUntil() when it
    // was last advanced: what arrived later is new to lateUpdateArrived().
    std::uint64_t applied_max_uid = 0;
  };
  std::vector<UpdateTopic> update_topics_;
  std::vector<PJ::ObjectTopicId> known_topics_;  // the dataset's topics when update_topics_ was listed

  // The mesh at the last rendered time. Its state owns its arrays (no anchor
  // into a plugin DSO), so no binding keepalive is needed.
  MeshMapReconstructor reconstructor_;
  std::uint64_t staged_update_key_ = 0;
  PJ::SequentialUID staged_base_uid_{};
  bool colors_dirty_ = true;
  std::uint64_t geometry_revision_ = 0;
  std::optional<AABB> bounds_;
  QString update_warning_;
  QString base_warning_;
  QString color_warning_;  // a requested colour source the map lacks

  std::string source_frame_;
  std::string fixed_frame_;
  bool tracker_dirty_ = false;
  bool visible_ = true;
  int rebase_count_ = 0;
  int applied_update_count_ = 0;

  std::string color_request_;
  std::string resolved_color_;              // a channel key or a kMeshMapSource* token
  std::vector<std::string> color_sources_;  // selector items: channels then the tokens

  ChannelColoring coloring_;  // colormap, auto / manual range, the last auto range
  bool lighting_ = true;
  double opacity_ = 1.0;
  bool show_links_ = true;

  std::optional<MeshMapUpload> staged_mesh_;
  std::uint64_t staged_revision_ = 0;
  std::optional<ElementRange> staged_color_delta_;
  std::optional<PJ::Timepoint> requested_time_;
};

}  // namespace pj::scene3d
