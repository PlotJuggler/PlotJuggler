#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDomDocument>
#include <QDomElement>
#include <QString>
#include <QStringList>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_datastore/object_store.hpp"

class QWidget;

namespace PJ {

// A single dropped object topic, handed to the object-widget factory so it can
// construct the right dock *and* seed it with its first topic. The factory is
// called with a seed for a catalog drop, or with `nullptr` (kind only) during
// layout restore, where the topics are repopulated from XML via xmlLoadState().
struct ObjectDropSeed {
  ObjectTopicId topic_id;
  sdk::BuiltinObjectType object_type;
  QString title;
};

// Contract implemented by every widget family (plot / 2D / 3D) so pj_runtime
// can drive tracker updates without coupling to concrete widget types.
class IDataWidget {
 public:
  virtual ~IDataWidget() = default;

  virtual QWidget* widget() = 0;

  /// Called with the playback cursor position as **display-axis seconds**:
  /// `time = toAxisDouble(currentTime())`, which equals `raw_ns * 1e-9` minus
  /// the session's DisplayOffset (see Time.h).  Today no production code sets a
  /// nonzero DisplayOffset, so display-axis seconds and absolute seconds are
  /// identical in practice — but implementors MUST NOT treat this value as an
  /// absolute store timestamp. Converting to an absolute Timepoint requires
  /// re-adding the DisplayOffset: `toAbsolute(time, offsetOf(domain))`.
  /// SceneDockWidget::onTrackerTime and Scene3DDockWidget::driveVisibleLayersToLiveEdge
  /// are the two scene-family entry points; they must remain consistent (both
  /// ultimately call pushTrackerTimeToView with absolute ns).
  virtual void onTrackerTime(double time) = 0;

  // Optional hook: a host (DockWidget) may offer a freshly-dropped object
  // topic to an existing widget that wants to consume it instead of being
  // replaced. Default = refuse, which preserves the historical replace-on-drop
  // behavior for plot and 2D widgets. Multi-topic 3D widgets override this to
  // accept compatible types in place. Return true if the topic was accepted.
  virtual bool tryAcceptObjectTopic(
      ObjectTopicId /*topic_id*/, sdk::BuiltinObjectType /*object_type*/, const QString& /*title*/) {
    return false;
  }

  /// Non-mutating hover preflight. Widgets with payload-dependent restrictions override
  /// this to reject incompatible catalog drops before the user releases the mouse.
  /// The default leaves acceptance to tryAcceptObjectTopic().
  [[nodiscard]] virtual bool canAcceptObjectTopic(ObjectTopicId, sdk::BuiltinObjectType) const {
    return true;
  }

  // Optional hook, sibling of tryAcceptObjectTopic for SCALAR catalog keys: a
  // host (DockWidget) offers the keys of a curve-list drop to a mounted widget
  // that consumes scalar series itself (the state-transitions strip takes
  // discrete series this way). The widget filters to the keys it can host and
  // returns whether it accepted any. Default = refuse, preserving the
  // historical scalar-drop behavior (plots are built by the host, not offered).
  virtual bool tryAcceptSeriesKeys(const QStringList& /*catalog_keys*/) {
    return false;
  }

  // Optional XML persistence. Default returns a null QDomElement, signalling
  // "this widget doesn't persist state yet" — the dispatcher (PlotDocker)
  // checks `isNull()` and skips honestly rather than writing an empty stub.
  // Widgets that own state override these to produce a single self-contained
  // element (e.g. Scene3DDockWidget returns `<scene3d>`).
  virtual QDomElement xmlSaveState(QDomDocument& /*doc*/) const {
    return {};
  }
  virtual bool xmlLoadState(const QDomElement& /*element*/) {
    return true;
  }

  /// Host call once the data a just-loaded `xmlLoadState` refers to is final: the shell
  /// after its whole load, or a dock after a paste/family switch into a session whose data
  /// is already loaded. A widget that defers saved elements until their data arrives
  /// settles whatever is still undecided and makes its last attempt here. Default no-op.
  virtual void finishRestore() {}

  /// Called once, right before the host schedules this widget for deferred deletion (never when it
  /// moves to another dock): release here, synchronously, what must not wait for the event loop.
  virtual void aboutToBeDiscarded() {}
};

}  // namespace PJ
