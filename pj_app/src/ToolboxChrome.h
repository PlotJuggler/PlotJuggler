// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QString>
#include <vector>

class QAbstractButton;
class QSplitter;
class QWidget;

namespace PJ {

class SvgButton;

// The `.ui` dynamic property (documented in pj_dialog_host/CLAUDE.md) tagging
// the one widget a toolbox panel wants hoisted out of its content into a
// full-height column at the left of the whole chrome. The chrome-action
// properties are read only through taggedChromeActions/makeChromeActionProxy.
inline constexpr const char* kToolboxSideDrawerProperty = "pjToolboxSideDrawer";

// The splitter wrapToolboxPanel put a hoisted side drawer in (outer's only
// child), or null when the panel has no drawer.
QSplitter* toolboxDrawerSplitter(QWidget* container);

// QSettings key for a persisted drawer width, keyed per plugin (or manifest)
// identity so different toolboxes don't fight over one saved width.
QString toolboxDrawerWidthSettingsKey(const QString& persist_key);

// A chrome button (banner or floating title bar): SVG icon, tooltip and the
// pointing-hand cursor every chrome control shares. The caller places it.
SvgButton* makeChromeButton(const QString& icon_path, const QString& tooltip, QWidget* parent);

// Stands a chrome proxy in for a plugin button tagged pjToolboxChromeAction:
// same icon (help.svg when the plugin names none), tooltip and cursor, clicks
// forwarded to the original so its routing is untouched. The caller places it.
SvgButton* makeChromeActionProxy(QAbstractButton* src, QWidget* parent);

std::vector<QAbstractButton*> taggedChromeActions(QWidget* content);

bool isLeadingChromeAction(const QAbstractButton* src);

}  // namespace PJ
