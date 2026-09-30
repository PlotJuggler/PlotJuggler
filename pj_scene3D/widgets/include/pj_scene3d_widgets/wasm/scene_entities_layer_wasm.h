// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QByteArray>
#include <QColor>
#include <QDomDocument>
#include <QFuture>
#include <QFutureWatcher>
#include <QMetaObject>
#include <QString>
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "pj_base/builtin/scene_entities.hpp"
#include "pj_base/time.hpp"
#include "pj_datastore/sequential_uid.hpp"
#include "pj_scene3d_core/scene_entities_budget.h"
#include "pj_scene3d_core/scene_entities_model_state.h"
#include "pj_scene3d_widgets/mesh_data.h"
#include "pj_scene3d_widgets/wasm/model_renderable_wasm.h"
#include "pj_scene_common/scene_layer.h"

namespace PJ {
class SessionManager;
}

class QThread;

namespace pj::scene3d {

class MeshLoader;
class UrlFetcher;

struct WasmMarkerInstanceSource {
  glm::dmat4 model{1.0};
  glm::vec4 color{1.0F};
  std::uint32_t frame_index = 0;
  float bottom_scale = 1.0F;
  float top_scale = 1.0F;
};

struct WasmMarkerStreamVertex {
  glm::dvec3 position{0.0};
  glm::vec4 color{1.0F};
};

struct WasmMarkerStreamBatch {
  glm::dmat4 model{1.0};
  std::vector<WasmMarkerStreamVertex> vertices;
  std::uint32_t frame_index = 0;
};

struct WasmMarkerBounds {
  glm::dvec3 min{0.0};
  glm::dvec3 max{0.0};
  bool valid = false;
};

struct WasmMarkerGeometry {
  std::vector<std::string> frames;
  std::vector<WasmMarkerBounds> frame_bounds;
  std::vector<WasmMarkerInstanceSource> cubes;
  std::vector<WasmMarkerInstanceSource> spheres;
  std::vector<WasmMarkerInstanceSource> cylinders;
  std::vector<WasmMarkerInstanceSource> arrows;
  std::vector<WasmMarkerInstanceSource> axes;
  std::vector<WasmMarkerStreamBatch> lines;
  std::vector<WasmMarkerStreamBatch> triangles;
  std::uint64_t line_vertices = 0;
  std::uint64_t triangle_vertices = 0;
  std::uint64_t skipped_texts = 0;
  std::uint64_t skipped_models = 0;
  std::uint64_t skipped_invalid = 0;

  [[nodiscard]] std::uint64_t instanceCount() const {
    return cubes.size() + spheres.size() + cylinders.size() + arrows.size() + axes.size();
  }
  [[nodiscard]] std::uint64_t maximumStreamVertexCount() const {
    // Wireframe triangle edges expand each triangle's three vertices to six.
    return line_vertices + triangle_vertices * 2U;
  }
  [[nodiscard]] bool empty() const {
    return instanceCount() == 0U && line_vertices == 0U && triangle_vertices == 0U;
  }
};

// Emscripten-only SceneEntities adapter. Procedural markers intentionally keep
// native's stateless latest-batch semantics; ModelPrimitive uses the separate
// native-compatible identity/deletion/lifetime replay track.
class WasmSceneEntitiesLayer final : public PJ::ISceneLayer, public WasmModelRenderable {
  Q_OBJECT

 public:
  static constexpr std::uint64_t kMaxWireBytesPerSample = kSceneLimits.max_marker_wire_bytes;
  static constexpr std::uint64_t kMaxInstancesPerLayer = kSceneLimits.max_marker_instances_per_layer;
  static constexpr std::uint64_t kMaxStreamVerticesPerLayer = kSceneLimits.max_marker_stream_vertices_per_layer;

  WasmSceneEntitiesLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent = nullptr);
  ~WasmSceneEntitiesLayer() override;

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

  bool prepareForRender();
  [[nodiscard]] const WasmMarkerGeometry& geometry() const {
    return geometry_;
  }
  [[nodiscard]] const std::vector<WasmModelDrawCall>& modelDrawCalls() const override {
    return model_draw_calls_;
  }
  [[nodiscard]] const std::unordered_map<std::string, std::shared_ptr<const MeshData>>& modelMeshes() const override {
    return model_meshes_;
  }
  [[nodiscard]] std::uint64_t modelRevision() const override {
    return model_revision_;
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
  [[nodiscard]] std::uint64_t geometryRevision() const {
    return geometry_revision_;
  }
  [[nodiscard]] const std::string& sourceFrame() const override {
    return source_frame_;
  }
  [[nodiscard]] bool visible() const override {
    return visible_;
  }
  [[nodiscard]] float opacity() const override {
    return opacity_;
  }
  [[nodiscard]] bool colorOverrideEnabled() const override {
    return color_override_enabled_;
  }
  [[nodiscard]] QColor overrideColor() const override {
    return override_color_;
  }
  [[nodiscard]] bool wireframe() const override {
    return wireframe_;
  }
  [[nodiscard]] QString warningReason() const {
    return warning_reason_;
  }

  [[nodiscard]] bool contributesToSceneBounds() const override {
    return true;
  }

  void noteRenderFailure(QString warning) override;
  void noteRenderSuccess() override;

  [[nodiscard]] bool decodeInFlightForTest() const {
    return inflight_sample_.has_value();
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

  [[nodiscard]] static bool validateXml(const QDomElement& element);

 signals:
  void sourceFrameChanged(QString frame);

 private:
  struct SampleId {
    std::int64_t stamp = std::numeric_limits<std::int64_t>::min();
    std::size_t payload_size = 0;
    PJ::SequentialUID uid;
    bool operator==(const SampleId&) const = default;
  };

  struct DecodeRequest {
    std::shared_ptr<const PJ::sdk::SceneEntities> scene;
    SampleId sample;
    std::uint64_t generation = 0;
  };

  struct DecodeResult {
    SampleId sample;
    std::uint64_t generation = 0;
    WasmMarkerGeometry geometry;
    QString error;
    bool ran_off_main_thread = false;
  };

  struct ParsedSettings {
    float opacity = 1.0F;
    bool color_override = false;
    QColor override_color{Qt::red};
    bool wireframe = false;
  };

  struct CachedSnapshot {
    std::shared_ptr<const PJ::sdk::SceneEntities> batch;
    std::uint64_t bytes = 0;
    std::int64_t store_ns = 0;
  };

  struct ModelLoad {
    enum class Phase { kWaiting, kFetching, kImporting, kReady, kFailed, kBlocked };

    std::string identity;
    QString source_label;
    QString error;
    std::uint64_t generation = 0;
    Phase phase = Phase::kWaiting;
  };

  struct ActiveModelFetch {
    std::string key;
    std::string identity;
    std::uint64_t generation = 0;
  };

  struct ActiveModelImport {
    std::string key;
    std::string identity;
    std::uint64_t generation = 0;
    QFuture<MeshData> future;
    std::unique_ptr<QFutureWatcher<MeshData>> watcher;
  };

  [[nodiscard]] static std::optional<ParsedSettings> parseSettings(const QDomElement& element);
  [[nodiscard]] static DecodeResult decode(DecodeRequest request, QThread* main_thread);
  bool decodeAt(PJ::Timepoint time);
  void queueDecode(DecodeRequest request);
  void startDecode(DecodeRequest request);
  void onDecodeFinished();
  void applyResult(DecodeResult result);
  void invalidateDecodeState();
  void clearGeometry();
  void resetModelState();
  void rebuildModelStateAt(PJ::Timepoint time);
  void ensureModelStateAt(PJ::Timepoint time);
  bool applyModelWindow(std::int64_t lo_ns, std::int64_t hi_ns);
  void cacheSnapshot(PJ::SequentialUID uid, std::shared_ptr<const PJ::sdk::SceneEntities> batch, std::int64_t store_ns);
  void pruneSnapshotCacheBelow(PJ::SequentialUID first_retained_uid);
  void syncModelSources();
  void refreshModelDrawCalls();
  void startModelLoad(const std::string& key, const PJ::sdk::ModelPrimitive& primitive);
  void pumpModelLoads();
  bool startModelImport(
      const std::string& key, const std::string& identity, std::uint64_t generation, const QByteArray& bytes,
      const QString& format_hint);
  void finishModelImport(const std::string& key, const std::string& identity, std::uint64_t generation);
  void updateModelWarning();
  void updateSourceFrame();
  void setDataWarning(QString warning);
  void updateWarning();
  void setOpacity(float opacity);
  void setColorOverrideEnabled(bool enabled);
  void setOverrideColor(QColor color);
  void setWireframe(bool enabled);
  void onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id);

  PJ::ObjectTopicId topic_id_;
  QString display_name_;
  PJ::SessionManager* session_ = nullptr;
  QMetaObject::Connection reload_connection_;
  QMetaObject::Connection samples_connection_;

  WasmMarkerGeometry geometry_;
  std::uint64_t geometry_revision_ = 0;
  std::string source_frame_;
  SampleId active_sample_;
  SampleId wanted_sample_;
  std::optional<PJ::Timepoint> requested_time_;
  bool decode_dirty_ = false;
  bool visible_ = true;

  float opacity_ = 0.6F;
  bool color_override_enabled_ = false;
  QColor override_color_{Qt::red};
  bool wireframe_ = false;
  QString data_warning_;
  QString model_state_warning_;
  QString model_warning_;
  QString render_warning_;
  QString warning_reason_;

  QFutureWatcher<DecodeResult>* decode_watcher_ = nullptr;
  // The worker owns the only SceneEntities copy. Keeping only its identity on
  // the main thread avoids retaining a second payload-sized object while the
  // bounded expansion is in flight.
  std::optional<SampleId> inflight_sample_;
  std::uint64_t inflight_generation_ = 0;
  std::optional<DecodeRequest> pending_;
  std::uint64_t decode_generation_ = 1;
  std::uint64_t decode_starts_ = 0;
  std::uint64_t decode_completions_ = 0;
  std::uint64_t decode_off_main_completions_ = 0;

  SceneEntitiesModelState model_state_;
  std::optional<PJ::Timepoint> model_state_built_at_;
  PJ::SequentialUID model_applied_uid_high_;
  std::map<PJ::SequentialUID, CachedSnapshot> snapshot_cache_;
  std::uint64_t snapshot_cache_bytes_ = 0;
  std::unique_ptr<MeshLoader> mesh_loader_;
  std::unique_ptr<UrlFetcher> url_fetcher_;
  std::optional<ActiveModelFetch> active_model_fetch_;
  std::unique_ptr<ActiveModelImport> active_model_import_;
  std::unordered_map<std::string, ModelLoad> model_loads_;
  std::unordered_map<std::string, std::shared_ptr<const MeshData>> model_meshes_;
  std::vector<WasmModelDrawCall> model_draw_calls_;
  std::uint64_t model_retained_bytes_ = 0;
  std::uint64_t model_revision_ = 0;
  // Never reset with layer state: a detached worker from an older dataset must
  // not alias a newly-created load that happens to reuse the same key/source.
  std::uint64_t next_model_load_generation_ = 1;
};

}  // namespace pj::scene3d
