#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QColor>
#include <QDomDocument>
#include <QDomElement>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pj_datastore/object_store.hpp"  // PJ::ObjectTopicId
#include "pj_runtime/SessionManager.h"    // ParserBinding
#include "pj_scene3d_core/trail_sampling.h"
#include "pj_scene3d_widgets/latest_wins_worker.h"
#include "pj_scene3d_widgets/scene3d_layer.h"

class QFormLayout;
class QWidget;

namespace pj::tf {
class TransformBuffer;
}  // namespace pj::tf

namespace pj::scene3d {

using pj::tf::TransformBuffer;

// What a TrailLayer tracks. kTfFrame trails need only the dock's TF buffer; kPoseTopic trails read the topic's messages
// (first pose each) AND the TF buffer to place them in the fixed frame.
struct TrailSource {
  enum class Kind { kTfFrame, kPoseTopic };
  Kind kind = Kind::kTfFrame;
  QString frame;               // kTfFrame
  PJ::ObjectTopicId topic{0};  // kPoseTopic

  static TrailSource tfFrame(QString frame_name) {
    TrailSource source;
    source.kind = Kind::kTfFrame;
    source.frame = std::move(frame_name);
    return source;
  }
  static TrailSource poseTopic(PJ::ObjectTopicId topic_id) {
    TrailSource source;
    source.kind = Kind::kPoseTopic;
    source.topic = topic_id;
    return source;
  }
};

// Scene3DLayer that draws the trajectory of a frame (or a pose topic's first pose) across the whole loaded time range,
// split-coloured at the tracker time (past colour before it, future colour after). The polyline rebuilds
// asynchronously on structural changes (fixed frame, reload; TF sources only: pose sources rebuild synchronously
// because ObjectStore/parseLocked access is a GUI-thread contract, bounded by decimate-before-decode) and appends
// incrementally at the live edge. Scrubbing costs a binary search: renderKey folds the split index, so the repaint
// gate coalesces between samples. Not topic-id-backed: the dock registers it under a synthetic local id
// (info().object_type == kNone) and persists it with role="trail" (Scene3DDockWidget::addTrailLayer /
// restoreTrailElement). The QRhi view reads the polyline through syncFixedFrame()/points()/pastCount().
class TrailLayer : public Scene3DLayer {
  Q_OBJECT
 public:
  TrailLayer(PJ::ObjectTopicId layer_id, TrailSource source, QObject* parent = nullptr);
  // Restore-path constructor: the source is installed later by xmlLoadStateResult (the dock creates the layer before
  // parsing the payload, as in the RobotModelLayer restore flow).
  explicit TrailLayer(PJ::ObjectTopicId layer_id, QObject* parent = nullptr);
  ~TrailLayer() override;

  // Tri-state full restore from a <trail> payload: resolves and installs the SOURCE (tf frame, or pose topic via the
  // session's ambiguity-safe dataset identity ladder) and applies the style. kDeferred = the pose topic is not loaded
  // yet, or a generic topic name matches several datasets (the dock re-queues the element); kInvalid = malformed.
  // `preferred_dataset` is the dataset the dock shows (uniqueObjectTopic's tie-break; 0 = none).
  enum class XmlLoadResult { kRestored, kDeferred, kInvalid };
  [[nodiscard]] XmlLoadResult xmlLoadStateResult(const QDomElement& element, PJ::DatasetId preferred_dataset = 0);
  // Parse-only checks reporting to an active PJ::XmlOptionsRecorder: validateXml is what xmlLoadStateResult refuses
  // as kInvalid before any data lookup (source and style); validateStyleXml is what xmlLoadState accepts, the check of
  // a pose layer's nested <trail>, whose saved source attributes it reports without judging them.
  [[nodiscard]] static bool validateXml(const QDomElement& element);
  [[nodiscard]] static bool validateStyleXml(const QDomElement& element);

  [[nodiscard]] PJ::SceneLayerInfo info() const override;
  [[nodiscard]] PJ::Range<PJ::Timepoint> timeRange() const override;
  [[nodiscard]] QStringList fallbackFrames() const override;
  [[nodiscard]] SceneContributions sceneContributions() override {
    return {.trail = this};
  }
  [[nodiscard]] QString sourceFrame() const override;
  QDomElement xmlSaveState(QDomDocument& doc) const override;
  // Applies STYLE only (colors/thickness/visibility): a family paste must restyle, never retarget, a trail. The full
  // source-installing restore is xmlLoadStateResult above.
  bool xmlLoadState(const QDomElement& element) override;

  bool attach(const PJ::SceneLayerContext& ctx) override;
  void detach() override;

  void setFixedFrame(const QString& frame) override;
  void setTrackerTime(PJ::Timepoint time) override;
  [[nodiscard]] uint64_t renderKey(PJ::Timepoint time) const override;
  void setVisible(bool visible) override;

  QWidget* createConfigWidget(QWidget* parent) override;
  // Appends this trail's style rows (thickness, past/future colour + eye) to an existing form: split out so an owner
  // that embeds a trail (PosesInFrameLayer) shows the identical controls inside its own settings panel.
  void appendStyleRows(QFormLayout* form, QWidget* parent);

  [[nodiscard]] const TrailSource& source() const {
    return source_;
  }
  // Re-binds the TF buffer after attach: layout restore replays <layer> elements BEFORE the <config_topic> that binds
  // the dock's TF buffer, so a restored trail attaches buffer-less ("waiting for TF") and the dock pushes the buffer
  // here once the binding lands (and null when it resets).
  void setTransformBuffer(std::shared_ptr<TransformBuffer> tf_buffer);
  void setPastColor(QColor color);
  void setFutureColor(QColor color);
  // Line width in px (a screen-space ribbon, so any width is honored exactly), clamped to [1, 8]; persisted per trail.
  void setThickness(float px);
  [[nodiscard]] float thickness() const {
    return thickness_px_;
  }
  // Per-half visibility (the config widget's eye toggles); persisted per trail.
  void setPastVisible(bool visible);
  void setFutureVisible(bool visible);
  [[nodiscard]] bool pastVisible() const {
    return past_visible_;
  }
  [[nodiscard]] bool futureVisible() const {
    return future_visible_;
  }
  [[nodiscard]] QColor pastColor() const {
    return past_color_;
  }
  [[nodiscard]] QColor futureColor() const {
    return future_color_;
  }
  [[nodiscard]] bool visible() const {
    return visible_;
  }

  // Renderer hook, once per painted frame: adopts the view's fixed frame (the
  // authoritative one; attach can precede the dock's fan-out). On a change it
  // requests a rebuild and returns false: points() are stale for this frame.
  bool syncFixedFrame(const std::string& fixed_frame);
  [[nodiscard]] const std::vector<glm::dvec3>& points() const {
    return points_;
  }
  [[nodiscard]] uint64_t buildRevision() const {
    return build_revision_;
  }
  [[nodiscard]] std::size_t pastCount(PJ::Timepoint time) const;

#ifdef PJ_SCENE3D_TEST_HOOKS
  // Bind a TF buffer without a session/dock (attach() is bypassed).
  void injectContextForTest(std::shared_ptr<TransformBuffer> tf_buffer) {
    ctx_.tf_buffer = std::move(tf_buffer);
  }
  void adoptPolylineForTest(TrailPolyline polyline) {
    adoptPolyline(std::move(polyline));
  }
  [[nodiscard]] std::size_t pointCountForTest() const {
    return stamps_.size();
  }
  [[nodiscard]] std::size_t pastCountForTest(PJ::Timepoint time) const {
    return pastCount(time);
  }
  void rebuildNowForTest(const QString& fixed_frame) {
    fixed_frame_ = fixed_frame.toStdString();
    rebuild_worker_.invalidate();
    startRebuild(/*synchronous=*/true);
  }
#endif

 private:
  struct ParsedStyle {
    QColor past_color;
    QColor future_color;
    float thickness = 4.0F;
    bool past_visible = true;
    bool future_visible = true;
  };
  [[nodiscard]] static std::optional<ParsedStyle> parseStyle(const QDomElement& element);
  struct ParsedSource {
    QString kind;  // "tf_frame" or "pose_topic"
    QString frame;
    QString topic_name;
    PJ::DatasetId dataset_id = 0;  // 0: not saved
    QString dataset_source;
    QString dataset_path;
  };
  [[nodiscard]] static std::optional<ParsedSource> parseSource(const QDomElement& element);
  bool loadStyle(const QDomElement& element);
  void adoptPolyline(TrailPolyline polyline);
  void appendTail(TrailPolyline tail);
  // What an off-thread TF-frame rebuild needs, captured by value. The shared_ptr keeps the buffer alive off-thread and
  // TransformBuffer is shared_mutex-protected, so concurrent GUI-thread writes are safe.
  struct TfTrailRequest {
    std::shared_ptr<TransformBuffer> tf;
    std::string fixed_frame;
    std::string frame;
  };
  [[nodiscard]] static TrailPolyline buildTfTrail(const TfTrailRequest& request);
  // Latest-wins async rebuild orchestration (TF async, pose sync); requestRebuild() makes any running build stale.
  void requestRebuild();
  void startRebuild(bool synchronous = false);
  [[nodiscard]] TrailPolyline buildPoseTrailNow();
  // Decodes one store entry into a PoseTrailSample (FIRST pose only). nullopt for evicted/empty/undecodable entries;
  // decode failures (as opposed to absence) increment `decode_failures` for the row warning.
  [[nodiscard]] std::optional<PoseTrailSample> decodePoseSample(
      const PJ::SessionManager::ParserBinding& binding, PJ::SequentialUID uid, std::size_t& decode_failures) const;
  void maybeAppendLiveSamples();

  PJ::ObjectTopicId layer_id_;
  TrailSource source_;
  QString display_name_;
  Scene3DLayerContext ctx_;
  std::string fixed_frame_;  // adopted by syncFixedFrame() on the first painted frame
  bool visible_ = true;

  // Parallel arrays (same length): stamps_ is the binary-search target for the split; points_ mirrors the pass's vertex
  // source so live appends can extend and ring-trim without re-sampling.
  std::vector<PJ::Timepoint> stamps_;
  std::vector<glm::dvec3> points_;
  uint64_t build_revision_ = 0;  // bumped by adoptPolyline + live appends
  int style_revision_ = 0;       // bumped by color edits

  QColor past_color_{0x1F, 0x77, 0xB4};    // #1f77b4 — the mockup blue
  QColor future_color_{0x9E, 0xCA, 0xE1};  // #9ecae1 — light blue
  float thickness_px_ = 4.0F;
  bool past_visible_ = true;
  bool future_visible_ = true;

  LatestWinsWorker<TfTrailRequest, TrailPolyline> rebuild_worker_{
      this, &TrailLayer::buildTfTrail, [this](TrailPolyline polyline) { adoptPolyline(std::move(polyline)); }};
  uint64_t last_seen_tf_revision_ = 0;
  // TransformBuffer::epoch() at the last full build: a replaced history rebuilds.
  uint64_t last_seen_tf_epoch_ = 0;
  std::size_t pose_seen_entry_count_ = 0;  // live-append cursor for pose sources
  // Decode failures of the last pose rebuild: tells "no data yet" from "data arrived but undecodable" in the row
  // warning.
  std::size_t last_pose_decode_failures_ = 0;
};

}  // namespace pj::scene3d
