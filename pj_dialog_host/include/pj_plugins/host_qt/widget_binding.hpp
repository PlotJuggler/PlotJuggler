#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <QWidget>
#include <functional>
#include <optional>
#include <pj_plugins/host/widget_data_view.hpp>
#include <string>
#include <string_view>

namespace PJ {

// Dynamic property a host sets on a panel ROOT: a QVariantList of QObject*
// pointing at widgets it moved out of the root's tree into its own chrome
// (a plugin's side drawer hoisted next to the title bar, say). Every by-name
// lookup the binding performs on the root also searches these, so a hoisted
// widget keeps receiving its data, rules and shortcuts unchanged. Signal
// wiring survives the reparent on its own.
inline constexpr const char* kHoistedWidgetsProperty = "pjHoistedWidgets";
/// A plain-text edit tagged `pjMarkdown` in the .ui is promoted after load to a
/// read-only QTextBrowser rendering Markdown (widget_adapters); the binder
/// then routes plain-text updates to it as Markdown.
inline constexpr const char* kMarkdownProperty = "pjMarkdown";

/// Callback for widget events: receives widget objectName + event JSON string.
using WidgetEventCallback = std::function<void(const std::string& widget_name, const std::string& event_json)>;

/// Resolve a plugin-supplied semantic icon id (setButtonIconNamed) to a themed
/// SVG resource path, or an empty QString for an unknown id (callers then leave
/// the widget icon untouched). The supported id set lives here as the single
/// source of truth so it can be unit-tested without constructing a dialog.
QString resolveNamedIconPath(std::string_view icon_name);

class AppSession;
class CatalogModel;

/// Apply widget data from a WidgetDataView to all matching child widgets of root.
/// Uses QSignalBlocker to prevent re-entrant signal firing during updates.
/// When session and catalog are provided, QFrame chart containers use a full
/// PlotWidget (zoom/tracker/legend) instead of ChartPreviewWidget.
/// Styled-widget adaptation (QRadioButton/QCheckBox/QComboBox → PJ controls)
/// lives in widget_adapters.hpp; the engines call adaptStyledWidgets() once
/// after loading the .ui, and applyWidgetData keeps the replacements in sync.
void applyWidgetData(
    QWidget* root, const PJ::WidgetDataView& view, AppSession* session = nullptr, CatalogModel* catalog = nullptr);

/// Connect primary change signals of all editable widgets under root
/// to the given callback. The callback receives the widget objectName and
/// an event JSON string built by WidgetEventBuilder.
void connectWidgetSignals(QWidget* root, WidgetEventCallback callback);

/// Create QShortcut objects for QPushButtons that declare a "shortcut" key
/// in the widget data. Each shortcut triggers click() on the target button.
/// Call once after the dialog is fully constructed and signals are connected.
void installButtonShortcuts(QWidget* root, const PJ::WidgetDataView& view);

/// Honor the declarative "pj_enable_when" / "pj_visible_when" dynamic
/// properties a plugin .ui may put on any widget: value
/// "<comboObjectName>:<index>[,<index>...]", or several such clauses joined by
/// ';' (e.g. "backend:1;mode:0,2") — the widget stays enabled / visible only
/// while EVERY clause's named QComboBox sits on one of that clause's indices
/// (AND across clauses; a single clause is the common case). Applies the
/// initial state and live-tracks index changes — needed because a modal
/// sub-dialog runs a nested event loop where the plugin cannot push updates
/// itself. A clause that fails to parse (unknown combo, no numeric index)
/// invalidates the whole rule (the widget just stays as authored).
/// Visibility goes through redirectAdaptedVisibility, so a swapped
/// checkbox/radio hides its adapter replacement rather than the hidden
/// original; tag both widgets of a QFormLayout/QGridLayout row for the row
/// itself to collapse. Call once per loaded tree, after adaptStyledWidgets.
void installDeclarativeRules(QWidget* root);

/// Re-asserts the rules installed under `root` (no re-parse, no signal
/// wiring). applyWidgetData calls this after every apply because plugin-pushed
/// combo changes arrive signal-blocked, and a plugin-pushed `enabled`/`visible`
/// on a rule-governed widget must not win over the rule.
void refreshDeclarativeRules(QWidget* root);

}  // namespace PJ
