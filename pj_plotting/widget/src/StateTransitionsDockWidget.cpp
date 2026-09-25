// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/StateTransitionsDockWidget.h"

#include <QVBoxLayout>

#include "pj_runtime/CatalogModel.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {
constexpr auto kPayloadTag = "state_transitions"_L1;
}  // namespace

StateTransitionsDockWidget::StateTransitionsDockWidget(
    SessionManager* session, CatalogModel* catalog, PlaybackEngine* playback, QWidget* parent)
    : QWidget(parent), catalog_(catalog) {
  view_ = new StateTransitionsView(this);
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  layout->addWidget(view_);

  controller_ = std::make_unique<StateTransitionsController>(view_, session, catalog, playback, this);
  connect(
      controller_.get(), &StateTransitionsController::seriesListChanged, this,
      &StateTransitionsDockWidget::workspaceChanged);
  connect(view_, &StateTransitionsView::viewStateChangeCommitted, this, &StateTransitionsDockWidget::workspaceChanged);
  if (catalog_ != nullptr) {
    // A series saved against a dataset that had not loaded yet binds the moment
    // its items surface, so a layout restored mid-load self-heals like plot
    // curves do (PendingDisplayBinder's flush, in strip form).
    connect(catalog_, &CatalogModel::itemsAdded, this, &StateTransitionsDockWidget::retryPendingSeries);
    // A surviving key whose item changed (e.g. a float field turning discrete on
    // its first whole-number data) surfaces no new key, so retry on that too.
    connect(catalog_, &CatalogModel::itemsChanged, this, &StateTransitionsDockWidget::retryPendingSeries);
  }
}

void StateTransitionsDockWidget::onTrackerTime(double time) {
  view_->setPlayhead(time);
}

bool StateTransitionsDockWidget::tryAcceptSeriesKeys(const QStringList& catalog_keys) {
  bool any_added = false;
  for (const QString& key : catalog_keys) {
    any_added = controller_->addSeries(key) || any_added;
  }
  return any_added;
}

QDomElement StateTransitionsDockWidget::xmlSaveState(QDomDocument& doc) const {
  QDomElement root = doc.createElement(kPayloadTag);
  root.setAttribute(u"zoom_px_per_ns"_s, QString::number(view_->zoom(), 'g', 17));
  root.setAttribute(u"viewport_left_ns"_s, QString::number(view_->viewportLeftDisplayNs()));
  root.setAttribute(u"viewport_top_px"_s, view_->viewportTopOffsetPx());

  const auto append_series = [&doc, &root](
                                 qint64 dataset_id, const QString& dataset_source, const QString& dataset_path,
                                 const QString& topic, const QString& field, bool visible) {
    QDomElement series = doc.createElement(u"series"_s);
    series.setAttribute(u"dataset_id"_s, QString::number(dataset_id));
    series.setAttribute(u"dataset_source"_s, dataset_source);
    series.setAttribute(u"dataset_path"_s, dataset_path);
    series.setAttribute(u"topic"_s, topic);
    series.setAttribute(u"field"_s, field);
    series.setAttribute(u"visible"_s, visible ? u"1"_s : u"0"_s);
    root.appendChild(series);
  };

  // currentSeries() and seriesEntries() walk the same row list, so index i of
  // one is index i of the other (descriptor identity + eye state per row).
  const std::vector<CurveDescriptor> descriptors = controller_->currentSeries();
  const std::vector<StateTransitionsController::SeriesEntry> entries = controller_->seriesEntries();
  for (std::size_t index = 0; index < descriptors.size(); ++index) {
    const CurveDescriptor& descriptor = descriptors[index];
    const QString source = catalog_ != nullptr
                               ? catalog_->datasetSourceName(descriptor.dataset_id).value_or(descriptor.dataset_name)
                               : descriptor.dataset_name;
    const QString path = catalog_ != nullptr ? catalog_->datasetSourcePath(descriptor.dataset_id) : QString();
    append_series(
        static_cast<qint64>(descriptor.dataset_id), source, path, descriptor.topic_name,
        descriptor.field_path.isEmpty() ? descriptor.field_name : descriptor.field_path,
        index < entries.size() ? entries[index].visible : true);
  }
  // Unresolved series ride along verbatim: a save without their dataset loaded
  // must not silently forget them.
  for (const PendingSeries& pending : pending_series_) {
    append_series(
        pending.dataset_id, pending.dataset_source, pending.dataset_path, pending.topic, pending.field,
        pending.visible);
  }
  return root;
}

bool StateTransitionsDockWidget::xmlLoadState(const QDomElement& element) {
  if (element.tagName() != kPayloadTag) {
    return false;
  }
  pending_series_.clear();
  for (QDomElement series = element.firstChildElement(u"series"_s); !series.isNull();
       series = series.nextSiblingElement(u"series"_s)) {
    const PendingSeries saved{
        .dataset_source = series.attribute(u"dataset_source"_s),
        .dataset_path = series.attribute(u"dataset_path"_s),
        .dataset_id = series.attribute(u"dataset_id"_s).toLongLong(),
        .topic = series.attribute(u"topic"_s),
        .field = series.attribute(u"field"_s),
        .visible = series.attribute(u"visible"_s, u"1"_s) != u"0"_s,
    };
    if (!tryResolveSeries(saved)) {
      pending_series_.push_back(saved);
    }
  }

  applyViewChrome(element);
  return true;
}

bool StateTransitionsDockWidget::tryResolveSeries(const PendingSeries& saved) {
  const std::optional<QString> key =
      catalog_ != nullptr ? catalog_->resolveCurveKey(
                                static_cast<DatasetId>(saved.dataset_id), saved.dataset_source, saved.dataset_path,
                                saved.topic, saved.field, SeriesCapability::kDiscrete)
                          : std::nullopt;
  if (!key.has_value() || !controller_->addSeries(*key)) {
    return false;
  }
  if (!saved.visible) {
    // addSeries appends, so the restored row is the last entry.
    const auto entries = controller_->seriesEntries();
    if (!entries.empty()) {
      controller_->setSeriesVisible(entries.back().row_id, false);
    }
  }
  return true;
}

void StateTransitionsDockWidget::retryPendingSeries() {
  std::erase_if(pending_series_, [this](const PendingSeries& saved) { return tryResolveSeries(saved); });
}

void StateTransitionsDockWidget::applyViewChrome(const QDomElement& element) {
  // View chrome after the rows: zoom first so the pan maps under it.
  bool zoom_ok = false;
  const double zoom = element.attribute(u"zoom_px_per_ns"_s).toDouble(&zoom_ok);
  if (zoom_ok && zoom > 0.0) {
    view_->setZoom(zoom);
    view_->setViewportLeftDisplayNs(element.attribute(u"viewport_left_ns"_s).toLongLong());
  }
  view_->setViewportTopOffsetPx(element.attribute(u"viewport_top_px"_s).toInt());
}

}  // namespace PJ
