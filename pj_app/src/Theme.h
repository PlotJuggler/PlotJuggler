#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>
#include <QString>
#include <QStringList>

namespace PJ {

// QSS source format: a PALETTE START block of `key: value` lines (one
// per line, anchored to start-of-line) followed by a PALETTE END line,
// then the body using `${KEY}` placeholders. See stylesheet_*.qss.
class Theme : public QObject {
  Q_OBJECT
 public:
  explicit Theme(QObject* parent = nullptr);

  static QStringList availableThemes();

  QString currentTheme() const;

  QString expandedQss() const;

  // The Qt color scheme matching the current theme. Requested from Qt on every
  // switch so a platform-drawn title bar follows it; a platform may ignore that.
  Qt::ColorScheme colorScheme() const;

 public slots:
  void setTheme(const QString& name);

 signals:
  // Fires when the theme name changes. Listeners refresh palette-tinted
  // SVG icons from the new theme.
  void themeChanged(const QString& name);

  // Fires whenever expandedQss() changes. Listeners reapply the
  // application stylesheet.
  void qssChanged();

 private:
  void rebuildQss();

  QString name_;
  QString expanded_qss_;
};

}  // namespace PJ
