#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QColor>
#include <QMetaObject>
#include <QString>
#include <QStringList>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "pj_base/builtin/scene_entities.hpp"
#include "pj_datastore/sequential_uid.hpp"
#include "pj_scene3d_core/scene_entities_model_state.h"
#include "pj_scene3d_core/scene_entities_render.h"
#include "pj_scene3d_core/tf/object_placement.h"
#include "pj_scene3d_widgets/latest_wins_worker.h"
#include "pj_scene3d_widgets/marker_geometry.h"
#include "pj_scene3d_widgets/mesh_data.h"
#include "pj_scene3d_widgets/model_renderable.h"
#include "pj_scene3d_widgets/scene3d_layer.h"

class QWidget;

namespace pj::scene3d {

class MeshLoadQueue;
class UrdfPackageResolver;

// Concrete Scene3DLayer for one visualization_msgs/MarkerArray-equivalent topic (canonical sdk::SceneEntities,
// kSceneEntities). Two primitive families:
//
// - **Markers** (cube/sphere/cylinder/arrow/line/triangle): the latest batch at or before the tracker time
//   (stateless), expanded into MarkerGeometry on a worker thread (latest wins, stale results dropped by generation),
//   which the view reads through geometry().
// - **Models** (ModelPrimitive, embedded glTF bytes or a file/http/package:// URL): all batches up to the tracker
//   time are replayed into an id-keyed entity state (SceneEntitiesModelState: replace-by-id, deletions, lifetime
//   expiry); the bytes load asynchronously through a MeshLoadQueue (fetch, then import off the GUI thread; bounds
//   in mesh_load_queue.h), each admitted against kSceneLimits' model budgets as it lands. Imported meshes are
//   retained as MeshData and exposed as frame-local ModelDrawCalls through ModelRenderable (the view applies TF).
//   Every http(s) request a model makes is gated by the QSettings bool "pj_scene3d/allow_remote_model_fetch"
//   (remoteModelFetchAllowed() in mesh_load_queue.h); local file URLs / bare paths always load. Failures surface
//   through remoteFetchNotice() and statusWarning().
//
// Known gap: the marker path shows only the latest batch, so markers from earlier batches with different entity ids
// disappear while their models persist.
//
// Viewer-side display overrides (opacity / color override / wireframe) are a per-topic preference, not marker data.
// Opacity and the color override also apply to model draws; wireframe is marker-only.
class SceneEntitiesLayer : public Scene3DLayer, public ModelRenderable {
  Q_OBJECT
 public:
  SceneEntitiesLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent = nullptr);
  ~SceneEntitiesLayer() override;

  [[nodiscard]] PJ::SceneLayerInfo info() const override;
  [[nodiscard]] PJ::Range<PJ::Timepoint> timeRange() const override;
  [[nodiscard]] QStringList fallbackFrames() const override;
  [[nodiscard]] SceneContributions sceneContributions() override {
    return {.markers = this, .models = this};
  }
  [[nodiscard]] QString sourceFrame() const override;
  // Persists the viewer-side display overrides (tag "markers").
  QDomElement xmlSaveState(QDomDocument& doc) const override;
  bool xmlLoadState(const QDomElement& element) override;
  [[nodiscard]] static bool validateXml(const QDomElement& element);

  // Needs a parser binding or a canonical codec; selects the first batch and replays the initial model state.
  bool attach(const PJ::SceneLayerContext& ctx) override;
  void detach() override;

  // No-op while hidden (the dock re-delivers the time on un-hide).
  void setTrackerTime(PJ::Timepoint time) override;
  // The tracker time with the active entry's SequentialUID: a same-stamp replacement at an unchanged time still
  // reopens the dock's gate. Never coalesces across times (a lifetime can expire with no new entry).
  [[nodiscard]] uint64_t renderKey(PJ::Timepoint time) const override;
  // A finding's real stamp (its entry sits at kFindingTimestamp), so it stays where it was measured in a
  // moving frame; nullopt otherwise. It places a finding's models and the layer's scene bounds; the marker
  // primitives are placed per group, by markerStamp().
  [[nodiscard]] std::optional<PJ::Timepoint> renderStamp() const override {
    return finding_stamp_;
  }
  // What the view places group `index` of geometry() with (MarkerGeometry::frames). The stamp is the finding's when
  // the layer shows one, else the entities' own on the dataset clock, so they stay where they were measured like a
  // cloud; nullopt places the group at the cursor (unstamped or frame-locked entities, which move with their frame).
  [[nodiscard]] std::optional<PJ::Timepoint> markerStamp(std::size_t index) const;
  [[nodiscard]] PlacementHold* markerPlacementHold(std::size_t index) const;
  [[nodiscard]] PlacementHold* placementHoldFor(const std::string& frame) const override;
  void setVisible(bool visible) override;

  QWidget* createConfigWidget(QWidget* parent) override;

  [[nodiscard]] float opacity() const override {
    return overrides_.opacity;
  }
  [[nodiscard]] bool colorOverrideEnabled() const override {
    return overrides_.color_override;
  }
  [[nodiscard]] QColor overrideColor() const override {
    return QColor::fromRgbF(overrides_.override_color.r, overrides_.override_color.g, overrides_.override_color.b);
  }
  [[nodiscard]] bool wireframe() const override {
    return overrides_.wireframe;
  }

  // Opacity in [0,1] multiplies every primitive's alpha.
  void setOpacity(float opacity);
  void setColorOverrideEnabled(bool enabled);
  void setOverrideColor(QColor color);
  void setWireframe(bool enabled);
  // Resolves package:// model URLs (a ROS Marker's mesh_resource); the dock's shared resolver, not owned. Without one a
  // package:// model fails with a visible reason.
  void setPackageResolver(UrdfPackageResolver* resolver) {
    package_resolver_ = resolver;
  }

  // Entities keep identity, frame, timing and models only (marker primitives are dropped).
  [[nodiscard]] const std::map<std::string, PJ::sdk::SceneEntity>& currentEntities() const {
    return model_state_.entities();
  }

  // Model load failures: URLs blocked by the remote-fetch policy gate (one
  // summary line), failed fetches (URL + network error) and rejected imports
  // (source + importer/budget message). Empty when there is nothing to surface.
  [[nodiscard]] QString remoteFetchNotice() const {
    return remote_fetch_notice_;
  }

  // Frame-local draws (frame_id + pose x scale), no TF applied.
  [[nodiscard]] const std::vector<ModelDrawCall>& modelDrawCalls() const override {
    return model_draw_calls_;
  }
  [[nodiscard]] const std::unordered_map<std::string, std::shared_ptr<const MeshData>>& modelMeshes() const override {
    return model_meshes_;
  }
  [[nodiscard]] std::uint64_t modelRevision() const override {
    return model_revision_;
  }
  [[nodiscard]] bool visible() const override {
    return visible_;
  }
  [[nodiscard]] const std::string& renderFrameId() const override {
    return source_frame_;
  }
  [[nodiscard]] bool contributesToSceneBounds() const override {
    return true;
  }
  [[nodiscard]] Scene3DLayer& modelLayer() override {
    return *this;
  }

  [[nodiscard]] std::size_t modelImportsOutstandingForTest() const;
  // Re-selects the marker batch when live samples arrived since the last tick; called by the view before it reads
  // geometry(). True when the geometry changed synchronously (cleared).
  bool prepareForRender();
  [[nodiscard]] const MarkerGeometry& geometry() const {
    return geometry_;
  }
  [[nodiscard]] std::uint64_t geometryRevision() const {
    return geometry_revision_;
  }

  [[nodiscard]] std::uint64_t modelRetainedBytes() const {
    return model_retained_bytes_;
  }
  [[nodiscard]] std::size_t readyModelCountForTest() const {
    return model_meshes_.size();
  }
  [[nodiscard]] std::size_t liveModelCountForTest() const {
    return model_draw_calls_.size();
  }
  [[nodiscard]] bool decodeInFlightForTest() const {
    return decode_worker_.busy();
  }
  [[nodiscard]] std::uint64_t decodeStartsForTest() const {
    return decode_worker_.starts();
  }
  [[nodiscard]] std::uint64_t decodeCompletionsForTest() const {
    return decode_worker_.completions();
  }
  [[nodiscard]] std::uint64_t decodeOffMainCompletionsForTest() const {
    return decode_worker_.offMainCompletions();
  }

 signals:
  void remoteFetchNoticeChanged(const QString& notice);

 private:
  struct ParsedSettings {
    float opacity = 1.0F;
    bool color_override = false;
    bool wireframe = false;
    std::optional<QColor> override_color;
  };
  [[nodiscard]] static std::optional<ParsedSettings> parseSettings(const QDomElement& element);

  struct CachedSnapshot {
    std::shared_ptr<const PJ::sdk::SceneEntities> batch;
    std::uint64_t bytes = 0;
    // ObjectStore entry timestamp; restores the expiry anchor on a cache-hit re-fold.
    std::int64_t store_ns = 0;
  };

  // One model primitive's load record, keyed by "topic:entity:index". Its identity is the source signature (content
  // hash / URL), so a re-published model with new bytes replaces the record and cancels its queue job.
  struct ModelLoad {
    enum class Phase { kLoading, kReady, kFailed, kBlocked };

    std::string identity;
    QString source_label;
    QString error;
    std::uint64_t job = 0;  // MeshLoadQueue job while kLoading
    Phase phase = Phase::kLoading;
    bool fetch_failed = false;
  };

  enum class MarkerPick { kUnchanged, kNoSample, kRejected, kBatch };

  // Resets every replay artifact (marker selection, model state, snapshot cache, model loads, frames). attach() runs it
  // because a dataset reload may re-attach without an intervening detach().
  void resetReplayState();
  void onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id);
  void renderAt(std::int64_t time_ns);
  // Shared selection: metadata-first skip of the unchanged batch, payload admission BEFORE parsing, cache-first parse.
  // A rejected or failed sample is memoized by UID and not retried. kBatch fills `batch`.
  MarkerPick selectMarkerBatch(std::int64_t time_ns, std::shared_ptr<const PJ::sdk::SceneEntities>& batch);
  std::shared_ptr<PJ::sdk::SceneEntities> decodeEntry(
      std::int64_t timestamp, const PJ::sdk::PayloadView& payload, QString& error) const;
  void setSourceFrame(std::string frame);

  // Model path. rebuildModelStateAt replays every batch up to `time` (first build, backward scrubs, retroactive
  // inserts); ensureModelStateAt folds only newly appended batches for forward playback.
  void rebuildModelStateAt(PJ::Timepoint time);
  void ensureModelStateAt(PJ::Timepoint time);
  // Fold every retained batch with lo_ns < ts <= hi_ns (ascending timestamp),
  // cache-first; a batch over the model-state budget is refused with a warning.
  bool applyModelWindow(std::int64_t lo_ns, std::int64_t hi_ns);
  // Fold one store entry into the model state, cache-first; counts a lost lazy re-read in `failed_fetches`.
  bool foldEntry(const PJ::ObjectStore::TimeRangeEntry& reference, std::size_t& failed_fetches);
  // Apply one decoded batch, refused over the model-state budget. Its lifetimes count from `store_ns`, except a
  // finding's, which never expire.
  bool applySnapshot(const PJ::sdk::SceneEntities& snapshot, std::int64_t store_ns);
  void cacheSnapshot(PJ::SequentialUID uid, std::shared_ptr<const PJ::sdk::SceneEntities> batch, std::int64_t store_ns);
  void pruneSnapshotCacheBelow(PJ::SequentialUID first_retained_uid);
  void updateModelFrames();
  // Reconciles load records with the live entities and pumps the load queue.
  void syncModelSources();
  // Admission: a new or changed source is recorded, then refused (no source,
  // oversized embedded bytes, remote fetch not allowed) or queued.
  void startModelLoad(
      const std::string& key, const std::string& entity_id, std::size_t model_index,
      const PJ::sdk::ModelPrimitive& primitive);
  void admitModel(const std::string& key, MeshData mesh, bool fetch_failed);
  void storeModelMesh(const std::string& key, MeshData mesh, std::uint64_t bytes);
  void releaseModelMesh(const std::string& key);
  void updateRemoteFetchNotice();
  void setBatchWarning(QString warning);
  // Data part of statusWarning(): the batch, model-state, model-draw and model load warnings, one per line.
  void updateWarning();
  void applyOverrides();

  PJ::ObjectTopicId topic_id_;
  QString display_name_;
  Scene3DLayerContext ctx_;
  // Parsers are deliberately NOT cached: every decode resolves a fresh binding through ctx_.session, so a file reload
  // that re-registers the topic's parser slot cannot leave a dangling one.
  QMetaObject::Connection reload_connection_;

  std::string source_frame_;
  // Every entry is a complete snapshot (pj_snapshot): the model state is the shown entry alone.
  bool snapshot_stream_ = false;
  std::optional<PJ::Timepoint> finding_stamp_;  // see renderStamp()
  // Store stamp of the entry shown at the cursor: with nothing at or before a later cursor it was evicted and stays;
  // before it, nothing is there and the layer clears.
  std::optional<std::int64_t> shown_store_ns_;
  // One placement hold per entity frame (placementHoldFor): a finding drawn in several frames.
  mutable std::map<std::string, PlacementHold, std::less<>> frame_holds_;
  // One placement hold per group of geometry_ (markerPlacementHold), replaced with the geometry they placed.
  mutable std::vector<PlacementHold> marker_holds_;
  // payload_stamp_shift of the entry the marker batch came from: puts the entities' own stamps on the dataset clock.
  PJ::Timestamp marker_stamp_shift_ = 0;
  PJ::SequentialUID last_marker_uid_;
  std::optional<PJ::Timepoint> requested_time_;
  bool visible_ = true;

  // 0.6 opacity and red are the design defaults.
  MarkerDisplayOverrides overrides_{.opacity = 0.6F, .override_color = glm::vec4(1.0F, 0.0F, 0.0F, 1.0F)};

  SceneEntitiesModelState model_state_;
  std::optional<PJ::Timepoint> state_built_at_;
  // High-water ObjectStore arrival UID folded into model_state_ at state_built_at_ (invalid = nothing folded; for a
  // snapshot stream, the shown entry's). The same-playhead early return compares it to the live high-water: a grown
  // value at/below the build time means a retroactive insert and forces a full rebuild.
  PJ::SequentialUID applied_uid_high_;
  // Decoded batches keyed by store UID, shared by the marker selection and the model fold, bounded by
  // kSceneLimits.max_model_snapshot_cache_bytes (oldest UIDs evicted first).
  std::map<PJ::SequentialUID, CachedSnapshot> snapshot_cache_;
  std::uint64_t snapshot_cache_bytes_ = 0;
  QStringList model_frames_;

  // Owned so destroying the layer aborts in-flight fetches and drops pending completions.
  std::unique_ptr<MeshLoadQueue> model_queue_;
  UrdfPackageResolver* package_resolver_ = nullptr;
  std::unordered_map<std::string, ModelLoad> model_loads_;
  std::unordered_map<std::string, std::uint64_t> model_mesh_bytes_;
  std::uint64_t model_retained_bytes_ = 0;

  QString batch_warning_;        // marker batch admission / decode
  QString model_state_warning_;  // model snapshot fold (budget, decode, lost batch)
  QString model_draw_warning_;   // QRhi model draw admission (refreshModelDrawCalls)
  QString remote_fetch_notice_;

  std::unordered_map<std::string, std::shared_ptr<const MeshData>> model_meshes_;
  std::vector<ModelDrawCall> model_draw_calls_;
  std::uint64_t model_revision_ = 0;

  struct DecodeRequest {
    std::shared_ptr<const PJ::sdk::SceneEntities> scene;
    PJ::SequentialUID uid;
  };
  struct DecodeResult {
    PJ::SequentialUID uid;
    MarkerGeometryResult built;
  };

  [[nodiscard]] static DecodeResult expandBatch(const DecodeRequest& request);
  void onDecodeFinished(DecodeResult result);
  void invalidateDecodeState();
  void clearGeometry();
  void refreshModelDrawCalls();

  QMetaObject::Connection samples_connection_;
  bool decode_dirty_ = false;
  MarkerGeometry geometry_;
  std::uint64_t geometry_revision_ = 0;
  LatestWinsWorker<DecodeRequest, DecodeResult> decode_worker_{
      this, &SceneEntitiesLayer::expandBatch, [this](DecodeResult result) { onDecodeFinished(std::move(result)); }};
};

}  // namespace pj::scene3d
