// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QByteArray>
#include <QColor>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "pj_datastore/object_store.hpp"
#include "pj_scene3d_core/robot_model.h"
#include "pj_scene3d_widgets/mesh_data.h"
#include "pj_scene3d_widgets/model_renderable.h"
#include "pj_scene3d_widgets/scene3d_layer.h"
#include "pj_tf/transform.h"  // StampedTransform (cached fixed-joint bridges)

class QWidget;

namespace pj::tf {
class TransformBuffer;
}  // namespace pj::tf

namespace pj::scene3d {

using pj::tf::StampedTransform;
using pj::tf::TransformBuffer;

class MeshLoadQueue;
class UrdfPackageResolver;

// Scene3DLayer that renders a robot's URDF (links, primitive geometry, and
// async-loaded meshes) posed by the live TF tree.
// It decodes its description at most once per source change and then latches,
// so it is not a hot path.
//
// Three source modes (SourceType):
//   - kTopic: a robot_description object topic in the ObjectStore, decoded
//     through the topic's MessageParser. The first sample is latched; while the
//     topic is still empty the layer waits and retries on each tracker tick.
//   - kFile:  a URDF file. Desktop reads the path; setSourceFileContent() feeds
//     the bytes directly instead (the browser's only route: a browser layout
//     never regains file access, so a restored kFile layer asks the user to
//     select the file again).
//   - kUrl:   a URL fetched ASYNCHRONOUSLY (file/http/https on desktop, http(s)
//     in the browser); the model is applied when the bytes land.
//
// Explicit package URL roots (setPackageUrlRoots) map package:// names to
// directory URLs for THIS model, so a topic URDF, which has no location of its
// own, can still fetch its meshes: in the browser, or from a web-hosted layout
// (on the desktop a layout file's http(s) root is skipped, UrdfPackageResolver).
// They are persisted as the robot_model's `package_url_roots` attribute
// (layout_resources.h), skipped or not.
//
// A robot restored from a web-hosted layout carries `remote_resources_only="true"`
// (stamped by resolveLayoutResourceUrls, then saved with the layer): its URDF's
// mesh references resolve to network resources and embedded assets only, never to
// files on this host (UrdfPackageResolver::resolveUri). Nothing in the UI clears it; loading
// a layout from any other location does.
//
// Coordinate contract. The layer's CPU model is TF-free: one ModelDrawCall per
// link geometry, holding the prefixed link frame plus the link-local
// <origin> × scale; the view applies TF(fixed <- link), exactly once. URDF fixed joints the data
// never publishes are bridged into the TF buffer (see ensureStaticBridges).
//
// Meshes load through a MeshLoadQueue, a few at a time (one in the browser):
// URL meshes are fetched, then imported off the GUI thread, and each result is
// admitted against kSceneLimits' retained-byte budgets as it lands. A source
// switch cancels every queued and running load, so no stale completion lands;
// a superseded import keeps its concurrency slot until its worker finishes.
//
// Parser lifetime (kTopic): parsers are deliberately NOT cached; every decode
// resolves a fresh ParserBinding through ctx_.session.
//
// The layer is deliberately bounds-less: it contributes nothing to the camera's
// scene fit (a robot moved by joints would otherwise pull the camera around).
class RobotModelLayer : public Scene3DLayer, public ModelRenderable {
  Q_OBJECT
 public:
  enum class SourceType { kTopic, kFile, kUrl };
  // kVisual: render only <visual> geometry; kCollision: render only <collision>
  // geometry (through the collision opacity/visibility group). kAuto: a link's
  // <visual> geometry when it has any; a collision-only link is promoted to the
  // visuals group (rendered solid) ONLY when the WHOLE model has no visuals, so an
  // all-collision URDF renders solid instead of ghosting at the collision group's
  // default opacity. In a MIXED model, a collision-only link is
  // auxiliary geometry (e.g. self-collision capsules) and renders in the
  // COLLISION group so the Collision opacity/visibility toggle controls it.
  enum class DisplayMode { kAuto, kVisual, kCollision };
  enum class XmlLoadResult { kRestored, kDeferred, kInvalid };

  RobotModelLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent = nullptr);
  ~RobotModelLayer() override;

  [[nodiscard]] PJ::SceneLayerInfo info() const override;
  [[nodiscard]] PJ::Range<PJ::Timepoint> timeRange() const override;
  [[nodiscard]] QStringList fallbackFrames() const override;
  [[nodiscard]] SceneContributions sceneContributions() override {
    return {.models = this};
  }
  [[nodiscard]] QString sourceFrame() const override;
  QDomElement xmlSaveState(QDomDocument& doc) const override;
  bool xmlLoadState(const QDomElement& element) override;
  [[nodiscard]] XmlLoadResult xmlLoadStateResult(const QDomElement& element);
  // Session-free structural check of a "robot_model" payload (xmlLoadState
  // additionally resolves the saved topic identity against the session).
  [[nodiscard]] static bool validateXml(const QDomElement& element);

  bool attach(const PJ::SceneLayerContext& ctx) override;
  void detach() override;

  void setFixedFrame(const QString& frame) override;
  void setTrackerTime(PJ::Timepoint time) override;
  void setVisible(bool visible) override;

  QWidget* createConfigWidget(QWidget* parent) override;

  void setPackageResolver(UrdfPackageResolver* resolver);
  // The buffer that fixed-joint bridges are injected into (the one the view
  // resolves TF from).
  void setTransformBuffer(std::shared_ptr<TransformBuffer> buffer);
  // In-band mesh assets (name == verbatim mesh ref) offered to the resolver on
  // the next parse.
  void setEmbeddedAssets(QMap<QString, QByteArray> assets);
  void setSourceTopic(PJ::ObjectTopicId topic_id, QString display_name = {});
  void setSourceFile(QString path);
  // kFile from bytes: `filename` is only a label (persisted as source_value);
  // the bytes are never persisted.
  void setSourceFileContent(QString filename, QByteArray bytes);
  void setSourceUrl(QString url);
  // Package name -> directory URL, consulted before the resolver's heuristics
  // and authoritative for the packages it names. A URL source's relative roots
  // resolve against its fetched URDF's final URL; other sources need absolute
  // roots (resolveLayoutResourceUrls makes a layout's absolute). A change
  // reloads from the current source.
  void setPackageUrlRoots(const QMap<QString, QUrl>& roots);
  void setFramePrefix(QString prefix);
  void setDisplayMode(DisplayMode mode);
  void setFallbackColor(QColor color);
  // Toggle the COLLADA up-axis override. When true, .dae/.collada meshes load
  // with the Y->Z flip forced OFF; other formats are unaffected. Changing the
  // value reloads from the current source (clearing the loader cache).
  void setIgnoreColladaUpAxis(bool ignore);
  // Reload from the current source (Retry).
  void retry();

  [[nodiscard]] SourceType sourceType() const {
    return source_type_;
  }
  [[nodiscard]] QString sourceValue() const {
    return source_value_;
  }
  [[nodiscard]] QString framePrefix() const {
    return frame_prefix_;
  }
  [[nodiscard]] DisplayMode displayMode() const {
    return display_mode_;
  }
  [[nodiscard]] QColor fallbackColor() const {
    return fallback_color_;
  }
  [[nodiscard]] bool ignoreColladaUpAxis() const {
    return ignore_collada_up_axis_;
  }
  [[nodiscard]] QString statusText() const {
    return status_text_;
  }
  [[nodiscard]] const RobotModel* robotModel() const {
    return model_.has_value() ? &*model_ : nullptr;
  }
  [[nodiscard]] int totalMeshCount() const {
    return total_mesh_count_;
  }
  [[nodiscard]] int unresolvedMeshCount() const {
    return unresolved_mesh_count_;
  }
  [[nodiscard]] int loadedMeshCount() const {
    return loaded_mesh_count_;
  }
  [[nodiscard]] std::size_t fixedJointBridgeCount() const {
    return static_bridges_.size();
  }
  [[nodiscard]] QString linkFrameName(const std::string& link_name) const;

  // ModelRenderable (read by the view): link-local
  // draws; the view applies TF(fixed <- frame_id).
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
  [[nodiscard]] float opacity() const override {
    return 1.0F;
  }
  [[nodiscard]] bool colorOverrideEnabled() const override {
    return false;
  }
  [[nodiscard]] QColor overrideColor() const override {
    return fallback_color_;
  }
  [[nodiscard]] bool wireframe() const override {
    return false;
  }
  [[nodiscard]] const std::string& renderFrameId() const override {
    return source_frame_;
  }
  [[nodiscard]] bool contributesToSceneBounds() const override {
    return false;
  }
  [[nodiscard]] Scene3DLayer& modelLayer() override {
    return *this;
  }

  // Mesh imports started whose worker has not finished yet (superseded ones
  // included): what actually runs on the pool.
  [[nodiscard]] std::size_t meshImportsOutstandingForTest() const;

#ifdef PJ_SCENE3D_TEST_HOOKS
  // Injects the fixed-joint TF bridges into `tf` (the buffer the layer renders
  // against). Inline: test hooks are compiled only into the test TU, never the library.
  void ensureStaticBridgesForTest(TransformBuffer& tf) {
    ensureStaticBridges(tf);
  }
  // Draw-group sizes of the TF-free model — lets a test assert which group
  // (visuals vs collision) a link's geometry landed in.
  [[nodiscard]] int visualDrawCountForTest() const {
    return static_cast<int>(model_draw_calls_.size()) - collisionDrawCountForTest();
  }
  [[nodiscard]] int collisionDrawCountForTest() const {
    return static_cast<int>(std::count_if(model_draw_calls_.begin(), model_draw_calls_.end(), [](const auto& call) {
      return call.group == ModelDrawGroup::kCollision;
    }));
  }
  [[nodiscard]] glm::vec4 firstCollisionDrawColorForTest() const {
    for (const ModelDrawCall& call : model_draw_calls_) {
      if (call.group == ModelDrawGroup::kCollision) {
        return call.override_color;
      }
    }
    return glm::vec4(0.0f);
  }
#endif

 signals:
  // Mesh-load progress. UNIT: all three of loaded/total/unresolved count
  // per-GEOMETRY mesh references, NOT unique files. (unresolved_packages, by
  // contrast, is the distinct-package list.)
  void meshLoadStatusChanged(int loaded, int total, QStringList unresolved_packages);
  void unresolvedPackages(QStringList packages);
  void statusTextChanged(const QString& status);

 private:
  // The checked attributes of a "robot_model" payload (validateXml()); the free
  // text ones (source_value, frame_prefix) are read as they are.
  struct ParsedSettings {
    SourceType source_type = SourceType::kTopic;
    DisplayMode display_mode = DisplayMode::kAuto;
    bool visible = true;
    bool ignore_collada_up_axis = false;
    std::optional<QColor> color;
    QString topic_name;            // non-empty exactly when the payload names a topic
    PJ::DatasetId dataset_id = 0;  // 0: none saved
    QString dataset_source;
    QString dataset_path;
    QMap<QString, QUrl> package_url_roots;
    bool remote_resources_only = false;
  };
  [[nodiscard]] static std::optional<ParsedSettings> parseSettings(const QDomElement& element);

  // Rebuild the ModelDrawCalls from the model, display mode and loaded meshes.
  // TF-independent: runs only when those change.
  void rebuildDrawCalls();
  // Recompute static_bridges_ from the current model_, with frame_prefix_ applied
  // to each parent/child frame. Called when the model loads or the prefix changes.
  void rebuildStaticBridges();
  // Inject any not-yet-present fixed-joint bridge into `buf`. Idempotent and
  // guarded (never overrides real /tf), so it is re-run on every tracker tick
  // and self-heals after a buffer clear (same-file reload).
  void ensureStaticBridges(TransformBuffer& buf);
  void ensureStaticBridges();
  void resetModelState();
  // SessionManager::datasetAboutToBeReplaced: a topic-backed model belongs to the
  // bytes being swapped out, so it goes now and reloads from the replacement.
  void onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id);
  bool loadFromCurrentSource();
  bool tryLoadTopicDescription();
  // `roots_base` anchors relative package roots (a URL source's final URL).
  bool applyRobotDescription(
      const QString& text, const QString& format, const QString& label, const QString& urdf_dir, bool source_is_url,
      const QUrl& roots_base = {});
  void queueMeshLoads();
  // A mesh load of the current source settled: admit it against the budget.
  void admitMesh(const std::string& key, MeshData mesh);
  // A mesh load settled (loaded or failed): refresh draws, counters, status.
  void onMeshLoadSettled();
  [[nodiscard]] bool meshReady(const std::string& key) const;
  // Single source of truth for the three per-reference mesh counters.
  void updateMeshCounters();
  // Recompute status_text_ and the data warning from the loaded model.
  void updateStatus();
  // The status text carries the renderer warning too.
  void renderWarningChanged() override {
    updateStatus();
  }
  // A terminal failure: status and warning both carry `message`.
  void fail(const QString& message);
  void setStatus(QString status);
  // Distinct unresolved package names for THIS model, in first-seen order.
  [[nodiscard]] QStringList unresolvedPackagesList() const;
  // " • N http refs blocked / N absolute paths missing": unresolved kinds Locate
  // cannot fix. Empty when there are none.
  [[nodiscard]] QString unresolvedIssueClause() const;
  [[nodiscard]] QString loadFailureClause() const;

  PJ::ObjectTopicId topic_id_;
  PJ::ObjectTopicId source_topic_id_;
  QString display_name_;
  Scene3DLayerContext ctx_;
  QMetaObject::Connection reload_connection_;
  std::shared_ptr<TransformBuffer> tf_buffer_;

  SourceType source_type_{SourceType::kTopic};
  QString source_value_;
  QByteArray local_file_bytes_;
  QMap<QString, QByteArray> embedded_assets_;
  QMap<QString, QUrl> package_url_roots_;
  bool remote_resources_only_{false};
  QString frame_prefix_;
  DisplayMode display_mode_{DisplayMode::kAuto};
  QColor fallback_color_{178, 178, 178};
  bool ignore_collada_up_axis_{false};
  bool visible_{true};
  QString fixed_frame_;
  // "URDF: <label>" once a model is applied; empty while none is.
  QString base_status_;
  QString status_text_;
  bool latch_pending_{false};
  std::chrono::steady_clock::time_point last_latch_retry_{};
  PJ::Timepoint tracker_time_{};

  std::optional<RobotModel> model_;
  std::string source_frame_;
  // Cached fixed-joint TF bridges (frame_prefix_ already applied), rebuilt on
  // model load / prefix change. Empty when the model has no fixed joints.
  std::vector<StampedTransform> static_bridges_;
  // True iff any link has visual geometry; gates kAuto's collision-only promotion.
  bool model_has_visuals_{false};
  int total_mesh_count_{0};
  int unresolved_mesh_count_{0};
  int loaded_mesh_count_{0};

  std::unique_ptr<UrdfPackageResolver> owned_resolver_;
  UrdfPackageResolver* resolver_{nullptr};
  // Fetches the URDF and loads the meshes; destroying it (or a source switch)
  // aborts in-flight fetches and drops their callbacks.
  std::unique_ptr<MeshLoadQueue> mesh_queue_;
  std::unordered_map<std::string, QString> mesh_errors_;
  // Retained estimate of each admitted mesh, keyed by resolved path; the key
  // set is the ready set.
  std::unordered_map<std::string, std::uint64_t> mesh_bytes_;
  std::uint64_t mesh_retained_bytes_{0};
  // Bumped by every source (re)load and detach; a URDF fetch or deferred reload
  // from an older generation is dropped.
  std::uint64_t load_generation_{0};

  // What the view reads (ModelRenderable).
  std::unordered_map<std::string, std::shared_ptr<const MeshData>> model_meshes_;
  std::vector<ModelDrawCall> model_draw_calls_;
  std::uint64_t model_revision_{0};
};

}  // namespace pj::scene3d
