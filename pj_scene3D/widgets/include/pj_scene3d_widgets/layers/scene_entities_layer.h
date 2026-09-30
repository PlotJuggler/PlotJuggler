#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QByteArray>
#include <QColor>
#include <QMetaObject>
#include <QString>
#include <QStringList>
#include <cstddef>
#include <cstdint>
#include <deque>
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
#include "pj_scene3d_widgets/mesh_data.h"
#include "pj_scene3d_widgets/model_renderable.h"
#include "pj_scene3d_widgets/scene3d_layer.h"
#ifdef PJ_SCENE3D_RHI
#include "pj_scene3d_widgets/latest_wins_worker.h"
#include "pj_scene3d_widgets/marker_geometry.h"
#else
#include "pj_scene3d_widgets/passes/marker_render_pass.h"
#include "pj_scene3d_widgets/passes/mesh_render_pass.h"
#endif

class QWidget;

namespace pj::scene3d {

class MeshImport;
class MeshLoader;
class UrlFetcher;

// Concrete Scene3DLayer for a single visualization_msgs/MarkerArray-equivalent
// topic (canonical sdk::SceneEntities, type kSceneEntities), shared by both
// renderers. Two primitive families:
//
// - **Markers** (cube/sphere/cylinder/arrow/line/triangle primitives): the
//   latest batch at/before the tracker time (stateless). Selection, payload
//   admission and parsing are shared; the sink differs. GL build: the batch is
//   decoded into the layer's MarkerRenderPass. QRhi build: the batch is expanded
//   into MarkerGeometry on a worker thread (latest wins, stale results dropped
//   by generation), which the view reads through geometry().
// - **Models** (ModelPrimitive, embedded glTF bytes or a file/http URL): all
//   batches up to the tracker time are replayed into an id-keyed entity state
//   (SceneEntitiesModelState: replace-by-id, deletions, lifetime expiry), and
//   the models' bytes load asynchronously a few at a time (fetch, then import off
//   the GUI thread; one in the browser, see maxConcurrentMeshLoads), each
//   admitted against kSceneLimits' model budgets as it lands. GL build:
//   imported meshes go to the layer's MeshRenderPass. QRhi build: they are
//   retained as MeshData and exposed with frame-local ModelDrawCalls through
//   ModelRenderable (the view applies TF).
//   DATA-SUPPLIED http(s) model URLs are policy-gated behind the QSettings bool
//   "pj_scene3d/allow_remote_model_fetch" (default ON; set false to opt OUT);
//   local file URLs / bare paths always load. Failures surface through
//   remoteFetchNotice() and statusWarning().
//
// Known divergence (follow-up): the marker path shows only the latest batch,
// so markers from earlier batches with different entity ids disappear, while
// their models persist.
//
// Viewer-side display overrides (opacity / color override / wireframe) are a
// per-topic preference, not marker data. Opacity and the color override also
// apply to model draws; wireframe is marker-only.
class SceneEntitiesLayer : public Scene3DLayer, public ModelRenderable {
  Q_OBJECT
 public:
  SceneEntitiesLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent = nullptr);
  ~SceneEntitiesLayer() override;

  // Identity row for the dock's layer list (family "Markers").
  [[nodiscard]] PJ::SceneLayerInfo info() const override;
  // First/last sample timestamps of the topic in the ObjectStore.
  [[nodiscard]] PJ::Range<PJ::Timepoint> timeRange() const override;
  // The marker batch's source frame plus every distinct model-entity frame.
  [[nodiscard]] QStringList fallbackFrames() const override;
  // Frame of the latest selected marker batch (first entity's frame_id).
  [[nodiscard]] QString sourceFrame() const override;
  // Persist / restore the viewer-side display overrides (tag "markers").
  QDomElement xmlSaveState(QDomDocument& doc) const override;
  bool xmlLoadState(const QDomElement& element) override;
  // Session-free check that `element` is a loadable "markers" payload.
  [[nodiscard]] static bool validateXml(const QDomElement& element);

  // Bind to the session (a parser binding or a canonical codec is required),
  // select the first batch and replay the initial model state.
  bool attach(const PJ::SceneLayerContext& ctx) override;
  // Drop session bindings and all marker + model state.
  void detach() override;

  // Select the marker batch and advance the model state to the new playhead
  // (no-op while hidden; the dock re-delivers the time on un-hide).
  void setTrackerTime(PJ::Timepoint time) override;
  void setVisible(bool visible) override;

#ifndef PJ_SCENE3D_RHI
  // GL lifecycle for both passes (per-context resources; see module CLAUDE.md).
  void initializeGL() override;
  // Draw markers, then model primitives.
  void render(const ViewParams& view_params, const FrameContext& frame_ctx) override;
  void releaseGL() override;
  // Model meshes cast shadows; marker primitives do not. Both hooks operate on
  // modelDrawCallsForFrame only.
  [[nodiscard]] std::optional<AABB> meshShadowBounds(const FrameContext& frame_ctx) override;
  void renderShadowCasters(const glm::mat4& light_view_proj, const FrameContext& frame_ctx) override;
  // World-space mesh draw calls for a frame (fixed-frame TF × entity pose × scale,
  // camera-relative through frame_ctx.lookup). Tests and diagnostics use it too.
  [[nodiscard]] std::vector<MeshRenderPass::DrawCall> modelDrawCallsForFrame(const FrameContext& frame_ctx) const;
#endif

  // Opacity / color-override / wireframe controls bound to this instance.
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

  // Display-override setters: update overrides_, request a repaint. Opacity ∈
  // [0,1] multiplies every primitive's alpha.
  void setOpacity(float opacity);
  void setColorOverrideEnabled(bool enabled);
  void setOverrideColor(QColor color);
  void setWireframe(bool enabled);

  // The id-keyed model-entity state at the last replayed tracker time. Entities
  // keep identity, frame, timing and models only (marker primitives are dropped).
  [[nodiscard]] const std::map<std::string, PJ::sdk::SceneEntity>& currentEntities() const {
    return model_state_.entities();
  }

  // Model load failures: URLs blocked by the remote-fetch policy gate (one
  // summary line), failed fetches (URL + network error) and rejected imports
  // (source + importer/budget message). Empty when there is nothing to surface.
  [[nodiscard]] QString remoteFetchNotice() const {
    return remote_fetch_notice_;
  }

  // ModelRenderable (read by the QRhi view; empty in the GL build):
  // frame-local draws (frame_id + pose × scale), no TF applied.
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

  // Model imports started whose completion has not been processed yet: an upper
  // bound on the imports still running on the pool.
  [[nodiscard]] std::uint64_t modelImportsOutstandingForTest() const {
    return model_import_starts_ - model_import_settles_;
  }
#ifndef PJ_SCENE3D_RHI
  /// The overrides the GL marker pass draws with.
  [[nodiscard]] const MarkerDisplayOverrides& markerPassOverridesForTest() const {
    return pass_.overrides();
  }
#else
  // Re-selects the marker batch when live samples arrived since the last tick;
  // called by the view before it reads geometry(). Returns true when the
  // geometry changed synchronously (cleared).
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
#endif

 signals:
  // remoteFetchNotice() changed (possibly back to empty).
  void remoteFetchNoticeChanged(const QString& notice);

 private:
  // A checked "markers" payload (validateXml()).
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

  // One model primitive's load record, keyed by "topic:entity:index". Its
  // identity is the source signature (content hash / URL), so a re-published
  // model with new bytes replaces the record; `generation` is never reused, so
  // a stale fetch/import completion cannot alias a newer record.
  struct ModelLoad {
    enum class Phase { kWaiting, kFetching, kImporting, kReady, kFailed, kBlocked };

    std::string identity;
    QString source_label;
    QString error;
    std::uint64_t generation = 0;
    Phase phase = Phase::kWaiting;
    bool fetch_failed = false;
  };

  // A waiting load in the pump queue. `generation` matches its ModelLoad while
  // that record is current; a superseded entry is skipped when popped.
  struct QueuedModelLoad {
    std::string key;
    std::string entity_id;
    std::size_t model_index = 0;
    std::uint64_t generation = 0;
  };

  enum class MarkerPick { kUnchanged, kNoSample, kRejected, kBatch };

  // Reset every replay artifact (marker selection, model state, snapshot cache,
  // model loads, frames) to the post-construction baseline. attach() runs it
  // because a dataset reload may re-attach without an intervening detach().
  void resetReplayState();
  void onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id);
  // Select the batch at/before time_ns and hand it to the renderer's sink.
  void renderAt(std::int64_t time_ns);
  // Shared selection: metadata-first skip of the unchanged batch, payload
  // admission BEFORE parsing, cache-first parse. A rejected or failed sample is
  // memoized by UID and not retried. kBatch fills `batch`.
  MarkerPick selectMarkerBatch(std::int64_t time_ns, std::shared_ptr<const PJ::sdk::SceneEntities>& batch);
  // Parse one store entry through its parser binding or the canonical codec.
  std::shared_ptr<PJ::sdk::SceneEntities> decodeEntry(
      std::int64_t timestamp, const PJ::sdk::PayloadView& payload, QString& error) const;
  void setSourceFrame(std::string frame);

  // Model path. rebuildModelStateAt replays every batch up to `time` (first
  // build, backward scrubs, retroactive inserts); ensureModelStateAt folds only
  // newly-appended batches for forward playback.
  void rebuildModelStateAt(PJ::Timepoint time);
  void ensureModelStateAt(PJ::Timepoint time);
  // Fold every retained batch with lo_ns < ts <= hi_ns (ascending timestamp),
  // cache-first; a batch over the model-state budget is refused with a warning.
  bool applyModelWindow(std::int64_t lo_ns, std::int64_t hi_ns);
  void cacheSnapshot(PJ::SequentialUID uid, std::shared_ptr<const PJ::sdk::SceneEntities> batch, std::int64_t store_ns);
  void pruneSnapshotCacheBelow(PJ::SequentialUID first_retained_uid);
  // Recompute the distinct entity frames feeding fallbackFrames().
  void updateModelFrames();
  // Reconcile load records with the live entities (drop records and meshes of
  // gone models, add records for new/changed ones) and pump the load queue.
  void syncModelSources();
  void startModelLoad(const std::string& key, const PJ::sdk::ModelPrimitive& primitive, QueuedModelLoad queued);
  // Aborts the in-flight fetches once none of them is still wanted (they share
  // one fetcher, so they can only be aborted together).
  void abortObsoleteFetches();
  // Start queued loads while fewer than maxConcurrentMeshLoads() run. Superseded
  // fetches and imports count until they settle (their results are discarded),
  // so replacing sources never runs more than the bound at once.
  void pumpModelLoads();
  bool startModelImport(
      const std::string& key, const std::string& identity, std::uint64_t generation, const QByteArray& bytes,
      const QString& format_hint);
  void finishModelImport(const std::string& key, const std::string& identity, std::uint64_t generation);
  // Renderer sink for an admitted mesh / a dropped one.
  void storeModelMesh(const std::string& key, MeshData mesh, std::uint64_t bytes);
  void releaseModelMesh(const std::string& key);
  void updateRemoteFetchNotice();
  void setBatchWarning(QString warning);
  // Data part of statusWarning(): the batch, model-state, model-draw and model
  // load warnings, one per line.
  void updateWarning();
  void applyOverrides();

  PJ::ObjectTopicId topic_id_;
  QString display_name_;
  Scene3DLayerContext ctx_;
  // Parsers are deliberately NOT cached: every decode resolves a fresh binding
  // through ctx_.session, so a file reload that re-registers the topic's parser
  // slot can never leave us dangling.
  QMetaObject::Connection reload_connection_;

  std::string source_frame_;
  // UID of the marker batch last selected (applied, rejected or in flight).
  PJ::SequentialUID last_marker_uid_;
  std::optional<PJ::Timepoint> requested_time_;
  bool visible_ = true;

  // Viewer-side display overrides. 0.6 opacity + red are the design defaults.
  MarkerDisplayOverrides overrides_{.opacity = 0.6F, .override_color = glm::vec4(1.0F, 0.0F, 0.0F, 1.0F)};

  // Model path. model_frames_ caches the distinct entity frames merged into
  // fallbackFrames().
  SceneEntitiesModelState model_state_;
  std::optional<PJ::Timepoint> state_built_at_;
  // High-water ObjectStore arrival UID folded into model_state_ at
  // state_built_at_ (invalid = nothing folded). The same-playhead early return
  // compares it to the live high-water; a grown value at/below the build time
  // means a retroactive insert, which forces a full rebuild.
  PJ::SequentialUID applied_uid_high_;
  // Decoded batches keyed by store UID, shared by the marker selection and the
  // model fold, bounded by kSceneLimits.max_model_snapshot_cache_bytes (oldest
  // UIDs evicted first).
  std::map<PJ::SequentialUID, CachedSnapshot> snapshot_cache_;
  std::uint64_t snapshot_cache_bytes_ = 0;
  QStringList model_frames_;

  std::unique_ptr<MeshLoader> mesh_loader_;
  // Owned so destroying the layer (or resetReplayState) aborts in-flight fetches;
  // one retired by abortObsoleteFetches() stays a QObject child until its deferred delete.
  std::unique_ptr<UrlFetcher> url_fetcher_;
  // Fetches (-> their key) and imports in flight, by load generation, superseded
  // ones included; together at most maxConcurrentMeshLoads().
  std::unordered_map<std::uint64_t, std::string> active_model_fetches_;
  std::unordered_map<std::uint64_t, std::unique_ptr<MeshImport>> active_model_imports_;
  std::deque<QueuedModelLoad> model_load_queue_;
  std::unordered_map<std::string, ModelLoad> model_loads_;
  // Retained estimate of each admitted mesh; the key set is the ready set.
  std::unordered_map<std::string, std::uint64_t> model_mesh_bytes_;
  std::uint64_t model_retained_bytes_ = 0;
  std::uint64_t next_model_load_generation_ = 1;
  std::uint64_t model_import_starts_ = 0;
  std::uint64_t model_import_settles_ = 0;

  QString batch_warning_;        // marker batch admission / decode
  QString model_state_warning_;  // model snapshot fold (budget, decode, lost batch)
  QString model_draw_warning_;   // QRhi model draw admission (refreshModelDrawCalls)
  QString remote_fetch_notice_;

  // QRhi sink (empty in the GL build).
  std::unordered_map<std::string, std::shared_ptr<const MeshData>> model_meshes_;
  std::vector<ModelDrawCall> model_draw_calls_;
  std::uint64_t model_revision_ = 0;

#ifdef PJ_SCENE3D_RHI
  struct DecodeRequest {
    std::shared_ptr<const PJ::sdk::SceneEntities> scene;
    PJ::SequentialUID uid;
  };
  struct DecodeResult {
    PJ::SequentialUID uid;
    MarkerGeometryResult built;
  };

  // The worker job: expands a batch into MarkerGeometry (pure).
  [[nodiscard]] static DecodeResult expandBatch(const DecodeRequest& request);
  void onDecodeFinished(DecodeResult result);
  // Drop queued/in-flight results and forget the selected batch.
  void invalidateDecodeState();
  void clearGeometry();
  // Frame-local ModelDrawCalls from the model state, admitted against the
  // per-layer draw budget.
  void refreshModelDrawCalls();

  QMetaObject::Connection samples_connection_;
  // Live samples arrived: prepareForRender re-selects at requested_time_.
  bool decode_dirty_ = false;
  MarkerGeometry geometry_;
  std::uint64_t geometry_revision_ = 0;
  LatestWinsWorker<DecodeRequest, DecodeResult> decode_worker_{
      this, &SceneEntitiesLayer::expandBatch, [this](DecodeResult result) { onDecodeFinished(std::move(result)); }};
#else
  MarkerRenderPass pass_;
  std::unique_ptr<MeshRenderPass> mesh_pass_;
#endif
};

}  // namespace pj::scene3d
