// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "PluginPlotTabsController.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <optional>

#include "pj_plotting/DockWidget.h"
#include "pj_plotting/PlotDocker.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_plotting/TabbedPlotWidget.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/ExtensionCatalogService.h"

using namespace Qt::StringLiterals;

namespace PJ {

namespace {

// The host-namespaced form of a plugin's own tab name. It becomes the docker's
// stateId, which is what stops two plugins choosing the same name from ever
// addressing each other's tab.
QString ownedTabKey(const QString& plugin_id, const QString& tab_id) {
  return plugin_id + u"/"_s + tab_id;
}

/// The ABI error for a tab id the calling plugin does not own (or that does not exist).
Unexpected<std::string> noOwnedTabError(const QString& tab_id) {
  return unexpected("no tab '" + tab_id.toStdString() + "' belonging to this plugin");
}

}  // namespace

PluginPlotTabsController::PluginPlotTabsController(
    TabbedPlotWidget& tabs, CatalogModel& catalog, ExtensionCatalogService& extensions)
    : tabs_(tabs), catalog_(catalog), extensions_(extensions) {}

PlotTabsRuntimeHost::Callbacks PluginPlotTabsController::plotTabsCallbacks(const QString& plugin_id) {
  return PlotTabsRuntimeHost::Callbacks{
      .create_tab =
          [this, plugin_id](std::string_view id, std::string_view title) {
            return createTab(plugin_id, QString::fromUtf8(id), QString::fromUtf8(title));
          },
      .close_tab = [this, plugin_id](std::string_view id) { return closeTab(plugin_id, QString::fromUtf8(id)); },
      .list_tab_ids = [this, plugin_id]() { return listTabIds(plugin_id); },
      .tab_config = [this, plugin_id](std::string_view id) { return tabConfig(plugin_id, QString::fromUtf8(id)); },
      .add_curve =
          [this, plugin_id](
              std::string_view id, std::string_view topic, std::string_view field, std::string_view dataset_source) {
            return addCurve(
                plugin_id, QString::fromUtf8(id), QString::fromUtf8(topic), QString::fromUtf8(field),
                QString::fromUtf8(dataset_source));
          },
      .remove_curve =
          [this, plugin_id](
              std::string_view id, std::string_view topic, std::string_view field, std::string_view dataset_source) {
            return removeCurve(
                plugin_id, QString::fromUtf8(id), QString::fromUtf8(topic), QString::fromUtf8(field),
                QString::fromUtf8(dataset_source));
          },
      .clear_tab = [this, plugin_id](std::string_view id) { return clearTab(plugin_id, QString::fromUtf8(id)); },
  };
}

ViewportRuntimeHost::Callbacks PluginPlotTabsController::viewportCallbacks(const QString& plugin_id) {
  return ViewportRuntimeHost::Callbacks{
      .zoom_to_time_range = [this, plugin_id](
                                double t0_s, double t1_s) { return zoomToTimeRange(plugin_id, t0_s, t1_s); },
      .zoom_reset = [this, plugin_id]() { return zoomOut(plugin_id); },
  };
}

void PluginPlotTabsController::forEachPlotOwnedBy(
    const QString& plugin_id, const std::function<void(PlotWidget*)>& operation) const {
  // This is where the boundary is actually enforced: a plugin drives the view
  // only where the user can see it is driving.
  tabs_.forEachDocker([&plugin_id, &operation](PlotDocker* docker) {
    if (docker->ownerPlugin() != plugin_id) {
      return;
    }
    const int plot_count = docker->plotCount();
    for (int index = 0; index < plot_count; ++index) {
      if (DockWidget* dock = docker->plotAt(index)) {
        if (PlotWidget* plot = dock->plotWidget()) {
          operation(plot);
        }
      }
    }
  });
}

Status PluginPlotTabsController::zoomToTimeRange(const QString& plugin_id, double t0_s, double t1_s) {
  // An explicit command sets every plot itself, so there is no link feedback to propagate.
  int owned = 0;
  int zoomed = 0;
  forEachPlotOwnedBy(plugin_id, [t0_s, t1_s, &owned, &zoomed](PlotWidget* plot) {
    ++owned;
    if (plot->isEmpty() || plot->isXYPlot()) {
      return;
    }
    plot->setVisibleXRange(t0_s, t1_s);
    ++zoomed;
  });
  if (zoomed != 0) {
    return okStatus();
  }
  // Two distinct dead ends with two distinct remedies — open a tab, or put
  // something in the one you have. Saying only "nothing to zoom" would leave a
  // caller repeating whichever of the two it guessed.
  return unexpected(
      owned == 0 ? "this plugin owns no plot tab; create one before zooming"
                 : "the tabs this plugin owns hold no time-series plot to zoom");
}

Status PluginPlotTabsController::zoomOut(const QString& plugin_id) {
  int owned = 0;
  forEachPlotOwnedBy(plugin_id, [&owned](PlotWidget* plot) {
    ++owned;
    plot->zoomOut(false);
  });
  if (owned == 0) {
    return unexpected("this plugin owns no plot tab; create one before zooming");
  }
  return okStatus();
}

PlotDocker* PluginPlotTabsController::ownedTab(const QString& plugin_id, const QString& tab_id) const {
  // Ownership is only ever established in createTab, which stamps both the
  // namespaced stateId and the owner metadata; requiring both to agree is what
  // keeps a user-made tab from ever answering to a plugin.
  const QString key = ownedTabKey(plugin_id, tab_id);
  return tabs_.findDocker(
      [&](PlotDocker* docker) { return docker->stateId() == key && docker->ownerPlugin() == plugin_id; });
}

Status PluginPlotTabsController::createTab(const QString& plugin_id, const QString& tab_id, const QString& title) {
  if (PlotDocker* existing = ownedTab(plugin_id, tab_id); existing != nullptr) {
    if (auto status = clearTab(plugin_id, tab_id); !status) {
      return status;
    }
    existing->setName(title.isEmpty() ? existing->name() : title);
    return okStatus();
  }
  PlotDocker* docker = tabs_.addTab(title);
  if (docker == nullptr) {
    return unexpected("the workspace refused a new tab");
  }
  docker->setStateId(ownedTabKey(plugin_id, tab_id));
  // Plugin composition has no history step: the tab is outside undo/redo even
  // though it is durable in a full layout file.
  docker->setHistoryExempt(true);
  docker->setOwnerMetadata(plugin_id, tab_id);
  docker->setOwnerBadge(toolboxBadge(plugin_id));
  return okStatus();
}

QString PluginPlotTabsController::toolboxBadge(const QString& plugin_id) const {
  const LoadedToolbox* toolbox = extensions_.findToolbox(plugin_id);
  if (toolbox == nullptr) {
    return {};
  }
  return toolbox->name.empty() ? plugin_id : QString::fromStdString(toolbox->name);
}

void PluginPlotTabsController::refreshAvailability() {
  tabs_.forEachDocker([this](PlotDocker* docker) {
    if (!docker->ownerPlugin().isEmpty()) {
      docker->setOwnerBadge(toolboxBadge(docker->ownerPlugin()));
    }
  });
}

Status PluginPlotTabsController::closeTab(const QString& plugin_id, const QString& tab_id) {
  PlotDocker* docker = ownedTab(plugin_id, tab_id);
  if (docker == nullptr) {
    return noOwnedTabError(tab_id);
  }
  tabs_.closeTab(docker);
  return okStatus();
}

Expected<std::vector<std::string>> PluginPlotTabsController::listTabIds(const QString& plugin_id) const {
  std::vector<std::string> ids;
  tabs_.forEachDocker([&](PlotDocker* docker) {
    if (docker->ownerPlugin() == plugin_id) {
      ids.push_back(docker->ownerTabId().toStdString());
    }
  });
  return ids;
}

Expected<std::string> PluginPlotTabsController::tabConfig(const QString& plugin_id, const QString& tab_id) const {
  PlotDocker* docker = ownedTab(plugin_id, tab_id);
  if (docker == nullptr) {
    return noOwnedTabError(tab_id);
  }
  QJsonArray curves;
  const int plot_count = docker->plotCount();
  for (int index = 0; index < plot_count; ++index) {
    DockWidget* dock = docker->plotAt(index);
    PlotWidget* plot = dock == nullptr ? nullptr : dock->plotWidget();
    if (plot == nullptr) {
      continue;
    }
    for (const auto& info : plot->curveList()) {
      // Reported from the catalog entry the curve is actually bound to, so the
      // dataset named here is the resolved one even when the caller left it
      // blank — the point of a read-back is to answer "which run is this?".
      const std::optional<CatalogItem> item = catalog_.itemDescriptor(info.source_name);
      if (!item.has_value()) {
        continue;
      }
      const ScalarFieldPayload* scalar = asScalarField(*item);
      if (scalar == nullptr) {
        continue;
      }
      QJsonObject curve;
      curve.insert(u"topic"_s, item->topic_name);
      curve.insert(u"field"_s, scalar->field_name);
      curve.insert(u"dataset"_s, catalog_.datasetSourceName(item->dataset_id).value_or(QString()));
      curves.append(curve);
    }
  }
  QJsonObject root;
  root.insert(u"title"_s, docker->name());
  root.insert(u"curves"_s, curves);
  return QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString();
}

Status PluginPlotTabsController::addCurve(
    const QString& plugin_id, const QString& tab_id, const QString& topic, const QString& field,
    const QString& dataset_source) {
  PlotDocker* docker = ownedTab(plugin_id, tab_id);
  if (docker == nullptr) {
    return noOwnedTabError(tab_id);
  }
  const std::optional<QString> key =
      catalog_.resolveCurveKey(0, dataset_source, {}, topic, field, SeriesCapability::kPlottable);
  if (!key.has_value()) {
    // resolveCurveKey folds "no such series" and "several datasets have it"
    // into one nullopt; the message names both remedies.
    return unexpected(("no plottable series '" + topic + u"/"_s + field +
                       "' is loaded, or it exists in several datasets (name one with dataset_source)")
                          .toStdString());
  }
  DockWidget* dock = docker->plotAt(0);
  PlotWidget* plot = dock == nullptr ? nullptr : dock->ensurePlotWidget();
  if (plot == nullptr) {
    return unexpected("the tab has no plot to draw in");
  }
  // A null return here means the title is taken, i.e. the curve is already drawn.
  plot->addCurve(*key);
  plot->replot();
  return okStatus();
}

Status PluginPlotTabsController::removeCurve(
    const QString& plugin_id, const QString& tab_id, const QString& topic, const QString& field,
    const QString& dataset_source) {
  PlotDocker* docker = ownedTab(plugin_id, tab_id);
  if (docker == nullptr) {
    return noOwnedTabError(tab_id);
  }
  const std::optional<QString> key =
      catalog_.resolveCurveKey(0, dataset_source, {}, topic, field, SeriesCapability::kPlottable);
  if (!key.has_value()) {
    return unexpected(("no plottable series '" + topic + u"/"_s + field + "' is loaded").toStdString());
  }
  bool removed = false;
  const int plot_count = docker->plotCount();
  for (int index = 0; index < plot_count; ++index) {
    DockWidget* dock = docker->plotAt(index);
    PlotWidget* plot = dock == nullptr ? nullptr : dock->plotWidget();
    if (plot == nullptr || plot->curveFromTitle(*key) == nullptr) {
      continue;
    }
    plot->removeCurve(*key);
    plot->replot();
    removed = true;
  }
  if (!removed) {
    return unexpected(("'" + topic + u"/"_s + field + "' is not drawn in that tab").toStdString());
  }
  return okStatus();
}

Status PluginPlotTabsController::clearTab(const QString& plugin_id, const QString& tab_id) {
  PlotDocker* docker = ownedTab(plugin_id, tab_id);
  if (docker == nullptr) {
    return noOwnedTabError(tab_id);
  }
  const int plot_count = docker->plotCount();
  for (int index = 0; index < plot_count; ++index) {
    DockWidget* dock = docker->plotAt(index);
    if (PlotWidget* plot = dock == nullptr ? nullptr : dock->plotWidget()) {
      plot->removeAllCurves();
      plot->replot();
    }
  }
  return okStatus();
}

}  // namespace PJ
