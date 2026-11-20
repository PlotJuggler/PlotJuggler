#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDomDocument>
#include <QDomElement>
#include <QObject>
#include <QString>
#include <QWidget>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/time.hpp"
#include "pj_datastore/object_store.hpp"

namespace PJ {

class SessionManager;

/// Stable snapshot of one layer's identity and presentation state.
struct SceneLayerInfo {
  ObjectTopicId topic_id;
  sdk::BuiltinObjectType object_type = sdk::BuiltinObjectType::kNone;
  QString display_name;
  QString family_name;
  bool visible = true;
};

/// Key of the per-topic layer and orphan maps shared by the base dock and its subclasses (the uint32 topic id
/// widened to int64); one definition so both sides key the same way.
[[nodiscard]] inline int64_t topicKey(ObjectTopicId topic_id) {
  return static_cast<int64_t>(topic_id.id);
}

/// Non-owning services passed to a layer when it is attached to a scene dock. Families derive from it to expose
/// renderer-specific dependencies without making ISceneLayer depend on a backend.
struct SceneLayerContext {
  SessionManager* session = nullptr;
  virtual ~SceneLayerContext() = default;
};

/// Live time range of one ObjectStore topic, in the inverted-empty form `ISceneLayer::timeRange()` expects
/// (`{Timepoint::max(), Timepoint::min()}` when `store` is null or the topic has no entries). Single-topic layers
/// return it verbatim from `timeRange()`: it is the store as it stands now, never a cached last-render range, which
/// would pin the dock's scrub clamp to an evicted time during streaming and freeze the scene.
[[nodiscard]] PJ::Range<PJ::Timepoint> liveTopicTimeRange(const ObjectStore* store, ObjectTopicId topic_id);

/// `renderKey()` of a layer with no active sample at the queried time, shared by every family's stamp-based
/// override. The dock mixes each layer's key with its topic id, so a real stamp equal to this value is not confused
/// with another layer's no-sample state.
inline constexpr uint64_t kNoSampleRenderKey = 0x0D15EA5EDEADBEEFULL;

/// `renderKey()` of a single-topic layer: the SequentialUID of the store entry active at `time`, or
/// kNoSampleRenderKey when there is none (or no store). Metadata only (no payload bytes are resolved), and the UID
/// rather than the stamp, so a same-stamp replacement entry changes it.
[[nodiscard]] uint64_t latestEntryRenderKey(const ObjectStore* store, ObjectTopicId topic_id, PJ::Timepoint time);

/// The writers of one layer's row warning. Each owns a part; the shown text is the non-empty parts joined one per
/// line in enum order, so no writer overwrites another.
class SceneWarningParts {
 public:
  enum class Part : std::size_t {
    kData,        ///< decode and admission problems of the layer's own data
    kRender,      ///< a GPU-side failure
    kDerived,     ///< why a derived topic shows nothing: a missing input or an error
    kAnnotation,  ///< annotations hidden because they are not paired with the shown image (Scene2D)
  };

  /// Returns true when the joined text changed (the caller then notifies).
  bool set(Part part, QString text);
  [[nodiscard]] const QString& text(Part part) const {
    return parts_[static_cast<std::size_t>(part)];
  }
  [[nodiscard]] const QString& combined() const {
    return combined_;
  }

 private:
  std::array<QString, 4> parts_;
  QString combined_;
};

/// What a scene shows beside a layer on an on-demand derived topic (docs/derived_objects/ARCHITECTURE.md "Scene
/// integration"), from its node's state. Every field is empty for any other topic.
struct DerivedTopicPresentation {
  QString warning;  ///< why it shows nothing: the node's missing input or error, a fault
  /// Not a fault: "behind 0.4 s" while the shown result lags the one the current request produces (the scheduler's
  /// lag, never the cursor minus a stamp), "computing…" before any result is known, and a finding's computation time
  /// in display seconds ("computed at 12.4 s").
  QString status;
  std::optional<Timestamp> lag;            ///< set, and positive, while a newer result computes
  bool computing = false;                  ///< the node's newest request has not settled
  std::optional<Timestamp> finding_stamp;  ///< a finding's real stamp: its store entry sits at kFindingTimestamp

  bool operator==(const DerivedTopicPresentation&) const = default;
};

/// The presentation of `topic_id`; empty when `session` is null or the topic is no on-demand output. GUI thread.
[[nodiscard]] DerivedTopicPresentation derivedTopicPresentation(SessionManager* session, ObjectTopicId topic_id);

/// The live topic an unqualified (generic-layout) saved reference names: the only topic with this name and builtin
/// type, else the only one of them in `preferred_dataset` (the dataset the dock shows; 0 = none). nullopt when none or
/// several match: an ambiguous reference is unresolved, not malformed, so restore code keeps it pending (as plots keep
/// an ambiguous curve) instead of failing the layout, and never binds an arbitrary dataset.
[[nodiscard]] std::optional<ObjectTopicId> uniqueObjectTopic(
    const ObjectStore& store, std::string_view topic_name, sdk::BuiltinObjectType object_type,
    DatasetId preferred_dataset = 0);

/// Every live topic with this name and builtin type, in store order: the candidates of uniqueObjectTopic.
[[nodiscard]] std::vector<ObjectTopicId> objectTopicsMatching(
    const ObjectStore& store, std::string_view topic_name, sdk::BuiltinObjectType object_type);

/// The live topic a saved object reference names. Without a saved dataset (`qualified` false): uniqueObjectTopic.
/// With one: `topic_name` in `dataset`, the dataset the qualifier resolved to; nullopt when it resolved to none.
[[nodiscard]] std::optional<ObjectTopicId> resolveSavedObjectTopic(
    const ObjectStore& store, std::string_view topic_name, sdk::BuiltinObjectType object_type, bool qualified,
    std::optional<DatasetId> dataset, DatasetId preferred_dataset);

/// Backend-neutral contract for one object topic in a layered scene. A layer is attached before use, updated by the
/// dock as tracker time and visibility change, and detached before removal or destruction.
class ISceneLayer : public QObject {
  Q_OBJECT
 public:
  explicit ISceneLayer(QObject* parent = nullptr);
  ~ISceneLayer() override;

  /// Returns the current layer metadata used by dock and list UI.
  [[nodiscard]] virtual SceneLayerInfo info() const = 0;

  /// The valid data range as an absolute interval, or the inverted `{Timepoint::max(), Timepoint::min()}` when the
  /// layer carries no data (a static layer positioned purely by TF); the dock detects it with `range.max <
  /// range.min`. Must be the data as it stands now (for a store-backed layer the live store range, never a value
  /// cached at the last render): the dock clamps the tracker to it, so a stale range pins scrubbing to an evicted
  /// time and freezes the scene while streaming. Single-topic layers delegate to `liveTopicTimeRange()`.
  [[nodiscard]] virtual PJ::Range<PJ::Timepoint> timeRange() const = 0;

  /// Binds the layer to shared scene services. Called once before updates; false rejects the layer.
  virtual bool attach(const SceneLayerContext& ctx) = 0;

  /// Releases resources acquired by attach(), before the layer is dropped.
  virtual void detach() = 0;

  /// Moves the layer to the current tracker Timepoint after dock-level clamping.
  virtual void setTrackerTime(PJ::Timepoint time) = 0;

  /// `setTrackerTime` after the store changed under an unchanged `time` (a derived result landed): a layer that
  /// skips a repeated time overrides it to drop that memo first, only when the entry it shows changed. Returns
  /// whether the layer may draw something new (the dock then forgets its last painted key).
  virtual bool refreshAt(PJ::Timepoint time) {
    setTrackerTime(time);
    return true;
  }

  /// The row warning: the parts its writers set (SceneWarningParts), joined.
  [[nodiscard]] const QString& warning() const {
    return warning_parts_.combined();
  }
  /// Sets the derived part of the warning: why a derived topic shows nothing. The dock sets it from the node's
  /// state when the layer is added and on every state change, never per frame.
  void setDerivedWarning(QString text) {
    setWarningPart(SceneWarningParts::Part::kDerived, std::move(text));
  }

  /// Fingerprint of what this layer would render at `time`: its active data sample plus any transform that
  /// positions it. The dock skips the repaint when the combined key of the visible layers is unchanged across
  /// tracker ticks (SceneDockWidget "Repaint coalescing" in docs/ARCHITECTURE.md). Must be cheap (no decode) and
  /// must change whenever the rendered output at `time` would; a key that misses a change freezes the scene. The
  /// default returns the tracker time itself ("always changed"): safe, no coalescing.
  [[nodiscard]] virtual uint64_t renderKey(PJ::Timepoint time) const {
    return static_cast<uint64_t>(PJ::toRaw(time));
  }

  /// Implementations emit visibilityChanged on change.
  virtual void setVisible(bool visible) = 0;

  /// Updates the fixed frame used by layers that depend on transforms.
  virtual void setFixedFrame(const QString& frame);

  /// The layer's settings UI, owned through `parent`.
  virtual QWidget* createConfigWidget(QWidget* parent) = 0;

  /// Layer-specific configuration, saved inside the dock's layer node; a null element saves none.
  virtual QDomElement xmlSaveState(QDomDocument& doc) const;

  /// Restores xmlSaveState(); false rejects the payload.
  virtual bool xmlLoadState(const QDomElement& element);

 signals:
  void infoChanged();
  void visibilityChanged(bool visible);
  /// Emitted after a user commits a persistent layer-parameter edit.
  void configurationChanged();
  void repaintRequested();
  void warningChanged(bool warn, QString reason);

 protected:
  void setWarningPart(SceneWarningParts::Part part, QString text);
  [[nodiscard]] const SceneWarningParts& warningParts() const {
    return warning_parts_;
  }
  /// After `warning()` changed: emits warningChanged, which the dock relays to the row. A family that combines the
  /// warning with its own row state overrides it.
  virtual void onWarningChanged();

 private:
  SceneWarningParts warning_parts_;
};

}  // namespace PJ
