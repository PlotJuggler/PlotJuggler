#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// QUiLoader that instantiates the host-provided widgets (see createWidget in pj_ui_loader.cpp) that plugin .ui
// files declare by class name; plain Qt classes fall through to QUiLoader. Both engines use it, so every dialog
// and panel sees the same widget vocabulary.

#include <QUiLoader>

namespace PJ {

class PjUiLoader : public QUiLoader {
 public:
  explicit PjUiLoader(QObject* parent = nullptr);

  QWidget* createWidget(const QString& class_name, QWidget* parent, const QString& name) override;

  // Hides QUiLoader::load (not virtual): call it through a PjUiLoader. After the load, every button or action icon
  // the .ui names by a host resource path (`:/resources/svg/...`) is replaced by the loadSvg rendering for the active
  // theme, as host-built icons are: a raw <iconset> keeps the SVG's authoring ink, near-invisible in the dark theme.
  // Applied once; a theme switch while the dialog is open keeps the earlier tint.
  QWidget* load(QIODevice* device, QWidget* parent = nullptr);
};

}  // namespace PJ
