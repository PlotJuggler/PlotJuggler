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
#include "pj_scene_common/scene_dock_widget.h"

class QEvent;
class QLabel;
class QStackedWidget;

namespace PJ {

class CompositeMediaSource;
class MediaViewerWidget;
class SessionManager;

/// 2D scene dock on top of pj_scene_common's layered SceneDockWidget.
///
/// Registers image/video/depth/annotation/entity layer types, wires their
/// MediaSources into a CompositeMediaSource, and nudges live layers to the newest
/// ObjectStore sample when streaming data arrives.
class Scene2DDockWidget : public SceneDockWidget {
  Q_OBJECT
 public:
  explicit Scene2DDockWidget(QWidget* parent = nullptr);
  ~Scene2DDockWidget() override;

  /// Saves/restores the base layer stack plus the 2D canvas zoom/pan viewport.
  QDomElement xmlSaveState(QDomDocument& doc) const override;
  bool xmlLoadState(const QDomElement& element) override;

  /// Stores a non-owning SessionManager pointer in the base and reconnects the
  /// live-sample follow connection. Replacing the session drops the old connection.
  void setSessionManager(SessionManager* session) override;
  /// Interactive drop: overlays (annotations/markers) stack, but a full-frame
  /// image/depth/video replaces the current background and is drawn beneath the
  /// overlays. The old background is removed only after the new one attached.
  /// addTopic() stays a plain append (workspace restore, programmatic stacking).
  bool tryAcceptObjectTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) override;
  /// Adds a 2D render layer for the topic. Returns false when the object type is
  /// unsupported or layer attach fails; success also updates the dock title.
  bool setImageTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title);
  /// Forwards the global point-inspector toggle to the viewer if it already exists;
  /// a pre-create call is intentionally not latched.
  void setPointInspectorEnabled(bool enabled);
  [[nodiscard]] bool pointInspectorEnabled() const noexcept;

  /// Clockwise quarter-turn applied to every layer: 0, 90, -90 or 180; other
  /// values are ignored. A real change is a workspace mutation (undo, save).
  void setViewRotation(int degrees);
  [[nodiscard]] int viewRotation() const;

  /// Single source of truth for the canonical object types the 2D scene family
  /// handles (render layers; the 2D family has no scene-wide config topics).
  /// Host-side drop routing and acceptsObjectType() both read this.
  [[nodiscard]] static bool handlesObjectType(sdk::BuiltinObjectType object_type);

  [[nodiscard]] size_t compositeLayerCountForTesting() const noexcept;
  /// Mirrors the visible CompositeMediaSource layer order after syncViewLayers().
  [[nodiscard]] std::vector<ObjectTopicId> compositeTopicOrderForTesting() const;
  /// True while the empty-state placeholder (the greyed image icon) should be
  /// shown instead of the GPU viewer — i.e. there are no visible layers.
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
  /// Builds the native QRhi bootstrap where required plus the real
  /// MediaViewerWidget, fronted by a stacked empty-state placeholder shown until
  /// the first layer arrives.
  QWidget* createSceneView() override;
  /// Re-tints the placeholder icon when the palette/theme changes.
  void changeEvent(QEvent* event) override;
  /// Supplies the current session pointer to Scene2DLayer::attach().
  std::unique_ptr<SceneLayerContext> makeContext() override;
  /// Keeps generic SceneDockWidget routing aligned with the static host classifier.
  [[nodiscard]] bool acceptsObjectType(sdk::BuiltinObjectType object_type) const override;
  /// Rebuilds the visible-layer CompositeMediaSource in draw order and repoints
  /// the viewer before the old composite is destroyed.
  void syncViewLayers(const std::vector<ISceneLayer*>& ordered_layers) override;
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
  /// Full-frame (image/depth/video) layers, in draw order.
  [[nodiscard]] std::vector<ObjectTopicId> backgroundTopics() const;
  /// Makes `topic_id` the only background, drawn beneath the overlays.
  void promoteBackground(ObjectTopicId topic_id);
  void primeRestoreLayerKind(const QDomElement& layer_element);
  /// Builds the centered, greyed image-SVG placeholder shown while the dock is
  /// empty (a nicer "drop a topic here" affordance than a blank GPU surface).
  QWidget* makeEmptyPlaceholder(QWidget* parent);
  /// Re-renders the placeholder icon for the active theme (LoadSvg ink tint).
  void retintEmptyPlaceholder();
  /// Switches the stacked view between the placeholder and the viewer to match
  /// empty_placeholder_active_. No-op until createSceneView() has run.
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

  // Native-only zero-size QRhiWidget kept as a child so Qt 6.8 creates an
  // RHI-backed backing store on first show(); the browser deliberately leaves
  // this null. See TECHNICAL_NOTES.md "QRhiWidget Multi-Instance Lifecycle".
  MediaViewerWidget* bootstrap_ = nullptr;
  // The real viewer is non-owning here; Qt parent ownership is the container made
  // by createSceneView(), while composite_ owns the source it polls.
  MediaViewerWidget* viewer_ = nullptr;
  // Stacked front for the scene view: page 0 is the empty-state placeholder page
  // (built by makeEmptyPlaceholder(), holding empty_placeholder_icon_), page 1 is
  // the viewer. Stacking (not overlaying) avoids compositing a raster label over
  // the QRhiWidget's surface. Both null until createSceneView() runs.
  QStackedWidget* view_stack_ = nullptr;
  QLabel* empty_placeholder_icon_ = nullptr;
  // Whether the empty-state placeholder should front the viewer (no visible
  // layers). Tracked independently of the (lazily created) view so it is correct
  // the moment createSceneView() runs.
  bool empty_placeholder_active_ = true;
  std::unique_ptr<CompositeMediaSource> composite_;
  // Live-follow subscription; reconnectLiveSamples owns disconnect/replacement.
  QMetaObject::Connection live_samples_conn_;
  // Visible topic ids in the exact order pushed into composite_; testing reads it
  // to verify layer reorder/add/remove reconciliation.
  std::vector<ObjectTopicId> composite_topic_order_;
  std::unordered_map<int64_t, ImageLayerKind> restore_image_layer_kinds_;
};

}  // namespace PJ
