#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QWidget>

class QDialog;

namespace PJ {

// Styled-widget adapters: swap plain controls that come out of a plugin's .ui
// for PlotJuggler's own styled equivalents, while keeping the ORIGINAL control
// hidden and alive so plugin data/events keep flowing through it unchanged.
//
//   QRadioButton (exclusive group, 2+ buttons) -> DualOptionsWidget (segmented control)
//   QCheckBox                                   -> ToggleSwitch (inline label on the left)
//   QComboBox                                   -> PJ::ComboBox styling (gradient popup)
//   QPlainTextEdit + pjMarkdown                 -> QTextBrowser (Markdown view)
//
// For interactive controls, the originals stay hidden and remain the source of
// truth for WidgetData/events while the styled replacements drive them. The
// read-only Markdown promotion replaces its original outright while preserving
// the binding name. Adaptation is structural, so engines run it once after load.

/// Adapt every adaptable control under `root` (radios, checkboxes, comboboxes,
/// and opted-in Markdown edits).
/// Call once after the .ui is loaded. Idempotent and safe to re-run.
void adaptStyledWidgets(QWidget* root);

/// When a plugin .ui's ROOT is a QDialog embedded as content inside the host's
/// PJ::Dialog chrome, forward the inner dialog's finished(result) to the outer
/// dialog. Without this, Esc lands on the inner QDialog::keyPressEvent, which
/// rejects and hides ONLY the content while the outer modal chrome stays open
/// and empty. No-op when `content` is not a QDialog.
void forwardEmbeddedDialogClose(QWidget* content, QDialog* outer);

/// Per-kind entry points (adaptStyledWidgets calls each). Exposed individually
/// so each adapter can be unit-tested in isolation.
void adaptRadioGroups(QWidget* root);
void adaptCheckBoxes(QWidget* root);
void adaptComboBoxes(QWidget* root);
/// Promote child QPlainTextEdit widgets with dynamic bool property `pjMarkdown`
/// to read-only QTextBrowser instances after .ui properties are available. The
/// replacement keeps the authored object name and dynamic properties, so normal
/// WidgetData plain_text binding addresses it. A QPlainTextEdit used as `root`
/// itself is outside this structural adapter's child-in-layout contract.
void adaptMarkdownEdits(QWidget* root);
/// Give every QTableView under `root` that opts in (dynamic bool property
/// "pjInteriorGrid", set in the plugin .ui) an interior-only cell grid: native
/// showGrid is turned off and an item delegate draws only the dividers BETWEEN
/// cells, never the table's outer right/bottom edge. Keeps internal H+V gridlines
/// while leaving the table's edges to the surrounding chrome (header, splitter
/// handle), so a stretched-column table no longer doubles the native grid's
/// trailing line against the chrome. Idempotent (marker property).
void adaptGridTables(QWidget* root);
/// Attach a PJ::Scrollbar overlay (one horizontal + one vertical) to every
/// QAbstractScrollArea found under `root` that has not yet been adapted. The
/// native bars are hidden (policy forced to AlwaysOff); the pill overlays own
/// hover/fade/drag. Idempotent: a marker property prevents double-attachment.
///
/// Per-area config (set as dynamic properties BEFORE calling this function):
///   pjScrollbarAutoHide (bool, default true)  — fade-on-hover vs always-on
///   pjScrollbarFadeMs   (int,  default 150)   — fade animation duration
void adaptScrollAreas(QWidget* root);

/// Reactively adapt the single widget `w` if it just became adaptable (e.g.
/// plugin data selected one option of a previously-unselected radio group, or
/// gave a checkbox its text). No-op if `w` is already adapted or not adaptable.
void tryAdaptStyledWidget(QWidget* w);

/// Push `w`'s freshly-applied state (checked / enabled) onto its styled
/// replacement. No-op if `w` is not an adapted original.
void syncStyledWidget(QWidget* w);

/// If `w` is an adapted original whose replacement owns the visible geometry,
/// stash the desired visibility on `w` (the replacement derives its own) and
/// return true. Returns false when `w` is not adapted — the caller should then
/// apply visibility to `w` directly.
[[nodiscard]] bool redirectAdaptedVisibility(QWidget* w, bool visible);

}  // namespace PJ
