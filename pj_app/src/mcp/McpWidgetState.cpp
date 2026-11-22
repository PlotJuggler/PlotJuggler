// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The widget_state app tool: a dock's XML state (what a layout saves for it) read, described and replaced.

#include <QDomDocument>
#include <cmath>
#include <string>

#include "LayoutXml.h"
#include "MainWindow.h"
#include "mcp/McpProtocol.h"
#include "mcp/McpServer.h"
#include "pj_plotting/DockWidget.h"
#include "pj_plotting/PlotDocker.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/IDataWidget.h"
#include "pj_runtime/XmlStateOptions.h"

namespace PJ {

using json = nlohmann::json;  // NOLINT(readability-identifier-naming)

namespace {

json ok(const json& payload) {
  return mcp::jsonResult(payload);
}

json fail(const std::string& message) {
  return mcp::textResult(message, true);
}

QDomElement saveState(DockWidget& dock, QDomDocument& doc) {
  if (PlotWidget* plot = dock.plotWidget(); plot != nullptr) {
    return plot->xmlSaveState(doc);
  }
  if (IDataWidget* widget = dock.objectWidget(); widget != nullptr) {
    return widget->xmlSaveState(doc);
  }
  return {};
}

bool validateState(DockWidget& dock, const QDomElement& element) {
  if (PlotWidget* plot = dock.plotWidget(); plot != nullptr) {
    return plot->xmlValidateState(element);
  }
  IDataWidget* widget = dock.objectWidget();
  return widget != nullptr && widget->xmlValidateState(element);
}

bool loadState(DockWidget& dock, const QDomElement& element) {
  if (PlotWidget* plot = dock.plotWidget(); plot != nullptr) {
    return plot->xmlLoadState(element);
  }
  IDataWidget* widget = dock.objectWidget();
  if (widget == nullptr || !widget->xmlLoadState(element)) {
    return false;
  }
  widget->finishRestore();  // the data its state names is already loaded
  return true;
}

// A float parser's open bound (lowest()/max()) says "any finite value"; printing 3.4e38 would only mislead.
bool isOpenBound(double bound) {
  return std::abs(bound) >= 1.0e30;
}

// Bounds of float attributes arrive widened from float (0.001f is 0.0010000000474974513); 7 significant digits is
// what the attribute can hold.
double floatPrecision(double value) {
  return QString::number(value, 'g', 7).toDouble();
}

json optionsJson(const std::vector<XmlOption>& options) {
  json out = json::object();
  for (const XmlOption& option : options) {
    json entry = {{"type", option.type.toStdString()}};
    if (!option.values.isEmpty()) {
      json values = json::array();
      for (const QString& value : option.values) {
        values.push_back(value.toStdString());
      }
      entry["values"] = std::move(values);
    }
    if (option.min.has_value() && !isOpenBound(*option.min)) {
      entry["min"] = floatPrecision(*option.min);
    }
    if (option.max.has_value() && !isOpenBound(*option.max)) {
      entry["max"] = floatPrecision(*option.max);
    }
    if (!option.fallback.isEmpty()) {
      entry["default"] = option.fallback.toStdString();
    }
    out[option.element.toStdString()][option.attribute.toStdString()] = std::move(entry);
  }
  return out;
}

}  // namespace

void McpServer::widgetState(const json& args, const Caller&, Reply reply) {
  const std::string action = args.value("action", "");
  if (action == "list") {
    json docks = json::array();
    window_.forEachTabDock([&docks](PlotDocker* tab, DockWidget* dock) {
      QDomDocument doc;
      const QDomElement state = saveState(*dock, doc);
      docks.push_back(
          {{"id", dock->stateId().toStdString()},
           {"tab", tab->name().toStdString()},
           {"title", dock->name().toStdString()},
           {"kind", state.isNull() ? std::string("empty") : state.tagName().toStdString()}});
    });
    reply(ok(docks));
    return;
  }
  if (action != "get" && action != "set") {
    reply(fail("action must be list, get or set"));
    return;
  }
  const QString id = QString::fromStdString(args.value("dock", ""));
  DockWidget* dock = nullptr;
  window_.forEachTabDock([&](PlotDocker*, DockWidget* candidate) {
    if (dock == nullptr && candidate->stateId() == id) {
      dock = candidate;
    }
  });
  if (dock == nullptr) {
    reply(fail("no dock '" + id.toStdString() + "': list gives the ids"));
    return;
  }

  QDomDocument current_doc;
  QDomElement current = saveState(*dock, current_doc);
  if (current.isNull()) {
    reply(fail("this dock holds no state: drop a series or an object topic on it first"));
    return;
  }
  current_doc.appendChild(current);

  if (action == "get") {
    XmlOptionsRecorder recorder;
    validateState(*dock, current);
    reply(
        ok({{"dock", id.toStdString()},
            {"xml", current_doc.toString(2).toStdString()},
            {"options", optionsJson(recorder.options())}}));
    return;
  }

  QDomDocument doc;
  const QDomDocument::ParseResult parsed = doc.setContent(QString::fromStdString(args.value("xml", "")));
  if (!parsed) {
    reply(fail(
        "xml does not parse (line " + std::to_string(parsed.errorLine) + "): " + parsed.errorMessage.toStdString()));
    return;
  }
  QDomElement element = doc.documentElement();
  if (element.tagName() != current.tagName()) {
    reply(fail("the root element must stay <" + current.tagName().toStdString() + ">, as get returned it"));
    return;
  }
  if (dock->plotWidget() != nullptr) {
    // A plot's curves are saved by series path; the session key they bind to is resolved here, as a layout load does.
    layout_xml::normalizePlotRangeBasis(doc);
    const CatalogModel& catalog = window_.appSession().catalogModel();
    const QList<layout_xml::SeriesPath> missing = layout_xml::rebindCurveKeys(
        doc, [&catalog](const layout_xml::SeriesPath& path) { return catalog.resolveCurveKey(path); });
    if (!missing.isEmpty()) {
      std::string names;
      for (const layout_xml::SeriesPath& path : missing) {
        names += (names.empty() ? "" : ", ") + path.display().toStdString();
      }
      reply(fail("no loaded series for " + names + "; nothing changed"));
      return;
    }
    dock->plotWidget()->stampLiveCurveKeys(current);  // the rollback binds what is shown now
  }
  {
    XmlOptionsRecorder recorder;
    // A rejection with a passing validation is a value a layout load would silently replace: refused here too.
    if (!validateState(*dock, element) || !recorder.firstRejection().isEmpty()) {
      const QString& why = recorder.firstRejection();
      reply(fail(
          "refused, nothing changed: " +
          (why.isEmpty() ? std::string("the element does not validate (get lists the options)") : why.toStdString())));
      return;
    }
  }
  if (!loadState(*dock, element)) {
    const bool restored = loadState(*dock, current);
    reply(fail(
        restored ? "the dock refused the element while applying it; it is back as it was"
                 : "the dock refused the element and could not restore its previous state; undo restores it"));
    if (!restored) {
      window_.recordAgentUndoStep();
    }
    return;
  }
  window_.recordAgentUndoStep();
  QDomDocument applied_doc;
  applied_doc.appendChild(saveState(*dock, applied_doc));
  reply(ok({{"dock", id.toStdString()}, {"xml", applied_doc.toString(2).toStdString()}}));
}

}  // namespace PJ
