#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QList>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "pj_widgets/ChromeMetrics.h"

class QChildEvent;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QLayoutItem;
class QLineEdit;
class QPushButton;

namespace PJ {

class Search;

// The canonical section header band — the titlebar-tone strip that titles panel
// sections ("Grid", "Curve Width", …) and the FFT/plugin panels. The background
// comes from the host app's stylesheet via the class selector
// (PJ--SectionHeaderBand); the 2-px leading indent is baked here so every band
// reads the same without per-instance QSS.
//
// Height is the canonical `ChromeMetrics::bandHeight()`, so every band in the
// app reads at one height (the left-panel "Sources" header, etc.). It starts at the
// default-metrics height; the host keeps it in step by routing its
// `chromeMetricsChanged` broadcast to onChromeMetricsChanged (for plugin-loaded
// bands, MainWindow walks the panel and wires this after the .ui is inflated).
// Do NOT call setFixedHeight from outside — that reintroduces the per-instance
// divergence this widget exists to remove.
//
// Optional inline filter: set `filterPlaceholder` (in code or via the .ui
// property) to grow a trailing search-glyph + flat QLineEdit on the band — the
// same "filter banner" the left-panel "Datasets"/"Custom Series" headers show.
// The field is flat-styled by the `PJ--SectionHeaderBand QLineEdit` QSS rule, so
// it reads as part of the band, not a separate input. Give it `filterFieldName`
// so the dialog host can bind its textChanged to the plugin (the QLineEdit gets
// that objectName). Access the field via filterEdit().
class SectionHeaderBand : public QWidget {
  Q_OBJECT
  Q_PROPERTY(QString text READ text WRITE setText)
  Q_PROPERTY(bool fillDockedWidgets READ fillDockedWidgets WRITE setFillDockedWidgets)
  Q_PROPERTY(QString titleObjectName READ titleObjectName WRITE setTitleObjectName)
  Q_PROPERTY(QString filterPlaceholder READ filterPlaceholder WRITE setFilterPlaceholder)
  Q_PROPERTY(QString filterFieldName READ filterFieldName WRITE setFilterFieldName)
  Q_PROPERTY(QString trailingComboName READ trailingComboName WRITE setTrailingComboName)
  Q_PROPERTY(bool comboExpanding READ comboExpanding WRITE setComboExpanding)
  Q_PROPERTY(bool comboEditable READ comboEditable WRITE setComboEditable)
  Q_PROPERTY(QString trailingButtonName READ trailingButtonName WRITE setTrailingButtonName)
  Q_PROPERTY(QString trailingButtonIcon READ trailingButtonIcon WRITE setTrailingButtonIcon)
  Q_PROPERTY(QStringList trailingButtonNames READ trailingButtonNames WRITE setTrailingButtonNames)
  Q_PROPERTY(QStringList trailingButtonToolTips READ trailingButtonToolTips WRITE setTrailingButtonToolTips)
  Q_PROPERTY(QString trailingToggleName READ trailingToggleName WRITE setTrailingToggleName)
  Q_PROPERTY(QString trailingToggleText READ trailingToggleText WRITE setTrailingToggleText)
  Q_PROPERTY(QString trailingToggleToolTip READ trailingToggleToolTip WRITE setTrailingToggleToolTip)
 public:
  explicit SectionHeaderBand(const QString& title, QWidget* parent = nullptr);
  // For uic: a band placed in a .ui file gets its title through the `text` property.
  explicit SectionHeaderBand(QWidget* parent = nullptr) : SectionHeaderBand(QString(), parent) {}

  void setText(const QString& title);
  [[nodiscard]] QString text() const;

  // Ordinary docked inputs keep the app-wide input-row height. Enable this for
  // bands whose docked controls should fill the band's full content box.
  void setFillDockedWidgets(bool fill);
  [[nodiscard]] bool fillDockedWidgets() const {
    return fill_docked_widgets_;
  }

  // objectName stamped on the inner title QLabel so a dialog host can retitle
  // the band by name (setLabel), mirroring Search::fieldObjectName. Defaults to
  // empty (the label stays anonymous).
  void setTitleObjectName(const QString& name);
  [[nodiscard]] QString titleObjectName() const;

  // Enable/relabel the trailing inline filter. A non-empty placeholder grows the
  // search glyph + field on first use; empty leaves the band a plain title strip.
  void setFilterPlaceholder(const QString& placeholder);
  [[nodiscard]] QString filterPlaceholder() const;

  // objectName stamped on the filter QLineEdit so the dialog-host binding can
  // route its textChanged to the plugin. Safe to set before or after the field
  // exists. Defaults to "bandFilter".
  void setFilterFieldName(const QString& name);
  [[nodiscard]] QString filterFieldName() const;

  // The inline filter field's line edit, or nullptr until a placeholder enables
  // it. Backed by the canonical Search control (see filterSearch()).
  [[nodiscard]] QLineEdit* filterEdit() const;

  // The inline filter control, or nullptr until a placeholder enables it.
  [[nodiscard]] Search* filterSearch() const {
    return filter_search_;
  }

  // Dock a combo box on the RIGHT of the band, turning a "Section title" strip
  // into a compact "Section title ........ [dropdown]" header row. The band
  // creates the combo lazily and stamps `name` as its objectName, so the dialog
  // host populates/binds it exactly as it would a standalone .ui combo (found by
  // objectName). Intended as an alternative to placing the combo on its own line
  // below the band. Mutually exclusive with the inline filter in practice.
  void setTrailingComboName(const QString& name);
  [[nodiscard]] QString trailingComboName() const;

  // The docked combo, or nullptr until setTrailingComboName enables it.
  [[nodiscard]] QComboBox* trailingCombo() const {
    return trailing_combo_;
  }

  // Expanding-combo mode: instead of docking on the right, the combo sits right
  // after the title and stretches to fill the band — the "label + input" banner
  // row (Surface::BannerInput), e.g. a "Server:" URL bar. Mutually exclusive
  // with the inline filter. Order-independent with setTrailingComboName.
  void setComboExpanding(bool expanding);
  [[nodiscard]] bool comboExpanding() const {
    return combo_expanding_;
  }

  // Make the trailing combo editable (free-typed values, e.g. a server URI).
  // Order-independent with setTrailingComboName.
  void setComboEditable(bool editable);
  [[nodiscard]] bool comboEditable() const {
    return combo_editable_;
  }

  // Dock SEVERAL flat, icon-only action buttons on the right of the band, in
  // list order. Each name becomes one button's objectName so the dialog host
  // wires clicks / named icons exactly as for a standalone button. The optional
  // tooltip list pairs by index. Buttons ride the chrome metrics (bandHeight
  // box, icon_size icon). The singular trailingButtonName API remains for the
  // one-button case; don't mix the two on one band.
  void setTrailingButtonNames(const QStringList& names);
  [[nodiscard]] QStringList trailingButtonNames() const;
  void setTrailingButtonToolTips(const QStringList& tooltips);
  [[nodiscard]] QStringList trailingButtonToolTips() const;

  // A checkable, flat TEXT affordance docked before the trailing buttons — the
  // classic filter-modifier toggle (e.g. ".*" for regex). `name` becomes its
  // objectName for host click routing; the checked state renders via the app
  // QSS :checked rules.
  void setTrailingToggleName(const QString& name);
  [[nodiscard]] QString trailingToggleName() const;
  void setTrailingToggleText(const QString& text);
  [[nodiscard]] QString trailingToggleText() const;
  void setTrailingToggleToolTip(const QString& tooltip);
  [[nodiscard]] QString trailingToggleToolTip() const;

  // The docked toggle, or nullptr until one of the setters enables it.
  [[nodiscard]] QPushButton* trailingToggle() const {
    return trailing_toggle_;
  }

  // Dock a flat, icon-only action button on the RIGHT of the band (e.g. an
  // "add folder" affordance beside the section title). `name` becomes the
  // button's objectName, so the dialog host wires its click exactly as it would
  // a standalone QPushButton (folder/file picker, etc.). `svgPath` is an SVG
  // resource path, theme-recoloured via loadSvg. Order-independent: set either
  // property first.
  void setTrailingButtonName(const QString& name);
  [[nodiscard]] QString trailingButtonName() const;
  void setTrailingButtonIcon(const QString& svgPath);
  [[nodiscard]] QString trailingButtonIcon() const;

  // The docked action button, or nullptr until one of the setters enables it.
  [[nodiscard]] QPushButton* trailingButton() const {
    return trailing_button_;
  }

  // Resize to the canonical band height for these metrics. Connect the host's
  // chromeMetricsChanged signal here so the band scales with the icon size.
  void onChromeMetricsChanged(const ChromeMetrics& metrics);

  // Dock an arbitrary widget on the trailing (right) side of the band, after the
  // title's stretch. This is what lets the band act as a container: a widget
  // nested inside the band in a .ui file is reparented onto it by the loader and
  // auto-docked here (see childEvent), in declaration order, so plugin panels can
  // group e.g. a checkbox + radios into the header strip.
  //
  // Docked widgets use the app-wide input-row height by default, or the band's
  // full content box when fillDockedWidgets is enabled. They also carry a
  // `pjBandDocked` dynamic property, which the app stylesheet keys on to lift
  // the single-input-row height cap it pins on the self-painted PJ controls —
  // QStyleSheetStyle enforces that cap over the layout, so without the property
  // those controls could not grow.
  void addTrailingWidget(QWidget* widget);

 protected:
  // Auto-docks externally-reparented children (the .ui-nested case above). The
  // band's OWN sub-widgets are created under the creating_internal_ guard and
  // self-insert, so they are not re-docked here.
  void childEvent(QChildEvent* event) override;

 private:
  // Remove the title's trailing stretch once an expanding control (filter or
  // expanding combo) takes over the middle of the band. Idempotent.
  void takeStretch();
  // Grow the search glyph + filter field on first request (idempotent).
  void ensureFilter();
  // Create the right-docked combo on first request (idempotent).
  void ensureTrailingCombo();
  // Size the trailing combo: filling the band's content box vertically when
  // fillDockedWidgets is set (snug top/bottom band padding), else its natural
  // input-row height capped at contentHeight(). Idempotent.
  void applyTrailingComboHeight();
  // Create the right-docked action button on first request (idempotent).
  void ensureTrailingButton();
  // Create the checkable text toggle on first request (idempotent).
  void ensureTrailingToggle();
  // Grow the plural trailing-button list to `count` buttons (idempotent).
  void ensureTrailingButtons(int count);
  // Layout index where the next toggle/button should go, keeping the canonical
  // trailing order [.. toggle][buttons..][docked combo] whatever the .ui
  // property order was.
  [[nodiscard]] int trailingInsertIndex(bool before_buttons) const;
  // Apply the canonical title-band insets + spacing to the band's layout.
  void applyBandLayoutMetrics();
  // Height left for chrome once the band's vertical insets are taken
  // (icon_size + icon_padding). Every control the band sizes uses this, so
  // nothing butts against the band's edges.
  [[nodiscard]] int contentHeight() const;
  // Size one metric-tracked band button (contentHeight box, icon_size icon).
  void applyButtonMetrics(QPushButton* button) const;
  // Pin one docked widget to the band height. A fixed height (rather than a
  // stretching size policy) is what survives a later setSizePolicy from whoever
  // adapts the widget — the dialog host swaps a docked QCheckBox for a
  // ToggleSwitch and re-declares its policy after the dock.
  void applyDockedWidgetMetrics(QWidget* widget) const;

  // >0 while the band constructs its OWN sub-widgets (title/filter/combo/buttons/
  // toggle), so childEvent can tell those apart from .ui-nested external children.
  int creating_internal_ = 0;

  QHBoxLayout* layout_ = nullptr;
  QLabel* label_ = nullptr;
  // The ctor's title stretch; nulled once an expanding control replaces it.
  QLayoutItem* stretch_ = nullptr;
  Search* filter_search_ = nullptr;
  QString filter_field_name_ = QStringLiteral("bandFilter");
  // Last metrics seen, so a filter created after the host's broadcast still
  // sizes correctly (ensureFilter seeds the new Search from this).
  ChromeMetrics current_metrics_{};
  QComboBox* trailing_combo_ = nullptr;
  QString trailing_combo_name_;
  bool combo_expanding_ = false;
  bool combo_editable_ = false;
  QPushButton* trailing_button_ = nullptr;
  QString trailing_button_name_;
  QString trailing_button_icon_;
  QList<QPushButton*> trailing_buttons_;
  QStringList trailing_button_names_;
  QStringList trailing_button_tooltips_;
  QPushButton* trailing_toggle_ = nullptr;
  QString trailing_toggle_name_;
  QString trailing_toggle_text_;
  QString trailing_toggle_tooltip_;
  bool fill_docked_widgets_ = false;
  // Externally docked widgets, kept so a later metrics change re-pins their
  // height. Guarded pointers: the host deletes and re-creates docked controls
  // when it adapts them (QCheckBox -> ToggleSwitch).
  QList<QPointer<QWidget>> docked_widgets_;
};

}  // namespace PJ
