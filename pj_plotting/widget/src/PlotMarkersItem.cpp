// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/PlotMarkersItem.h"

#include <qwt_scale_map.h>

#include <QColor>
#include <QFontMetrics>
#include <QPainter>
#include <QPen>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <utility>

#include "pj_base/builtin/plot_markers_codec.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/MarkerTopics.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"

namespace PJ {

namespace {

QColor severityColor(sdk::MarkerSeverity severity, const sdk::ColorRGBA& override_color) {
  if (override_color.a != 0) {
    return {override_color.r, override_color.g, override_color.b, override_color.a};
  }
  switch (severity) {
    case sdk::MarkerSeverity::kInfo:
      return {80, 140, 255};
    case sdk::MarkerSeverity::kWarning:
      return {240, 180, 40};
    case sdk::MarkerSeverity::kError:
      return {230, 70, 60};
    case sdk::MarkerSeverity::kCritical:
      return {170, 30, 160};
  }
  return {80, 140, 255};
}

// --- Label "pill" badges -----------------------------------------------------
// A label renders as a filled rounded-rect in the marker color with white text,
// matching the chart-annotation look (cf. pj_widgets/CheckButton).

constexpr double kPillPadH = 6.0;
constexpr double kPillPadV = 3.0;
constexpr double kPillRadius = 4.0;

QSizeF pillSize(const QFontMetrics& fm, const QString& text) {
  return {fm.horizontalAdvance(text) + 2.0 * kPillPadH, fm.height() + 2.0 * kPillPadV};
}

// Draw a horizontal pill with its top-left at `topLeft`. `fm` is the painter's font
// metrics (passed in so it's built once per paint, not per pill).
void drawPill(
    QPainter* painter, const QPointF& topLeft, const QString& text, const QColor& color, const QFontMetrics& fm) {
  const QRectF box(topLeft, pillSize(fm, text));
  painter->save();
  painter->setRenderHint(QPainter::Antialiasing, true);
  painter->setPen(Qt::NoPen);
  painter->setBrush(color);
  painter->drawRoundedRect(box, kPillRadius, kPillRadius);
  painter->setPen(Qt::white);
  painter->drawText(box, Qt::AlignCenter, text);
  painter->restore();
}

// Nudge a pill's top-left so the whole box stays inside `bounds`.
QPointF clampPill(QPointF topLeft, const QSizeF& size, const QRectF& bounds) {
  const double maxX = std::max(bounds.left(), bounds.right() - size.width());
  const double maxY = std::max(bounds.top(), bounds.bottom() - size.height());
  topLeft.setX(std::clamp(topLeft.x(), bounds.left(), maxX));
  topLeft.setY(std::clamp(topLeft.y(), bounds.top(), maxY));
  return topLeft;
}

// A pill rotated 90° (text reads bottom-to-top) running down a vertical line at
// screen-x `x`, anchored near the top of `bounds` and centered on the line.
void drawVerticalPill(
    QPainter* painter, double x, const QRectF& bounds, const QString& text, const QColor& color,
    const QFontMetrics& fm) {
  const QSizeF sz = pillSize(fm, text);
  // The rotated pill spans sz.width() vertically (down from the top) and
  // sz.height() horizontally across the line — keep both on-screen.
  const double cx = std::clamp(x, bounds.left() + sz.height() / 2.0, bounds.right() - sz.height() / 2.0);
  painter->save();
  painter->setRenderHint(QPainter::Antialiasing, true);
  painter->translate(cx, bounds.top() + 4.0 + sz.width());
  painter->rotate(-90.0);  // local +x -> screen up; text reads bottom-to-top
  const QRectF box(QPointF(0.0, -sz.height() / 2.0), sz);
  painter->setPen(Qt::NoPen);
  painter->setBrush(color);
  painter->drawRoundedRect(box, kPillRadius, kPillRadius);
  painter->setPen(Qt::white);
  painter->drawText(box, Qt::AlignCenter, text);
  painter->restore();
}

// A full-height vertical line at screen-x `x` plus an optional rotated label pill —
// shared by time-event markers (kEvent, no value) and label markers (kLabel). Culled
// when off-screen.
void drawTimeLineMarker(
    QPainter* painter, double x, const QRectF& bounds, const std::string& label, const QColor& color,
    const QFontMetrics& fm) {
  if (x < bounds.left() || x > bounds.right()) {
    return;  // line outside the current view
  }
  painter->setPen(QPen(color, 1.5));
  painter->drawLine(QPointF(x, bounds.top()), QPointF(x, bounds.bottom()));
  if (!label.empty()) {
    drawVerticalPill(painter, x, bounds, QString::fromStdString(label), color, fm);
  }
}

}  // namespace

std::vector<MarkerTargetTopic> markerTargetTopics(const ObjectStore& store, const MarkerTarget& target) {
  const std::string key = target.topic.toStdString();
  std::vector<MarkerTargetTopic> topics;
  if (key != kAllDatasetsMarkerTopic) {
    for (const ObjectTopicId id : markerTopicsOf(store, target.dataset, key)) {
      topics.push_back(MarkerTargetTopic{.id = id, .display_frame = false});
    }
  }
  for (const ObjectTopicId id : markerTopicsOf(store, kAllDatasetsMarkerDataset, key)) {
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

void PlotMarkersItem::draw(
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

  for (const MarkerTarget& target : targets) {
    const DisplayOffset dataset_offset = session_->displayOffset(target.dataset);
    for (const MarkerTargetTopic& topic : markerTargetTopics(store, target)) {
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
    const QColor color = severityColor(m.severity, m.color);

    switch (m.kind) {
      case sdk::MarkerKind::kRegion: {
        const double x0 = xMap.transform(rawToDisplaySeconds(m.t_start + stamp_shift, offset).value);
        const double x1 = xMap.transform(rawToDisplaySeconds(m.t_end + stamp_shift, offset).value);
        if (std::max(x0, x1) < canvasRect.left() || std::min(x0, x1) > canvasRect.right()) {
          break;  // region fully outside the current view
        }
        QColor fill = color;
        fill.setAlpha(60);
        painter->fillRect(
            QRectF(QPointF(std::min(x0, x1), canvasRect.top()), QPointF(std::max(x0, x1), canvasRect.bottom())), fill);
        QColor edge = color;
        edge.setAlpha(190);
        painter->setPen(QPen(edge, 1.0));
        painter->drawLine(QPointF(x0, canvasRect.top()), QPointF(x0, canvasRect.bottom()));
        painter->drawLine(QPointF(x1, canvasRect.top()), QPointF(x1, canvasRect.bottom()));
        if (!m.label.empty()) {
          const QString text = QString::fromStdString(m.label);
          const QSizeF sz = pillSize(fm, text);
          const QPointF tl = clampPill(QPointF(std::min(x0, x1) + 4.0, canvasRect.top() + 4.0), sz, canvasRect);
          drawPill(painter, tl, text, color, fm);
        }
        break;
      }
      case sdk::MarkerKind::kEvent: {
        const double x = xMap.transform(rawToDisplaySeconds(m.t_start + stamp_shift, offset).value);
        if (m.has_value) {
          // Point event: a hollow ring at (t, value) — coloured outline, transparent
          // centre (the curve/grid shows through).
          const double y = yMap.transform(m.value_low);
          if (x < canvasRect.left() || x > canvasRect.right() || y < canvasRect.top() || y > canvasRect.bottom()) {
            break;  // point outside the current view
          }
          painter->setPen(QPen(color, 2.0));
          painter->setBrush(Qt::NoBrush);
          painter->drawEllipse(QPointF(x, y), 4.5, 4.5);
          if (!m.label.empty()) {
            const QString text = QString::fromStdString(m.label);
            const QSizeF sz = pillSize(fm, text);
            // Centered just above the dot; flip below if it would clip the top.
            double top = y - 6.0 - sz.height();
            if (top < canvasRect.top()) {
              top = y + 6.0;
            }
            const QPointF tl = clampPill(QPointF(x - sz.width() / 2.0, top), sz, canvasRect);
            drawPill(painter, tl, text, color, fm);
          }
        } else {
          // Time event: a full-height vertical line (no value attached).
          drawTimeLineMarker(painter, x, canvasRect, m.label, color, fm);
        }
        break;
      }
      case sdk::MarkerKind::kLabel: {
        // A label marker renders identically to a time event: a vertical line + pill.
        const double x = xMap.transform(rawToDisplaySeconds(m.t_start + stamp_shift, offset).value);
        drawTimeLineMarker(painter, x, canvasRect, m.label, color, fm);
        break;
      }
      case sdk::MarkerKind::kValueBand: {
        const double y0 = yMap.transform(m.value_low);
        const double y1 = yMap.transform(m.value_high);
        if (m.value_low == m.value_high) {
          // Zero-height band: draw a horizontal line at the value (a "value event"),
          // the value-axis dual of a time event's vertical line.
          if (y0 < canvasRect.top() || y0 > canvasRect.bottom()) {
            break;  // horizontal line outside the current view
          }
          painter->setPen(QPen(color, 1.5));
          painter->drawLine(QPointF(canvasRect.left(), y0), QPointF(canvasRect.right(), y0));
          if (!m.label.empty()) {
            const QString text = QString::fromStdString(m.label);
            const QSizeF sz = pillSize(fm, text);
            const QPointF tl = clampPill(QPointF(canvasRect.left() + 4.0, y0 - sz.height() / 2.0), sz, canvasRect);
            drawPill(painter, tl, text, color, fm);
          }
        } else {
          QColor fill = color;
          fill.setAlpha(50);
          painter->fillRect(
              QRectF(QPointF(canvasRect.left(), std::min(y0, y1)), QPointF(canvasRect.right(), std::max(y0, y1))),
              fill);
        }
        break;
      }
    }
  }
}

}  // namespace PJ
