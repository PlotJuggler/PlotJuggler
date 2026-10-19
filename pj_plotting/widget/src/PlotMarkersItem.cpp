// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/PlotMarkersItem.h"

#include <qwt_plot.h>
#include <qwt_plot_legenditem.h>
#include <qwt_scale_map.h>

#include <QFontMetrics>
#include <QPainter>
#include <QPalette>
#include <QRectF>
#include <QWidget>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "pj_base/builtin/plot_markers_codec.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_plotting/MarkerPainter.h"
#include "pj_runtime/MarkerTopics.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"

namespace PJ {

std::vector<MarkerTargetTopic> markerTargetTopics(
    const MarkerTarget& target, const std::vector<MarkerTopic>& dataset_topics,
    const std::vector<MarkerTopic>& shared_topics) {
  std::vector<MarkerTargetTopic> topics;
  if (target.marker.scope != MarkerScopeKind::kAll) {
    for (const ObjectTopicId id : markerTopicsOf(dataset_topics, target.marker)) {
      topics.push_back(MarkerTargetTopic{.id = id, .display_frame = false});
    }
  }
  for (const ObjectTopicId id : markerTopicsOf(shared_topics, target.marker)) {
    topics.push_back(MarkerTargetTopic{.id = id, .display_frame = true});
  }
  return topics;
}

PlotMarkersItem::PlotMarkersItem(SessionManager* session) : session_(session) {
  setZ(40.0);  // above curves (curves sit around z 20)
  setRenderHint(QwtPlotItem::RenderAntialiased, true);
}

void PlotMarkersItem::setTargetsProvider(std::function<std::vector<MarkerTarget>()> provider) {
  targets_provider_ = std::move(provider);
}

namespace {
constexpr qreal kNoteMargin = 8.0;
constexpr qreal kNotePadding = 6.0;
}  // namespace

bool PlotMarkersItem::setStatusNote(const QString& text) {
  if (status_note_ == text) {
    return false;
  }
  status_note_ = text;
  return true;
}

QRectF PlotMarkersItem::statusNoteRect(const QRectF& canvas_rect) const {
  if (status_note_.isEmpty() || plot() == nullptr) {
    return {};
  }
  const QFontMetricsF fm(plot()->canvas()->font());
  const QSizeF size(fm.horizontalAdvance(status_note_) + 2 * kNotePadding, fm.height() + kNotePadding);
  // The top-left corner, unless the legend is aligned there.
  const QwtPlotItemList legends = plot()->itemList(QwtPlotItem::Rtti_PlotLegend);
  const bool legend_top_left = std::ranges::any_of(legends, [](const QwtPlotItem* item) {
    const auto* legend = static_cast<const QwtPlotLegendItem*>(item);
    return legend->isVisible() && (legend->alignmentInCanvas() & Qt::AlignLeft) &&
           (legend->alignmentInCanvas() & Qt::AlignTop);
  });
  const qreal left =
      legend_top_left ? canvas_rect.right() - kNoteMargin - size.width() : canvas_rect.left() + kNoteMargin;
  return QRectF(QPointF(left, canvas_rect.top() + kNoteMargin), size);
}

void PlotMarkersItem::drawStatusNote(QPainter* painter, const QRectF& canvas_rect) const {
  const QRectF rect = statusNoteRect(canvas_rect);
  if (rect.isEmpty()) {
    return;
  }
  const QWidget* canvas = plot()->canvas();
  painter->save();
  painter->setFont(canvas->font());
  QColor background = canvas->palette().color(QPalette::Window);
  background.setAlpha(220);
  painter->setPen(canvas->palette().color(QPalette::Mid));
  painter->setBrush(background);
  painter->drawRoundedRect(rect, 3.0, 3.0);
  painter->setPen(canvas->palette().color(QPalette::WindowText));
  painter->drawText(rect, Qt::AlignCenter, status_note_);
  painter->restore();
}

void PlotMarkersItem::draw(
    QPainter* painter, const QwtScaleMap& xMap, const QwtScaleMap& yMap, const QRectF& canvasRect) const {
  drawMarkerSets(painter, xMap, yMap, canvasRect);
  drawStatusNote(painter, canvasRect);
}

void PlotMarkersItem::drawMarkerSets(
    QPainter* painter, const QwtScaleMap& xMap, const QwtScaleMap& yMap, const QRectF& canvasRect) const {
  if (session_ == nullptr || !targets_provider_) {
    return;
  }
  ObjectStore& store = session_->objectStore();
  const std::vector<MarkerTarget> targets = targets_provider_();
  const QFontMetrics fm(painter->font());  // constant for the whole paint; built once
  // A display-frame family carries no dataset alignment: only the global origin.
  const DisplayOffset display_frame_offset{Duration{session_->globalTimeReference()}};
  std::set<std::uint32_t> drawn;  // topic ids already painted this pass
  // Each dataset's marker topics, scanned from the store once per paint however many
  // targets (one per curve) name it.
  std::map<DatasetId, std::vector<MarkerTopic>> scans;
  const auto scanOf = [&](DatasetId dataset) -> const std::vector<MarkerTopic>& {
    const auto found = scans.find(dataset);
    return found != scans.end() ? found->second
                                : scans.emplace(dataset, scanMarkerTopics(store, dataset)).first->second;
  };

  for (const MarkerTarget& target : targets) {
    const DisplayOffset dataset_offset = session_->displayOffset(target.dataset);
    for (const MarkerTargetTopic& topic :
         markerTargetTopics(target, scanOf(target.dataset), scanOf(kAllDatasetsMarkerDataset))) {
      if (!drawn.insert(topic.id.id).second) {
        continue;
      }
      drawTopic(
          painter, xMap, yMap, canvasRect, fm, store, topic.id,
          topic.display_frame ? display_frame_offset : dataset_offset);
    }
  }
}

void PlotMarkersItem::drawTopic(
    QPainter* painter, const QwtScaleMap& xMap, const QwtScaleMap& yMap, const QRectF& canvasRect,
    const QFontMetrics& fm, const ObjectStore& store, ObjectTopicId topic_id, DisplayOffset offset) const {
  // Read the latest set regardless of playback position (sentinel timestamp).
  const std::optional<ResolvedObjectEntry> entry = store.latestAt(topic_id, std::numeric_limits<Timestamp>::max());
  if (!entry) {
    return;
  }
  // Re-decode only when the producer republished (uid changed); otherwise reuse
  // the cached set. Avoids a full deserialize (+ string/vector allocs) per paint.
  CachedMarkers& slot = decode_cache_[topic_id.id];
  if (slot.uid != entry->sequential_uid.value) {
    const Expected<sdk::PlotMarkers> decoded =
        deserializePlotMarkers(entry->payload.bytes.data(), entry->payload.bytes.size());
    if (!decoded.has_value()) {
      return;
    }
    slot.markers = decoded.value();
    slot.uid = entry->sequential_uid.value;
  }
  // Marker times are payload-embedded, so a time-shifted dataset merge moves the
  // entry but not the bytes; the entry records the delta instead. Apply it here
  // (per paint, NOT baked into the decode cache — the shift can change while the
  // payload uid does not) so markers land on the merged clock like their curves.
  const Timestamp stamp_shift = entry->payload_stamp_shift;
  for (const sdk::PlotMarker& m : slot.markers.markers) {
    paintMarker(
        painter, xMap, yMap, canvasRect, fm,
        MarkerPaintData{
            .kind = m.kind,
            .x0 = rawToDisplaySeconds(m.t_start + stamp_shift, offset).value,
            .x1 = rawToDisplaySeconds(m.t_end + stamp_shift, offset).value,
            .y0 = m.value_low,
            .y1 = m.value_high,
            .has_value = m.has_value,
            .color = markerSeverityColor(m.severity, m.color),
            .label = m.label,
        });
  }
}

}  // namespace PJ
