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

// The `.ui` dynamic property (a plugin-facing contract, see pj_dialog_host/CLAUDE.md) tagging the one widget a
// toolbox panel wants hoisted into a full-height column at the left of the chrome.
inline constexpr const char* kToolboxSideDrawerProperty = "pjToolboxSideDrawer";

// The splitter holding a hoisted side drawer; null when the panel has none.
QSplitter* toolboxDrawerSplitter(QWidget* container);

// Per plugin (or manifest) identity, so toolboxes do not share one saved width.
QString toolboxDrawerWidthSettingsKey(const QString& persist_key);

// A banner or floating-title-bar button with the shared chrome cursor; the caller places it.
SvgButton* makeChromeButton(const QString& icon_path, const QString& tooltip, QWidget* parent);

// A proxy for a plugin button tagged pjToolboxChromeAction: clicks go to the original, so its routing is
// untouched. help.svg when the plugin names no icon; the caller places it.
SvgButton* makeChromeActionProxy(QAbstractButton* src, QWidget* parent);

std::vector<QAbstractButton*> taggedChromeActions(QWidget* content);

bool isLeadingChromeAction(const QAbstractButton* src);

}  // namespace PJ
