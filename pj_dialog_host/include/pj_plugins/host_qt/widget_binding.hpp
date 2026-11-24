#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <QWidget>
#include <functional>
#include <optional>
#include <pj_plugins/host/widget_data_view.hpp>
#include <pj_plugins/host_qt/embedded_view_host.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace PJ {

// Dynamic property a host sets on a panel root: a QVariantList of QObject* for the widgets it moved out of the
// root's tree into its own chrome (a plugin's side drawer hoisted next to the title bar). Every by-name lookup the
// binding makes on the root also searches them, so a hoisted widget keeps its data, rules and shortcuts. Signal
// wiring survives the reparent on its own.
inline constexpr const char* kHoistedWidgetsProperty = "pjHoistedWidgets";
/// A plain-text edit tagged `pjMarkdown` in the .ui is promoted after load to a read-only QTextBrowser
/// (widget_adapters); the binder routes plain-text updates to it as Markdown.
inline constexpr const char* kMarkdownProperty = "pjMarkdown";

/// Item-data role (bool) on every item of a locked table row: a row the plugin both disables (`disabled_rows`) and
/// selects. The item stays disabled, so clicks, keyboard and selection never reach it and `selectionChanged` never
/// carries it, and the host delegates paint it as selected. A reader reporting selection state (the MCP ui_tree)
/// counts a disabled row whose first item holds bool true here as selected. Travels with the item when the user
/// sorts. The value sits far above Qt::UserRole, clear of the small offsets other models use.
inline constexpr int kLockedRowRole = Qt::UserRole + 0x504A;

/// Receives the widget's objectName and the event JSON.
using WidgetEventCallback = std::function<void(const std::string& widget_name, const std::string& event_json)>;

/// Resolves a plugin's semantic icon id (setButtonIconNamed) to a themed SVG resource path; empty for an unknown id
/// (the caller leaves the icon alone). The supported id set is defined here.
QString resolveNamedIconPath(std::string_view icon_name);

/// Where a plugin picker opens: a structured picker's initial_directory and suggested_name, kept apart so
/// FileDialog never stats a path to tell them apart. Both empty for a legacy picker, and for a structured one that
/// fails validation (logged): the dialog keeps its default folder.
struct FilePickerStart {
  QString dir;
  QString name;
};
FilePickerStart filePickerStart(const WidgetDataView& view, std::string_view name);

class AppSession;
class CatalogModel;

/// Applies widget data to the matching child widgets of `root`, with their signals blocked. With a session and
/// catalog, QFrame chart containers use a full PlotWidget (zoom, tracker, legend) instead of ChartPreviewWidget.
/// With `embedded_views`, a QFrame carrying the `scene_view` key hosts a view built by it, its topics reconciled by
/// difference; without, such frames are left untouched. Returns the `scene_view` requests it could not honour, in
/// request order, for the caller to deliver to the plugin after this call, never from inside it. An unchanged
/// request is not re-applied: a refused topic is reported, and retried, again only once the frame's kind or
/// topic set changes. The engines call adaptStyledWidgets() once after loading the .ui; this keeps the replacements
/// in sync.
std::vector<EmbeddedViewFailure> applyWidgetData(
    QWidget* root, const PJ::WidgetDataView& view, AppSession* session = nullptr, CatalogModel* catalog = nullptr,
    const EmbeddedViewHost* embedded_views = nullptr);

/// Connects the primary change signals of every editable widget under `root` to `callback`.
void connectWidgetSignals(QWidget* root, WidgetEventCallback callback);

/// Creates a QShortcut, clicking the button, for each QPushButton that declares a "shortcut" key in the widget
/// data. Call once, after the dialog is built and its signals are connected.
void installButtonShortcuts(QWidget* root, const PJ::WidgetDataView& view);

/// Honors the "pj_enable_when" / "pj_visible_when" dynamic properties a plugin .ui may set on any widget: value
/// "<comboObjectName>:<index>[,<index>...]", or clauses joined by ';' ("backend:1;mode:0,2"). The widget stays
/// enabled or visible only while every clause's named QComboBox sits on one of that clause's indices. Applies the
/// initial state and tracks index changes live, for the places the plugin cannot push updates itself (a modal
/// sub-dialog's nested event loop). A clause that fails to parse (unknown combo, no numeric index) invalidates the
/// whole rule: the widget stays as authored. Visibility goes through redirectAdaptedVisibility, so a swapped
/// checkbox or radio hides its replacement, not the hidden original; tag both widgets of a QFormLayout or
/// QGridLayout row for the row to collapse. Call once per loaded tree, after adaptStyledWidgets.
void installDeclarativeRules(QWidget* root);

/// Re-asserts the rules installed under `root` (no re-parse, no signal wiring). applyWidgetData calls it after
/// every apply: plugin-pushed combo changes arrive signal-blocked, and a plugin-pushed `enabled` or `visible` on a
/// rule-governed widget must not win over the rule.
void refreshDeclarativeRules(QWidget* root);

}  // namespace PJ
