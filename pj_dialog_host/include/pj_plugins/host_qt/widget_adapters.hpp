#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QWidget>

class QDialog;

namespace PJ {

// Styled-widget adapters swap the plain controls of a plugin .ui for PlotJuggler's styled ones:
//
//   QRadioButton (exclusive group, 2+ buttons) -> DualOptionsWidget (segmented control)
//   QCheckBox                                   -> ToggleSwitch (inline label on the left)
//   QComboBox                                   -> PJ::ComboBox styling (gradient popup)
//   QPlainTextEdit + pjMarkdown                 -> QTextBrowser (Markdown view)
//
// For interactive controls the original stays hidden and alive as the source of truth for WidgetData and events,
// and the replacement drives it. The read-only Markdown promotion replaces its original outright, keeping the
// binding name. Adaptation is structural: engines run it once after load.

/// Adapts every adaptable control under `root` (radios, checkboxes, comboboxes, opted-in Markdown edits). Call
/// once after the .ui is loaded; idempotent.
void adaptStyledWidgets(QWidget* root);

/// When a plugin .ui's root is a QDialog embedded in the host's PJ::Dialog chrome, forwards the inner dialog's
/// finished(result) to the outer one. Otherwise Esc rejects and hides only the inner content while the outer modal
/// chrome stays open and empty. No-op when `content` is not a QDialog.
void forwardEmbeddedDialogClose(QWidget* content, QDialog* outer);

/// Per-kind entry points that adaptStyledWidgets calls; exposed for isolated tests.
void adaptRadioGroups(QWidget* root);
void adaptCheckBoxes(QWidget* root);
void adaptComboBoxes(QWidget* root);
/// Promotes child QPlainTextEdit widgets with the bool property `pjMarkdown` to read-only QTextBrowsers. The
/// replacement keeps the object name and dynamic properties, so plain_text binding addresses it. A QPlainTextEdit
/// that is `root` itself is not promoted (the adapter replaces children in their layout).
void adaptMarkdownEdits(QWidget* root);
/// Gives every QTableView under `root` with the bool property "pjInteriorGrid" an interior-only cell grid: native
/// showGrid is off and an item delegate draws only the dividers between cells, never the outer right and bottom
/// edge, which belongs to the surrounding chrome (header, splitter handle). Idempotent (marker property).
void adaptGridTables(QWidget* root);
/// Frames the footer of an edge-to-edge plugin dialog like the host's title bar. When `root`'s vertical layout has
/// zero left, right and bottom margins and its last item holds a QDialogButtonBox (the box, a wrapper widget or a
/// button row layout), that item moves into a host footer (objectName "pjDialogFooter") with a hairline above it
/// and one standard inset of padding, replacing the item's own outer margins. Padded dialogs are left alone.
/// Idempotent.
void adaptDialogFooter(QWidget* root);
/// Attaches PJ::Scrollbar overlays (one horizontal, one vertical) to every QAbstractScrollArea under `root` not yet
/// adapted. The native bars are forced to AlwaysOff; the overlays own hover, fade and drag. Idempotent (marker
/// property).
///
/// Per-area dynamic properties, set before this call:
///   pjScrollbarAutoHide (bool, default true): fade on hover vs always on
///   pjScrollbarFadeMs   (int,  default 150):  fade duration
void adaptScrollAreas(QWidget* root);

/// Adapts `w` if it just became adaptable (plugin data selected an option of a radio group, or gave a checkbox
/// its text). No-op if `w` is adapted already or not adaptable.
void tryAdaptStyledWidget(QWidget* w);

/// Pushes `w`'s freshly applied state (checked, enabled) onto its styled replacement. No-op if `w` is not an
/// adapted original.
void syncStyledWidget(QWidget* w);

/// If `w` is an adapted original whose replacement owns the visible geometry, records the wanted visibility on `w`
/// (the replacement derives its own) and returns true. False when `w` is not adapted: the caller sets visibility
/// on `w` directly.
[[nodiscard]] bool redirectAdaptedVisibility(QWidget* w, bool visible);

}  // namespace PJ
