#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QColor>
#include <QFutureWatcher>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/compressed_point_cloud.hpp"
#include "pj_base/builtin/point_cloud.hpp"
#include "pj_datastore/object_store.hpp"  // ObjectStore::TimeRangeEntry (SampleId)
#include "pj_scene3d_widgets/passes/pointcloud_render_pass.h"
#include "pj_scene3d_widgets/scene3d_layer.h"

class QWidget;

namespace pj::scene3d {

// First concrete Scene3DLayer: a single sensor_msgs/PointCloud2 topic.
// Owns its own PointcloudRenderPass and absorbs the per-topic state that
// used to live in Scene3DDockWidget::PointCloudTopicState.
//
// Dual-mode: the topic may carry raw PointCloud objects (converted and pushed
// synchronously) or CompressedPointCloud blobs (Draco / Cloudini), which are
// decoded on the Qt thread pool and pushed when the result lands. The mode is
// derived per-sample from the parsed object, never latched.
//
// Exposes a per-instance config widget containing the color-field combo.
// Adding new per-instance parameters (point size, alpha, colormap choice)
// is local to this class — Scene3DDockWidget and Scene3DConfigPanel do
// not need to change.
class PointCloudLayer : public Scene3DLayer {
  Q_OBJECT
 public:
  // The topic id + initial display name are needed before attach() since
  // info() can be called immediately after construction (e.g. by the
  // dock to populate its row). object_type is what info() reports (the layer
  // renders kPointCloud and kCompressedPointCloud topics identically).
  PointCloudLayer(
      PJ::ObjectTopicId topic_id, QString display_name, PJ::sdk::BuiltinObjectType object_type,
      QObject* parent = nullptr);
  ~PointCloudLayer() override;

  // Scene3DLayer
  [[nodiscard]] PJ::SceneLayerInfo info() const override;
  [[nodiscard]] PJ::Range<PJ::Timepoint> timeRange() const override;
  [[nodiscard]] QStringList fallbackFrames() const override;
  [[nodiscard]] QString sourceFrame() const override;
  QDomElement xmlSaveState(QDomDocument& doc) const override;
  bool xmlLoadState(const QDomElement& element) override;

  bool attach(const PJ::SceneLayerContext& ctx) override;
  void detach() override;

  void setFixedFrame(const QString& frame) override;
  void setTrackerTime(PJ::Timepoint time) override;
  // Fingerprints the active cloud sample at `time` plus the fixed←source transform
  // (so the dock skips repaints when the same cloud sits at the same pose, but
  // repaints the instant a new sample lands or the sensor frame moves). No decode.
  [[nodiscard]] uint64_t renderKey(PJ::Timepoint time) const override;
  void setVisible(bool visible) override;

  void initializeGL() override;
  void render(const ViewParams& view_params, const FrameContext& frame_ctx) override;
  void releaseGL() override;
  // Source-frame extent of the decoded cloud, recomputed on each decode in
  // renderAt(). nullopt until the first cloud is decoded.
  [[nodiscard]] std::optional<AABB> worldBounds() const override {
    return world_bounds_;
  }

  QWidget* createConfigWidget(QWidget* parent) override;

  // PointCloud-specific accessors used by the config widget. Kept on the
  // concrete class so the abstract base doesn't carry pointcloud
  // concepts.
  [[nodiscard]] QStringList availableColorFields() const {
    return available_color_fields_;
  }
  [[nodiscard]] QString colorField() const {
    return QString::fromStdString(color_field_);
  }
  [[nodiscard]] PointcloudRenderPass::Shape shape() const {
    return shape_;
  }
  [[nodiscard]] float sizeMeters() const {
    return size_meters_;
  }
  [[nodiscard]] float sizePixels() const {
    return size_pixels_;
  }
  [[nodiscard]] PointcloudRenderPass::ColorType colorType() const {
    return color_type_;
  }
  // True when the active cloud carries a per-point colour field (a packed 'rgba'/'rgb'
  // field, or separate red/green/blue[/alpha] channels). Gates the config widget's
  // single "RGB" colour-type entry.
  [[nodiscard]] bool hasColorField() const {
    return has_color_;
  }
  [[nodiscard]] QColor solidColor() const {
    return solid_color_;
  }
  [[nodiscard]] PointcloudRenderPass::Colormap colormap() const {
    return colormap_;
  }
  [[nodiscard]] bool invertLut() const {
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
  void setShape(PointcloudRenderPass::Shape shape);
  void setSizeMeters(float meters);
  void setSizePixels(float pixels);
  void setColorType(PointcloudRenderPass::ColorType type);
  void setSolidColor(QColor color);
  void setColormap(PointcloudRenderPass::Colormap cm);
  void setInvertLut(bool invert);
  void setOutsideRangeOpacity(float opacity);
  void setOutsideRangeVisible(bool visible);
  void setAutoRange(bool enable);
  void setManualRange(float min_value, float max_value);

#ifdef PJ_SCENE3D_TEST_HOOKS
  void renderAtForTest(int64_t time_ns) {
    renderAt(time_ns);
  }
  // Store stamp of the sample currently held by the render pass; nullopt when
  // nothing is pushed (the default SampleId sentinel).
  [[nodiscard]] std::optional<int64_t> lastPushedStampForTest() const {
    return last_pushed_id_ == SampleId{} ? std::nullopt : std::optional<int64_t>{last_pushed_id_.timestamp};
  }
  // True while an async compressed decode is in flight (or its finished event
  // is still queued) — lets tests pump the loop until the result landed.
  [[nodiscard]] bool decodeInFlightForTest() const {
    return inflight_ != SampleId{};
  }
  // Number of times startDecode() actually dispatched a worker. The coalescing
  // test asserts on this: a superseded pending sample, a known-failed sample,
  // and a redundant re-request must NOT bump it.
  [[nodiscard]] int startDecodeCountForTest() const {
    return start_decode_count_;
  }
  // True when the render pass holds a zero-copy fast cloud (vs the CloudVertex fallback) — lets a
  // test assert an eligible cloud (incl. packed-rgba RGB-direct) actually took the fast path.
  [[nodiscard]] bool activeCloudIsFastForTest() const {
    return cloud_pass_.activeCloudIsFastForTest();
  }
#endif

 signals:
  // Emitted whenever the color-field set or selection changes — the
  // config widget (if alive) listens and updates its combo without
  // having to be rebuilt.
  void colorFieldsChanged(const QStringList& fields);
  void currentColorFieldChanged(const QString& field);
  // Auto-range mode pushes the computed (min, max) back to the panel so
  // the spinboxes (when subsequently unhidden) start from real data
  // values rather than stale defaults.
  void autoRangeComputed(float min_value, float max_value);

 private:
  // Identity of an ObjectStore sample, computable WITHOUT resolving the payload:
  // the entry's SequentialUID (never reused, so it cannot collide across
  // duplicate stamps, same-size payload swaps, or dataset reloads) plus its
  // store timestamp, carried along for display/tests. Exactly what
  // latestEntryIdAt() returns; value-initialized {} is the "none" sentinel.
  using SampleId = PJ::ObjectStore::TimeRangeEntry;
  struct DecodeResult {
    SampleId id;
    std::shared_ptr<PJ::sdk::PointCloud> cloud;  // null on decode failure
    QString error;
  };
  // The newest sample requested while a decode ran (the depth-1 "latest wins" queue).
  struct PendingDecode {
    PJ::sdk::CompressedPointCloud cloud;  // its shared anchor keeps the blob alive
    SampleId id;
  };

  // Decode the first sample once at attach so we know available fields,
  // source frame, and time range before render is called.
  bool bootstrap();
  // Decode + push the cloud at time_ns into the render pass. No-op when
  // we don't have a parser or when the store has no sample at/before
  // time_ns. Skips all work when the pass already holds exactly that
  // sample with the current color field (the common tracker tick).
  void renderAt(int64_t time_ns);
  // Consults the sample-identity memos for `id`, recording it as wanted_.
  // Returns true when no payload resolve is needed: the sample is already
  // pushed under the current color settings, was re-pushed from the decoded
  // cache, or is a known-failed decode. Shared by renderAt's metadata-first
  // gate and its post-resolve re-check (a streaming insert can change which
  // entry is latest between the two store queries).
  bool servedFromMemos(SampleId id);
  // Re-decode at the current playhead (or the first sample before any tracker
  // tick) after a color-field or auto-range change, so the renderer reflects
  // the new state without waiting for the next tracker tick.
  void refreshNow();

  // Convert a canonical PointCloud (raw, or freshly decompressed) into the render
  // struct and push it to the pass. The single point where a cloud reaches the GPU,
  // shared by the raw and compressed paths. `id` identifies the pushed sample for
  // the redundant-push skip in renderAt().
  void pushCloud(const PJ::sdk::PointCloud& cloud, SampleId id);

  // Receives an async GPU AABB reduction from the render pass (GL thread). Updates
  // world_bounds_ (+ the spatial-axis colormap range) and requests a repaint so the
  // camera scene-fit picks up the new extent. Wired as the pass bounds callback.
  void onGpuAabb(std::optional<AABB> box);

  // The single writer of world_bounds_: it also hands the extent to the render pass,
  // which frustum-culls the whole cloud with it. Assigning the member directly would
  // leave the pass testing a stale box, so don't.
  void setWorldBounds(std::optional<AABB> bounds);

  // Track a (possibly changing) source frame_id; notify the dock/panel on change.
  void updateSourceFrame(const std::string& frame_id);

  // For a fixed-frame (x/y/z) colour axis: the [min,max] of that axis over the
  // cached source bounds transformed into the fixed frame at the current tracker
  // time. Used to freeze a sensible world-axis range when auto-range is switched
  // off. std::nullopt when bounds/TF/time are unavailable (e.g. before first push).
  [[nodiscard]] std::optional<std::pair<float, float>> currentWorldAxisRange(int axis) const;

  // Shared body of setAutoRange. `seed_manual_from_world` governs the auto-OFF
  // path: true (interactive toggle) seeds the manual range from the world-axis
  // range currently on screen so colours don't jump; false (state restore) keeps
  // the existing manual_range_min_/max_ verbatim. Restore MUST pass false — the
  // dock attach()es the layer (rendering the first sample, so world_bounds_ is
  // already populated) before calling xmlLoadState, so seeding would overwrite
  // the just-restored saved range with the recomputed data range.
  void applyAutoRange(bool enable, bool seed_manual_from_world);

  // Fold the opacity scrubber + visibility eye into the pass's single effective
  // outside-range alpha (eye off -> 0). Called from both setters and construction.
  void pushOutsideRangeAlpha();

  // --- Compressed-cloud async decode (Draco / Cloudini) ---
  // Compressed decode is CPU-heavy (~100ms for large Draco clouds), so it runs on the
  // Qt thread pool and never blocks the UI. requestDecode() records the request as
  // wanted_ and coalesces latest-wins; onDecodeFinished() runs on the GUI thread,
  // caches the result, and pushes it only if it still matches wanted_.
  // Raw PointCloud topics skip all of this and convert synchronously (cheap).
  void ensureDecodeWorker();
  void requestDecode(const PJ::sdk::CompressedPointCloud& cloud, SampleId id);
  void startDecode(const PJ::sdk::CompressedPointCloud& cloud, SampleId id);
  void onDecodeFinished();
  void populateColorFields(const PJ::sdk::PointCloud& cloud);

  // Invoked by SessionManager::datasetAboutToBeReplaced (any dataset; filtered to
  // this layer's). A reload keeps the ObjectTopicId stable while swapping the
  // store bytes; every sample-identity memo — decode cache, failure memo, pushed
  // sample, wanted_/in-flight — resets so pre-reload content is dropped and the
  // new bytes re-render.
  void onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id);

  PJ::ObjectTopicId topic_id_;
  QString display_name_;
  Scene3DLayerContext ctx_;
  // Parsers are deliberately NOT cached: every decode resolves a fresh
  // ParserBinding through ctx_.session (see parseLocked()), so a file reload
  // that re-registers the topic's parser slot can never leave us dangling.

  std::string color_field_;
  QStringList available_color_fields_;
  std::string source_frame_;
  QString fixed_frame_;
  // The latest tracker time pushed to this layer; the Timepoint the cached VBO
  // was (re)decoded at. Used by refreshNow() to re-decode at the current playhead.
  // nullopt until the first tracker tick — 0 ns is a valid timestamp, so absence
  // is explicit (the dock's stance; see SceneDockWidget::registerLayer).
  std::optional<PJ::Timepoint> decoded_at_ns_;
  // Set by setTrackerTime, consumed by render(): the decode (parse + convert +
  // GPU upload) is deferred to the next painted frame instead of running eagerly
  // per tracker tick. Qt coalesces repaints, so a fast scrub that fires many ticks
  // decodes only the final cloud once, not every skipped intermediate frame.
  bool tracker_dirty_ = false;

  bool visible_ = true;
  bool range_dirty_ = true;
  // Store stamp of the topic's first sample, captured at attach; nullopt when the
  // store had no entries yet (0 is a valid stamp, so absence is explicit).
  std::optional<int64_t> ts_first_;

  // Source-frame bounds of the most recently decoded cloud (see worldBounds()).
  std::optional<AABB> world_bounds_;

  // User-tunable per-cloud parameters. Mirror the render pass's current
  // values; the panel reads them on rebuild so a re-opened config widget
  // always reflects what the user picked.
  PointcloudRenderPass::Shape shape_ = PointcloudRenderPass::Shape::kSphere;
  float size_meters_ = 0.02f;
  float size_pixels_ = 2.0f;
  PointcloudRenderPass::ColorType color_type_ = PointcloudRenderPass::ColorType::kField;
  // True once the active cloud is known to carry a per-point colour field. Drives the
  // "RGB" combo entry and the fresh-attach default to kRgb.
  bool has_color_ = false;
  // Set when the colour mode was chosen explicitly (layout restore or a user combo
  // pick), so populateColorFields() does NOT override it with the colour-present RGB
  // default on the next decoded sample. Cleared state = "pick a smart default".
  bool color_choice_explicit_ = false;
  QColor solid_color_{255, 0, 0};
  PointcloudRenderPass::Colormap colormap_ = PointcloudRenderPass::Colormap::kTurbo;
  bool invert_lut_ = false;
  float outside_range_opacity_ = 1.0f;
  bool outside_range_visible_ = true;
  bool auto_range_ = true;
  float manual_range_min_ = 0.0f;
  float manual_range_max_ = 1.0f;

  // What info() reports; kPointCloud and kCompressedPointCloud render identically.
  PJ::sdk::BuiltinObjectType object_type_ = PJ::sdk::BuiltinObjectType::kPointCloud;

  // Sample + color field currently held by the render pass; lets renderAt() skip
  // the per-tick parse/convert/upload when nothing changed.
  SampleId last_pushed_id_;
  std::string last_pushed_color_field_;
  // Whether the last GPU push extracted per-point RGBA (kRgb mode). Part of the
  // renderAt() skip key so toggling RGB on/off forces a re-decode even on the same
  // sample (color_field_ alone doesn't change between field/solid and rgb).
  bool last_pushed_rgb_ = false;
  // Scalar axis of the last push (-1 non-spatial, 0/1/2 = x/y/z). onGpuAabb() reads
  // it to decide whether an async GPU bounds result must also refresh the
  // spatial-axis colormap range.
  int last_pushed_axis_ = -1;

  // Async compressed-cloud decode state. Inert for raw PointCloud topics.
  QFutureWatcher<DecodeResult>* decode_watcher_ = nullptr;  // child of this; created on first compressed sample
  SampleId inflight_;                                       // the sample currently decoding; default = idle
  std::optional<PendingDecode> pending_;
  std::shared_ptr<PJ::sdk::PointCloud> decoded_cache_;  // last decoded cloud, reused on color-field change
  SampleId decoded_cache_id_;                           // identity of decoded_cache_
  SampleId wanted_;     // the sample the tracker currently wants; gates painting async results
  SampleId failed_id_;  // last failed decode; not retried until a dataset reload swaps the store bytes

  // Counts startDecode() dispatches; read only via startDecodeCountForTest(). Kept
  // unconditional (not behind PJ_SCENE3D_TEST_HOOKS) so the class layout is identical
  // in the library and in the test TUs — a macro-gated member would mismatch and
  // read garbage from the test side.
  int start_decode_count_ = 0;

  // Lives only while attached: resets the sample-identity caches when a dataset
  // reload swaps this topic's store bytes (see onDatasetAboutToBeReplaced).
  QMetaObject::Connection reload_connection_;
  // Set when a reload invalidates the sample currently decoding: its result is
  // pre-reload content, so onDecodeFinished must not cache or memoize it.
  bool drop_inflight_result_ = false;

  PointcloudRenderPass cloud_pass_;
};

}  // namespace pj::scene3d
