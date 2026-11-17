#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QColor>
#include <QElapsedTimer>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/compressed_point_cloud.hpp"
#include "pj_base/builtin/point_cloud.hpp"
#include "pj_datastore/object_store.hpp"   // ObjectStore::TimeRangeEntry (SampleId)
#include "pj_scene3d_core/scene_limits.h"  // BudgetLatch
#include "pj_scene3d_widgets/latest_wins_worker.h"
#include "pj_scene3d_widgets/layer_style.h"
#include "pj_scene3d_widgets/point_renderable.h"
#include "pj_scene3d_widgets/scene3d_layer.h"

class QWidget;

namespace pj::scene3d {

// A packPointVertices() ParallelFor on the global thread pool; the calling thread runs chunks too while it waits.
void parallelForOnPool(std::size_t count, const std::function<void(std::size_t)>& body);

// A single sensor_msgs/PointCloud2 topic. Dual-mode: raw PointCloud objects are converted and pushed synchronously,
// CompressedPointCloud blobs (Draco / Cloudini) decode on the Qt thread pool and push when the result lands; the mode
// is derived per sample, never latched.
//
// Every sample is admitted against kSceneLimits (per-cloud limits, decode limits before decompression; fail-closed
// with a status warning). The layer converts to source-frame PointVertex data the view reads through PointRenderable
// under the view's vertex budget, and drops a static cloud's array once uploaded (re-converting it for a new QRhi).
class PointCloudLayer : public Scene3DLayer, public PointRenderable {
  Q_OBJECT
 public:
  // Needed before attach(): info() can be called right after construction (the dock populates its row). `object_type`
  // is what info() reports (kPointCloud and kCompressedPointCloud render identically).
  PointCloudLayer(
      PJ::ObjectTopicId topic_id, QString display_name, PJ::sdk::BuiltinObjectType object_type,
      QObject* parent = nullptr);
  ~PointCloudLayer() override;

  [[nodiscard]] PJ::SceneLayerInfo info() const override;
  [[nodiscard]] PJ::Range<PJ::Timepoint> timeRange() const override;
  [[nodiscard]] QStringList fallbackFrames() const override;
  [[nodiscard]] SceneContributions sceneContributions() override {
    return {.points = this};
  }
  [[nodiscard]] QString sourceFrame() const override;
  QDomElement xmlSaveState(QDomDocument& doc) const override;
  bool xmlLoadState(const QDomElement& element) override;

  bool attach(const PJ::SceneLayerContext& ctx) override;
  void detach() override;

  void setFixedFrame(const QString& frame) override;
  void setTrackerTime(PJ::Timepoint time) override;
  // Fingerprints the active cloud sample at `time` plus the fixed<-source transform, so the dock skips repaints while
  // the same cloud sits at the same pose and repaints the instant a new sample lands or the sensor frame moves. No
  // decode.
  [[nodiscard]] uint64_t renderKey(PJ::Timepoint time) const override;
  void setVisible(bool visible) override;

  // Source-frame extent of the decoded cloud, recomputed on each decode in renderAt(); nullopt until the first decode.
  [[nodiscard]] std::optional<AABB> worldBounds() const override {
    return world_bounds_;
  }

  QWidget* createConfigWidget(QWidget* parent) override;

  [[nodiscard]] QStringList availableColorFields() const {
    return available_color_fields_;
  }
  [[nodiscard]] QString colorField() const {
    return QString::fromStdString(color_field_);
  }
  [[nodiscard]] PointShape shape() const override {
    return shape_;
  }
  [[nodiscard]] float sizeMeters() const override {
    return size_meters_;
  }
  [[nodiscard]] float sizePixels() const override {
    return size_pixels_;
  }
  [[nodiscard]] PointColorType colorType() const override {
    return color_type_;
  }
  // True when the active cloud carries a per-point colour field (a packed 'rgba'/'rgb' field, or separate
  // red/green/blue[/alpha] channels); gates the config widget's single "RGB" colour-type entry.
  [[nodiscard]] bool hasColorField() const {
    return has_color_;
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
  [[nodiscard]] float outsideRangeOpacity() const {
    return outside_range_opacity_;
  }
  [[nodiscard]] bool outsideRangeVisible() const {
    return outside_range_visible_;
  }
  [[nodiscard]] bool autoRange() const {
    return auto_range_;
  }
  [[nodiscard]] float manualRangeMin() const {
    return manual_range_min_;
  }
  [[nodiscard]] float manualRangeMax() const {
    return manual_range_max_;
  }

  void setColorField(const QString& field);
  void setShape(PointShape shape);
  void setSizeMeters(float meters);
  void setSizePixels(float pixels);
  void setColorType(PointColorType type);
  void setSolidColor(QColor color);
  void setColormap(PJ::Colormap cm);
  void setInvertLut(bool invert);
  void setOutsideRangeOpacity(float opacity);
  void setOutsideRangeVisible(bool visible);
  void setAutoRange(bool enable);
  void setManualRange(float min_value, float max_value);

  // Full validation of a <pointcloud> payload without mutating any layer, so a dock can vet a whole workspace before
  // tearing down its live layers.
  [[nodiscard]] static bool validateXml(const QDomElement& element);

  // Compressed-decode worker counters, read by the browser acceptance probes.
  [[nodiscard]] bool isCompressedForTest() const {
    return object_type_ == PJ::sdk::BuiltinObjectType::kCompressedPointCloud;
  }
  // True while an async compressed decode is in flight (or its finished event is still queued).
  [[nodiscard]] bool decodeInFlightForTest() const {
    return decode_worker_.busy();
  }
  // Number of decodes actually dispatched: a superseded pending sample, a known-failed sample and a redundant
  // re-request must NOT bump it.
  [[nodiscard]] int startDecodeCountForTest() const {
    return static_cast<int>(decode_worker_.starts());
  }
  [[nodiscard]] int decodeCompletionsForTest() const {
    return static_cast<int>(decode_worker_.completions());
  }
  [[nodiscard]] int decodeOffMainCompletionsForTest() const {
    return static_cast<int>(decode_worker_.offMainCompletions());
  }

  // prepareForRender() drains a pending tracker move (one decode per painted frame) and enforces the view's aggregate
  // vertex budget: an over-budget cloud is dropped with a warning and retried once the budget frees up.
  bool prepareForRender(std::uint64_t remaining_view_vertices) override;
  [[nodiscard]] const QByteArray& vertexBytes() const override {
    return vertex_bytes_;
  }
  [[nodiscard]] PointVertexFormat vertexFormat() const override {
    return vertex_format_;
  }
  [[nodiscard]] std::size_t pointCount() const override {
    return point_count_;
  }
  [[nodiscard]] std::uint64_t geometryRevision() const override {
    return geometry_revision_;
  }
  // A static cloud (no new sample within kStreamingWindow before it) drops its array as soon as it is uploaded; a
  // stream keeps it for the in-place rewrite until it has been quiet for that long.
  void onVerticesUploaded(std::uint64_t revision) override;
  bool ensureVertexBytes() override;
#ifdef PJ_SCENE3D_TEST_HOOKS
  [[nodiscard]] bool verticesEvictedForTest() const {
    return evicted();
  }
#endif
  [[nodiscard]] const std::string& renderFrameId() const override {
    return geometry_frame_;
  }
  [[nodiscard]] std::optional<PJ::Timepoint> renderStamp() const override {
    return geometry_stamp_;
  }
  [[nodiscard]] PlacementHold* placementHold() const override {
    return Scene3DLayer::placementHold();
  }
  [[nodiscard]] std::optional<AABB> sourceBounds() const override {
    return world_bounds_;
  }
  [[nodiscard]] bool visible() const override {
    return visible_;
  }
  [[nodiscard]] float outsideRangeAlpha() const override {
    return outside_range_visible_ ? outside_range_opacity_ : 0.0f;
  }
  [[nodiscard]] int scalarAxis() const override;
  [[nodiscard]] std::pair<float, float> scalarRange(const glm::mat4& fixed_from_source) const override;
  // The view owns fixed<-source, so it reports the effective auto range after applying TF; an interactive auto->manual
  // toggle freezes exactly these colours.
  void noteRenderedScalarRange(float minimum, float maximum) override;
  [[nodiscard]] bool isDepthCloud() const override {
    return false;
  }

#ifdef PJ_SCENE3D_TEST_HOOKS
  void renderAtForTest(int64_t time_ns) {
    renderAt(time_ns);
  }
  // Store stamp of the sample the render pass holds; nullopt when nothing is pushed.
  [[nodiscard]] std::optional<int64_t> lastPushedStampForTest() const {
    return last_pushed_id_ == SampleId{} ? std::nullopt : std::optional<int64_t>{last_pushed_id_.timestamp};
  }
#endif

 signals:
  void colorFieldsChanged(const QStringList& fields);
  void currentColorFieldChanged(const QString& field);
  // Auto-range pushes the computed (min, max) back so the panel's hidden spinboxes start from real data.
  void autoRangeComputed(float min_value, float max_value);

 private:
  // Identity of an ObjectStore sample, computable WITHOUT resolving the payload: the entry's SequentialUID (never
  // reused, so no collision across duplicate stamps, same-size payload swaps or dataset reloads) plus its store
  // timestamp for display and tests. Exactly what latestEntryIdAt() returns; {} is the "none" sentinel.
  using SampleId = PJ::ObjectStore::TimeRangeEntry;
  struct DecodeResult {
    SampleId id;
    std::shared_ptr<PJ::sdk::PointCloud> cloud;  // null on decode failure
    QString error;
  };
  struct Settings {
    PointShape shape = PointShape::kSphere;
    float size_meters = 0.0f;
    float size_pixels = 0.0f;
    PointColorType color_type = PointColorType::kField;
    bool color_choice_explicit = false;
    std::optional<QString> color_field;  // absent attribute: keep the current field
    std::optional<QColor> solid_color;   // absent attribute: keep the current colour
    PJ::Colormap colormap = PJ::Colormap::kTurbo;
    bool auto_range = true;
    bool invert_lut = false;
    float outside_range_opacity = 1.0f;
    bool outside_range_visible = true;
    float range_min = 0.0f;
    float range_max = 1.0f;
  };
  [[nodiscard]] static std::optional<Settings> parseSettings(const QDomElement& element);
  // The newest sample requested while a decode ran (the depth-1 "latest wins" queue).
  struct PendingDecode {
    PJ::sdk::CompressedPointCloud cloud;  // its shared anchor keeps the blob alive
    SampleId id;
  };

  // Decode the first sample once at attach: the available fields, source frame and time range must be known before
  // render.
  bool bootstrap();
  // Decode + push the cloud at time_ns into the retained vertices. No-op without a parser or a sample at/before
  // time_ns; skips all work when the layer already holds that sample with the current colour field (the common tracker
  // tick).
  void renderAt(int64_t time_ns);
  void drainPendingUpdate();
  [[nodiscard]] std::uint64_t budgetCandidate() const;
  // Consults the sample-identity memos for `id`, recording it as wanted_. True when no payload resolve is needed: the
  // sample is already pushed under the current colour settings, was re-pushed from the decoded cache, or is a
  // known-failed decode. Shared by renderAt's metadata-first gate and its post-resolve re-check (a streaming insert can
  // change which entry is latest between the two store queries).
  bool servedFromMemos(SampleId id);
  // Re-decode at the current playhead (or the first sample before any tracker tick) after a colour-field or auto-range
  // change, without waiting for the next tracker tick.
  void refreshNow();

  // Drops every sample-identity memo (decode cache, failure memo, pushed sample, wanted_) and invalidates queued and
  // running decodes. Used when the store bytes behind a stable topic id are replaced, and when a hidden layer releases
  // its retained cloud.
  void invalidateSampleState();

  // Converts a canonical PointCloud (raw, or freshly decompressed) into the retained vertices: the single point where
  // a cloud reaches the GPU, shared by the raw and compressed paths. `id` identifies the pushed sample for renderAt()'s
  // redundant-push skip. `rederive` (QRhi): `id` is the pushed sample, materialized again for an upload
  // (rederivePushedSample()); the push keeps geometry_revision_ and the streaming state and requests no repaint (the
  // frame that asked is being prepared).
  void pushCloud(const PJ::sdk::PointCloud& cloud, SampleId id, bool rederive = false);

  void evictVertices();
  // A static cloud's uploaded array was dropped: point_count_, vertex_format_, world_bounds_, the scalar range and
  // geometry_revision_ still describe it, and ensureVertexBytes() rebuilds it from last_pushed_id_ when a new QRhi
  // needs the upload again.
  [[nodiscard]] bool evicted() const {
    return vertex_bytes_.isEmpty() && point_count_ != 0;
  }
  // Re-converts last_pushed_id_ under the current style into vertex_bytes_ (pushCloud()'s `rederive`): from the decode
  // cache, else the store entry. A compressed entry the cache no longer holds clears the cloud until its off-UI-thread
  // decode lands; a sample the store no longer has clears it and lets the next frame decode at the playhead.
  void rederivePushedSample();

  // Fail-closed outcome for sample `id`: clears the drawn cloud, shows `warning` on the layer row, and memoizes the
  // failure so the sample is not re-decoded until a reload (or re-show) resets the memos.
  void failSample(SampleId id, const QString& warning);
  void clearGeometry();
  // Whether the vertices carry the colormap scalar: only for a colormapped
  // non-spatial field (colour, range cull and fade read it; no other style does).
  [[nodiscard]] bool storesScalar() const;

  void updateSourceFrame(const std::string& frame_id);

  // For a fixed-frame (x/y/z) colour axis: the [min,max] of that axis over the cached source bounds transformed into
  // the fixed frame at the current tracker time. nullopt when bounds, TF or time are unavailable (before the first
  // push).
  [[nodiscard]] std::optional<std::pair<float, float>> currentWorldAxisRange(int axis) const;

  // Shared body of setAutoRange. `seed_manual_from_world` governs the auto-OFF path: true (interactive toggle) seeds
  // the manual range from the world-axis range on screen so colours do not jump; false (state restore) keeps
  // manual_range_min_/max_ verbatim. Restore MUST pass false: the dock attach()es the layer (rendering the first
  // sample, so world_bounds_ is populated) before xmlLoadState, and seeding would overwrite the restored range.
  void applyAutoRange(bool enable, bool seed_manual_from_world);

  // Compressed decode is CPU-heavy (~100 ms for large Draco clouds), so it runs on the Qt thread pool: requestDecode()
  // records the request as wanted_ and coalesces latest-wins; onDecodeFinished() runs on the GUI thread, caches the
  // result and pushes it only if it still matches wanted_. Raw PointCloud topics convert synchronously.
  void requestDecode(const PJ::sdk::CompressedPointCloud& cloud, SampleId id);
  // The worker job: pure, touches no layer state (safe past the layer's death).
  [[nodiscard]] static DecodeResult decodeCompressed(const PendingDecode& request);
  void onDecodeFinished(DecodeResult result);
  void populateColorFields(const PJ::sdk::PointCloud& cloud);

  // Invoked by SessionManager::datasetAboutToBeReplaced (any dataset, filtered to this layer's). A reload keeps the
  // ObjectTopicId stable while swapping the store bytes: every sample-identity memo resets so pre-reload content is
  // dropped and the new bytes re-render.
  void onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id);

  PJ::ObjectTopicId topic_id_;
  QString display_name_;
  Scene3DLayerContext ctx_;
  // Parsers are deliberately NOT cached: every decode resolves a fresh ParserBinding through ctx_.session (see
  // parseLocked()), so a file reload that re-registers the topic's parser slot cannot leave a dangling one.

  std::string color_field_;
  QStringList available_color_fields_;
  // The latest requested sample's frame (sourceFrame(), the dock's frame
  // checks); a pending compressed decode's frame lands here before its points do.
  std::string source_frame_;
  std::string geometry_frame_;
  std::optional<PJ::Timepoint> geometry_stamp_;
  QString fixed_frame_;
  // The latest tracker time pushed to this layer, which refreshNow() re-decodes at. nullopt until the first tracker
  // tick: 0 ns is a valid timestamp, so absence is explicit (see SceneDockWidget::registerLayer).
  std::optional<PJ::Timepoint> decoded_at_ns_;
  // Set by setTrackerTime, consumed by drainPendingUpdate(): the decode is deferred to the next painted frame, and Qt
  // coalesces repaints, so a fast scrub decodes only the final cloud.
  bool tracker_dirty_ = false;

  bool visible_ = true;
  bool range_dirty_ = true;
  // Store stamp of the topic's first sample, captured at attach; nullopt when the
  // store had no entries yet (0 is a valid stamp, so absence is explicit).
  std::optional<int64_t> ts_first_;

  std::optional<AABB> world_bounds_;

  // User-tunable parameters, mirrored from the render pass; the config widget reads them on rebuild.
  PointShape shape_ = PointShape::kSphere;
  float size_meters_ = 0.02f;
  float size_pixels_ = 2.0f;
  PointColorType color_type_ = PointColorType::kField;
  bool has_color_ = false;
  // Set when the colour mode was chosen explicitly (layout restore or a user combo pick), so populateColorFields() does
  // NOT override it with the colour-present RGB default on the next decoded sample.
  bool color_choice_explicit_ = false;
  QColor solid_color_{255, 0, 0};
  PJ::Colormap colormap_ = PJ::Colormap::kTurbo;
  bool invert_lut_ = false;
  float outside_range_opacity_ = 1.0f;
  bool outside_range_visible_ = true;
  bool auto_range_ = true;
  float manual_range_min_ = 0.0f;
  float manual_range_max_ = 1.0f;

  PJ::sdk::BuiltinObjectType object_type_ = PJ::sdk::BuiltinObjectType::kPointCloud;

  // Sample and colour field the render pass holds; lets renderAt() skip the per-tick parse/convert/upload.
  SampleId last_pushed_id_;
  std::string last_pushed_color_field_;
  // Whether the last GPU push extracted per-point RGBA (kRgb mode). Part of the renderAt() skip key: toggling RGB must
  // re-decode even on the same sample, since color_field_ alone does not change between field/solid and rgb.
  bool last_pushed_rgb_ = false;
  bool last_pushed_scalar_ = false;  // the last push stored the colormap scalar (storesScalar())
  // Scalar axis of the last push (-1 non-spatial, 0/1/2 = x/y/z); written by pushCloud(), read nowhere.
  int last_pushed_axis_ = -1;

  std::shared_ptr<PJ::sdk::PointCloud> decoded_cache_;  // last decoded cloud, reused on color-field change
  SampleId decoded_cache_id_;                           // identity of decoded_cache_
  SampleId wanted_;         // the sample the tracker currently wants; gates painting async results
  SampleId failed_id_;      // last failed sample; not retried until a dataset reload swaps the store bytes
  QString failed_warning_;  // the warning re-shown whenever failed_id_ is revisited

  // Lives only while attached: resets the sample-identity caches when a reload swaps this topic's store bytes.
  QMetaObject::Connection reload_connection_;

  QByteArray vertex_bytes_;  // vertex_format_ records: finite points of the active cloud, source frame
  PointVertexFormat vertex_format_ = PointVertexFormat::kXyz;  // layout of vertex_bytes_
  std::size_t point_count_ = 0;                                // records in vertex_bytes_ (the rest is padding)
  std::uint64_t geometry_revision_ = 0;
  // A new sample followed the previous one within kStreamingWindow: the uploaded array stays for the next sample's
  // in-place rewrite, and evict_timer_ drops it once the stream has been quiet for that long.
  bool streaming_ = false;
  QElapsedTimer last_sample_push_;
  std::uint64_t uploaded_revision_ = ~std::uint64_t{0};  // last onVerticesUploaded()
  QElapsedTimer last_stream_upload_;                     // restarted by every upload while streaming_
  QTimer evict_timer_;
  // Auto range of the active cloud's non-spatial scalar (spatial axes derive it
  // from world_bounds_ under the live transform instead).
  std::pair<float, float> automatic_scalar_range_{0.0f, 1.0f};
  std::pair<float, float> last_rendered_scalar_range_{0.0f, 1.0f};
  BudgetLatch budget_;

  LatestWinsWorker<PendingDecode, DecodeResult> decode_worker_{
      this, &PointCloudLayer::decodeCompressed, [this](DecodeResult result) { onDecodeFinished(std::move(result)); }};
};

}  // namespace pj::scene3d
