// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT

#include "pj_plugins/host_qt/pj_ui_loader.hpp"

#include <pj_widgets/ComboBox.h>
#include <pj_widgets/CredentialsEditor.h>
#include <pj_widgets/DateRangePicker.h>
#include <pj_widgets/MarkerTimeline.h>
#include <pj_widgets/RangeSlider.h>
#include <pj_widgets/Search.h>
#include <pj_widgets/SectionHeaderBand.h>
#include <pj_widgets/ToggleSwitch.h>

namespace PJ {

PjUiLoader::PjUiLoader(QObject* parent) : QUiLoader(parent) {
  // No widgets from external designer plugin paths: only standard Qt widgets and the ones below.
  setLanguageChangeEnabled(false);
}

QWidget* PjUiLoader::createWidget(const QString& class_name, QWidget* parent, const QString& name) {
  QWidget* w = nullptr;
  if (class_name == QLatin1String("RangeSlider")) {
    w = new RangeSlider(Qt::Horizontal, RangeSlider::kDoubleHandles, parent);
  } else if (class_name == QLatin1String("MarkerTimeline")) {
    w = new MarkerTimeline(parent);
  } else if (class_name == QLatin1String("DateRangePicker")) {
    w = new DateRangePicker(parent);
  } else if (class_name == QLatin1String("CredentialsEditor")) {
    w = new CredentialsEditor(parent);
  } else if (class_name == QLatin1String("SectionHeaderBand")) {
    w = new SectionHeaderBand(QString(), parent);
  } else if (class_name == QLatin1String("Search")) {
    // Plugins bind the field by name through the `fieldObjectName` .ui property (the inner QLineEdit gets it).
    w = new Search(parent);
  } else if (class_name == QLatin1String("ToggleSwitch")) {
    w = new ToggleSwitch(parent);
  } else if (class_name == QLatin1String("PJ::ComboBox")) {
    // .ui files opt in with the promoted-widget class name, as pj_app's .ui files do.
    w = new ComboBox(parent);
  }

  if (w != nullptr) {
    if (!name.isEmpty()) {
      w->setObjectName(name);
    }
    return w;
  }
  return QUiLoader::createWidget(class_name, parent, name);
}

}  // namespace PJ
