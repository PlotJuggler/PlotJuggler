#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QSet>
#include <QString>
#include <QWidget>

#include "pj_widgets/ChromeMetrics.h"

class QHBoxLayout;
class QLineEdit;

namespace PJ {

class SvgButton;

/// The one search and filter input across PJ4: a non-interactive magnifying-glass glyph flush against a borderless line
/// edit, sharing one flat background so they read as one control.
///
/// The glyph is a themed SvgButton made mouse-transparent, so it re-tints on a theme switch yet never steals a click. A
/// sibling button, not a QLineEdit leading action, because Qt hardcodes a leading-action icon to 16 px on fields
/// shorter than 34 px, so a sibling is the only way to get a chrome-sized glyph. A trailing clear "x" appears only
/// while the field holds text.
///
/// Background: the widget paints a QSS-driven flat fill (WA_StyledBackground); the child line edit and buttons are
/// transparent, so the fill shows through as one surface. Two tones through the `variant` dynamic property:
/// Variant::kBanner (default, `banner_input` tone) for header-band filters, Variant::kStandalone (`input` tone) for a
/// filter on its own surface.
///
/// Height tracks the app's icon-size chrome metric through setChromeMetrics(), so it lines up with every section band;
/// wire it to the host's chromeMetricsChanged broadcast (MainWindow does for band-hosted instances). Unwired, it keeps
/// the first-launch height.
class Search : public QWidget {
  Q_OBJECT
  Q_PROPERTY(QString placeholder READ placeholder WRITE setPlaceholder)
  Q_PROPERTY(QString fieldObjectName READ fieldObjectName WRITE setFieldObjectName)
 public:
  /// Background tone (see the class doc), selected through the `variant` dynamic property so the app stylesheet drives
  /// both fills.
  enum class Variant { kBanner, kStandalone };

  explicit Search(QWidget* parent = nullptr);
  ~Search() override;

  /// Placeholder shown when the field is empty; "Filter..." by default.
  void setPlaceholder(const QString& text);
  [[nodiscard]] QString placeholder() const;

  [[nodiscard]] QString text() const;
  void setText(const QString& text);
  void clear();

  /// The wrapped line edit, for advanced wiring (event filters, focus handling, returnPressed). Prefer text() /
  /// textChanged().
  [[nodiscard]] QLineEdit* lineEdit() const {
    return line_edit_;
  }

  /// Stamps an objectName on the inner line edit so a dialog host can bind its textChanged by name (as
  /// SectionHeaderBand::filterFieldName); on the inner field, not the Search frame, which keeps its own objectName when
  /// empty.
  void setFieldObjectName(const QString& name);
  [[nodiscard]] QString fieldObjectName() const;

  /// Selects the background tone (see Variant); kBanner by default.
  void setVariant(Variant variant);
  [[nodiscard]] Variant variant() const {
    return variant_;
  }

  /// Sizes the height and glyphs to the app icon metrics (icon_size + icon_padding), in step with the section bands.
  void setChromeMetrics(const ChromeMetrics& metrics);

 signals:
  /// Forwarded from the inner line edit on every text change.
  void textChanged(const QString& text);
  /// Forwarded when Return or Enter is pressed in the field.
  void returnPressed();

 protected:
  /// Tracks hover (across the frame and children) and the inner field's focus, so the border treatment applies to the
  /// whole control, not the line edit.
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  /// Toggles the `pjHovered` / `pjFocused` dynamic property and repolishes so the QSS border state updates; no-op when
  /// unchanged.
  void setStateProperty(const char* name, bool on);

  QHBoxLayout* layout_ = nullptr;
  SvgButton* glyph_ = nullptr;
  SvgButton* clear_button_ = nullptr;
  QLineEdit* line_edit_ = nullptr;
  Variant variant_ = Variant::kBanner;
  ChromeMetrics chrome_metrics_{};
  /// The frame and child widgets under the mouse. Hover is the union (any non-empty), so moving between the glyph,
  /// field and clear button reads as one continuous hover.
  QSet<QObject*> hovered_objects_;
};

}  // namespace PJ
