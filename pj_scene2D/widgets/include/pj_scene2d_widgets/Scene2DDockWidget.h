#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QMetaObject>
#include <QWidget>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_scene2d_core/media_frame.h"
#include "pj_scene_common/scene_dock_widget.h"

class QEvent;
class QLabel;
class QStackedWidget;

namespace PJ {

class CompositeMediaSource;
class MediaViewerWidget;
class Scene2DLayer;
class SessionManager;

/// The 2D scene dock: its layers' MediaSources feed one CompositeMediaSource, and live layers follow the newest
/// ObjectStore sample as streaming data arrives.
class Scene2DDockWidget : public SceneDockWidget {
  Q_OBJECT
 public:
  explicit Scene2DDockWidget(QWidget* parent = nullptr);
  ~Scene2DDockWidget() override;

  /// Saves/restores the base layer stack plus the 2D canvas zoom/pan viewport.
  QDomElement xmlSaveState(QDomDocument& doc) const override;
  bool xmlLoadState(const QDomElement& element) override;
  /// The checks xmlLoadState makes before it touches the dock: the root, the layer elements (identity attributes and
  /// each payload) and the view. Parse-only.
  [[nodiscard]] bool xmlValidateState(const QDomElement& element) const override;

  /// Stores a non-owning SessionManager pointer in the base and reconnects the
  /// live-sample follow connection. Replacing the session drops the old connection.
  void setSessionManager(SessionManager* session) override;
  /// Interactive drop: overlays stack, a full-frame image, depth or video replaces the background beneath them (the old
  /// one is removed only after the new one attached). addTopic() always appends.
  bool tryAcceptObjectTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) override;
  /// Adds a 2D render layer for the topic. Returns false when the object type is
  /// unsupported or layer attach fails; success also updates the dock title.
  bool setImageTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title);
  /// Full-frame (image/depth/video) layers, in draw order; an interactive drop leaves at most one.
  [[nodiscard]] std::vector<ObjectTopicId> backgroundTopics() const;
  /// Forwards the global point-inspector toggle to the viewer if it already exists;
  /// a pre-create call is intentionally not latched.
  void setPointInspectorEnabled(bool enabled);
  [[nodiscard]] bool pointInspectorEnabled() const noexcept;

  /// Clockwise quarter-turn applied to every layer: 0, 90, -90 or 180; other
  /// values are ignored. A real change is a workspace mutation (undo, save).
  void setViewRotation(int degrees);
  [[nodiscard]] int viewRotation() const;

  /// Host-side drop routing and acceptsObjectType() both read this.
  [[nodiscard]] static bool handlesObjectType(sdk::BuiltinObjectType object_type);

  [[nodiscard]] size_t compositeLayerCountForTesting() const noexcept;
  /// Mirrors the visible CompositeMediaSource layer order after syncViewLayers().
  [[nodiscard]] std::vector<ObjectTopicId> compositeTopicOrderForTesting() const;
  /// Polls the composite as the viewer does, without a GPU (nullopt: nothing new, or no composite).
  [[nodiscard]] std::optional<MediaFrame> takeCompositeFrameForTesting();
  /// True while there are no visible layers.
  [[nodiscard]] bool emptyPlaceholderActiveForTesting() const noexcept {
    return empty_placeholder_active_;
  }

 signals:
  /// Emitted whenever the rotation changes, including on workspace restore/undo.
  void viewRotationChanged(int degrees);

 protected:
  /// Workspace XML tag for the 2D scene dock.
  [[nodiscard]] QString xmlTag() const override;
  [[nodiscard]] bool acceptsStateChildTag(const QString& tag) const override;
  QWidget* createSceneView() override;
  void changeEvent(QEvent* event) override;
  /// Supplies the current session pointer to Scene2DLayer::attach().
  std::unique_ptr<SceneLayerContext> makeContext() override;
  /// Keeps generic SceneDockWidget routing aligned with the static host classifier.
  [[nodiscard]] bool acceptsObjectType(sdk::BuiltinObjectType object_type) const override;
  /// Rebuilds the visible-layer CompositeMediaSource in draw order and repoints
  /// the viewer before the old composite is destroyed.
  void syncViewLayers(const std::vector<ISceneLayer*>& ordered_layers) override;
  /// Pairs `layer`'s contribution to `composite` (tagged `identity`) with its store topic, the cadence of its entries
  /// and its row warning (CompositeMediaSource::setLayerPairing); its annotations are checked only when the topic is
  /// an on-demand derived one.
  static void setLayerPairing(
      CompositeMediaSource& composite, SessionManager& session, ObjectTopicId topic, const void* identity,
      Scene2DLayer* layer);
  /// Repaints only the viewer; the layer list UI is owned by the base dock.
  void refreshView() override;
  /// Re-applies an explicit saved Image-vs-Depth concrete kind when a deferred
  /// kImage topic finally becomes available.
  bool restoreOnePending(const QDomElement& element) override;

 private:
  enum class ImageLayerKind { kImage, kDepth };

  /// Resolves saved layer identities that are already available and primes the
  /// encoding-aware factory with their explicit concrete kind.
  void primeRestoreLayerKinds(const QDomElement& root);
  /// Makes `topic_id` the only background, drawn beneath the overlays.
  void promoteBackground(ObjectTopicId topic_id);
  void primeRestoreLayerKind(const QDomElement& layer_element);
  QWidget* makeEmptyPlaceholder(QWidget* parent);
  void retintEmptyPlaceholder();
  /// No-op until createSceneView() has run.
  void applyEmptyPlaceholderState();
  /// Connects live ObjectStore ingestion to jump visible layers to the data edge.
  void reconnectLiveSamples(SessionManager* session);
  /// Drives visible layers and the 2D composite to the newest stored sample.
  void driveVisibleLayersToLiveEdge();
  /// Seeds a freshly rebuilt composite from the last tracker/live time, or from
  /// the first visible layer with a retained tracker timestamp.
  void syncCompositeTimestamp(const std::vector<ISceneLayer*>& ordered_layers);
  /// Returns raw nanoseconds because CompositeMediaSource is the core-side seam.
  [[nodiscard]] std::optional<int64_t> seedTimestampNs(const std::vector<ISceneLayer*>& ordered_layers) const;

  // The real viewer is non-owning here; Qt parent ownership is the container made
  // by createSceneView(), while composite_ owns the source it polls.
  MediaViewerWidget* viewer_ = nullptr;
  // Both null until createSceneView() runs.
  QStackedWidget* view_stack_ = nullptr;
  QLabel* empty_placeholder_icon_ = nullptr;
  // Tracked apart from the lazily created view, so it is right when createSceneView() runs.
  bool empty_placeholder_active_ = true;
  std::unique_ptr<CompositeMediaSource> composite_;
  // Live-follow subscription; reconnectLiveSamples owns disconnect/replacement.
  QMetaObject::Connection live_samples_conn_;
  // Visible topic ids in composite_'s order.
  std::vector<ObjectTopicId> composite_topic_order_;
  std::unordered_map<int64_t, ImageLayerKind> restore_image_layer_kinds_;
};

}  // namespace PJ
