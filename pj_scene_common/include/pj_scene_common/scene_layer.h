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
#include <utility>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/time.hpp"  // PJ::Timepoint, PJ::Range, fromRaw/toRaw
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

/// Single keying convention for the per-topic layer/orphan maps shared by the
/// base dock and its subclasses (the uint32 topic id widened to int64). One
/// definition so the dock's layer map and a subclass's orphan map provably key
/// the same way; if the convention ever changes (e.g. to disambiguate
/// dataset+topic) both sides change together.
[[nodiscard]] inline int64_t topicKey(ObjectTopicId topic_id) {
  return static_cast<int64_t>(topic_id.id);
}

/// Non-owning services passed to a layer when it is attached to a scene dock.
///
/// Scene families can derive from this context to expose renderer-specific
/// dependencies while keeping ISceneLayer independent from 2D/3D backends.
struct SceneLayerContext {
  SessionManager* session = nullptr;
  virtual ~SceneLayerContext() = default;
};

/// Live time range of a single ObjectStore topic, in the inverted-empty form
/// `ISceneLayer::timeRange()` expects (`{Timepoint::max(), Timepoint::min()}`
/// when `store` is null or the topic has no entries). Single-topic layers should
/// return this verbatim from `timeRange()`: it reflects the store as it stands
/// now, never a cached last-render range — a stale range pins the dock's scrub
/// clamp to an evicted time during streaming and freezes the scene.
[[nodiscard]] PJ::Range<PJ::Timepoint> liveTopicTimeRange(const ObjectStore* store, ObjectTopicId topic_id);

/// Sentinel `renderKey()` contribution for "this layer has no active sample at the
/// queried time" — a stable value distinct from a stamp-derived key. Shared so every
/// family's stamp-based `renderKey` override returns the SAME no-sample key (the dock
/// XORs each layer's key with its topic id, so a real stamp that happened to equal
/// this value still cannot be confused with another layer's no-sample state).
inline constexpr uint64_t kNoSampleRenderKey = 0x0D15EA5EDEADBEEFULL;

/// `renderKey()` of a single-topic layer: the SequentialUID of the store entry
/// active at `time`, or kNoSampleRenderKey when there is none (or no store).
/// Metadata-only (latestEntryIdAt never resolves payload bytes), and the UID
/// rather than the stamp, so a same-stamp replacement entry still changes it.
[[nodiscard]] uint64_t latestEntryRenderKey(const ObjectStore* store, ObjectTopicId topic_id, PJ::Timepoint time);

/// The writers of one layer's row warning. Each owns a part; the shown text is the non-empty parts joined one per
/// line in enum order, so no writer overwrites another.
class SceneWarningParts {
 public:
  enum class Part : std::size_t {
    kData,        ///< decode and admission problems of the layer's own data
    kRender,      ///< a GPU-side failure
    kDerived,     ///< why a derived topic shows nothing: a missing input or an error (D6)
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

/// What a scene shows beside a layer on an on-demand derived topic (docs/derived_objects/ARCHITECTURE.md §17), from
/// its node's state. Every field is empty for any other topic.
struct DerivedTopicPresentation {
  QString warning;  ///< why it shows nothing: the node's missing input or error, a fault
  /// Not a fault: "behind 0.4 s" while the shown result lags the one the current request produces (D30, the
  /// scheduler's lag, never the cursor minus a stamp), "computing…" before any result is known, and a finding's
  /// computation time in display seconds ("computed at 12.4 s", D26).
  QString status;
  std::optional<Timestamp> lag;            ///< set, and positive, while a newer result computes
  bool computing = false;                  ///< the node's newest request has not settled
  std::optional<Timestamp> finding_stamp;  ///< a finding's real stamp: its store entry sits at kFindingTimestamp

  bool operator==(const DerivedTopicPresentation&) const = default;
};

/// The presentation of `topic_id`; empty when `session` is null or the topic is no on-demand output. GUI thread.
[[nodiscard]] DerivedTopicPresentation derivedTopicPresentation(SessionManager* session, ObjectTopicId topic_id);

/// Backend-neutral contract for one object topic in a layered scene.
///
/// Implementations own the per-topic adapter state and expose changes through
/// Qt signals. A layer is attached before use, updated by the dock as tracker
/// time and visibility change, and detached before removal or destruction.
class ISceneLayer : public QObject {
  Q_OBJECT
 public:
  explicit ISceneLayer(QObject* parent = nullptr);
  ~ISceneLayer() override;

  /// Returns the current layer metadata used by dock and list UI.
  [[nodiscard]] virtual SceneLayerInfo info() const = 0;

  /// Returns the valid data range as an absolute Timepoint interval, or an
  /// inverted/empty range `{Timepoint::max(), Timepoint::min()}` if the layer
  /// carries no data (e.g. a static layer positioned purely by TF). The dock
  /// detects emptiness with `range.max < range.min`.
  ///
  /// MUST reflect the layer's data as it stands NOW — for a store-backed layer,
  /// the live ObjectStore range, never a value cached at the last render. The
  /// dock clamps the tracker to this range, so a stale range pins scrubbing to
  /// an evicted time and freezes the scene during streaming. Single-topic layers
  /// should delegate to `liveTopicTimeRange()`.
  [[nodiscard]] virtual PJ::Range<PJ::Timepoint> timeRange() const = 0;

  /// Binds the layer to shared scene services. Called once before updates.
  virtual bool attach(const SceneLayerContext& ctx) = 0;

  /// Releases resources acquired by attach(). Called before the layer is dropped.
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

  /// Cheap fingerprint of what this layer would render at `time` — its active
  /// data sample plus any transform that positions it. The dock combines the keys
  /// of all visible layers across consecutive tracker ticks and SKIPS the repaint
  /// when the combined key is unchanged, so a 60 Hz playhead over <10 Hz data (or
  /// a paused-but-still-ticking clock) does not drive a 60 Hz repaint.
  ///
  /// MUST be cheap (no decode) and MUST change whenever the rendered output would
  /// differ at `time`. The default returns the tracker time itself — i.e. "always
  /// changed", which is safe (it never skips a real change, only forgoes the
  /// optimization); layers override it to opt into repaint coalescing. This gate
  /// covers only the per-tick tracker path; async/settings repaints flow through
  /// repaintRequested → refreshView independently of it.
  [[nodiscard]] virtual uint64_t renderKey(PJ::Timepoint time) const {
    return static_cast<uint64_t>(PJ::toRaw(time));
  }

  /// Updates visibility; implementations should emit visibilityChanged on change.
  virtual void setVisible(bool visible) = 0;

  /// Updates the fixed frame used by layers that depend on transforms.
  virtual void setFixedFrame(const QString& frame);

  /// Creates optional layer settings UI owned through the supplied Qt parent.
  virtual QWidget* createConfigWidget(QWidget* parent) = 0;

  /// Saves optional layer-specific configuration inside the dock's layer node.
  virtual QDomElement xmlSaveState(QDomDocument& doc) const;

  /// Restores optional layer-specific configuration from xmlSaveState().
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
