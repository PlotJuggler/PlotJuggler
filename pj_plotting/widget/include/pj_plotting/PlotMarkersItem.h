#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <qwt_plot_item.h>

#include <QString>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include "pj_base/builtin/plot_markers.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/object_store.hpp"  // ObjectStore, ObjectTopicId
#include "pj_runtime/Time.h"              // DisplayOffset

class QFontMetrics;
class QPainter;
class QwtScaleMap;

namespace PJ {

class SessionManager;

/// A (dataset, topic) the marker overlay should render markers for. The topic is
/// either a series identity (field path) or the reserved dataset-global name.
struct MarkerTarget {
  DatasetId dataset = 0;
  QString topic;
};

/// One object topic the overlay draws for a target, and the frame its times are
/// stored in: the dataset's raw clock, or the display frame of the shared
/// all-datasets home (`kAllDatasetsMarkerDataset`).
struct MarkerTargetTopic {
  ObjectTopicId id;
  bool display_frame = false;
};

/// Every family member `target` resolves to: the all-datasets scope reads the
/// shared home only; any other key reads its dataset's family and then the shared
/// home's (a scope=all rule may publish under a per-series key). The overlay's
/// resolver, exposed as the test seam.
[[nodiscard]] std::vector<MarkerTargetTopic> markerTargetTopics(const ObjectStore& store, const MarkerTarget& target);

// The reserved marker-scope topic names live in pj_runtime/MarkerTopics.h; this
// module has no shadow copy of them.

/// Custom QwtPlotItem that overlays plot markers (regions / events / value bands /
/// labels), drawn on top of the curves. Each (dataset, topic) target is a FAMILY of
/// serialized PlotMarkers object topics in the session ObjectStore — the bare topic
/// plus one per owning generator (`markerTopicsOf`) — drawn as their union; each
/// topic is drawn once per paint even when several targets resolve to it (the
/// all-datasets scope of every dataset on the plot is the same shared topic).
/// Time anchors (raw int64 ns) are converted to display seconds via the dataset's
/// display offset; a family on the shared all-datasets home is already stored in
/// the display frame, so only the uniform global origin applies to it.
class PlotMarkersItem : public QwtPlotItem {
 public:
  explicit PlotMarkersItem(SessionManager* session);

  /// Provider of the (dataset, topic) targets to render, set by the owning
  /// PlotWidget and recomputed from the plot's current curves on each draw.
  void setTargetsProvider(std::function<std::vector<MarkerTarget>()> provider);

  void setSession(SessionManager* session) {
    session_ = session;
  }

  [[nodiscard]] int rtti() const override {
    return QwtPlotItem::Rtti_PlotUserItem;
  }

  void draw(
      QPainter* painter, const QwtScaleMap& xMap, const QwtScaleMap& yMap, const QRectF& canvasRect) const override;

 private:
  /// Paint one object topic's set (decoded through `decode_cache_`), raw times
  /// mapped through `offset`.
  void drawTopic(
      QPainter* painter, const QwtScaleMap& xMap, const QwtScaleMap& yMap, const QRectF& canvasRect,
      const QFontMetrics& fm, const ObjectStore& store, ObjectTopicId topic_id, DisplayOffset offset) const;

  SessionManager* session_ = nullptr;
  std::function<std::vector<MarkerTarget>()> targets_provider_;

  // Decode cache: the deserialized marker set per object-topic. draw() runs on
  // every replot (up to 60 Hz while streaming) but markers change only when a
  // producer republishes — which bumps the entry's sequential_uid. Keying on that
  // uid means a paint re-decodes only on an actual change; otherwise it is a map
  // lookup. draw() is const, so the cache is mutable.
  struct CachedMarkers {
    std::uint64_t uid = 0;  // SequentialUID::value; 0 = empty slot (uids start at 1)
    sdk::PlotMarkers markers;
  };
  mutable std::map<std::uint64_t, CachedMarkers> decode_cache_;
};

}  // namespace PJ
