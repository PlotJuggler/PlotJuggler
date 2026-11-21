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
#include <pj_widgets/SvgUtil.h>
#include <pj_widgets/ToggleSwitch.h>

#include <QAbstractButton>
#include <QAction>
#include <QBuffer>
#include <QXmlStreamReader>
#include <utility>
#include <vector>

namespace PJ {

namespace {

// (object name, resource path) of each `icon` property of a <widget> or <action> that names a host SVG. The first text
// inside the property is the path: `<iconset><normaloff>path</normaloff>path</iconset>`, or `<iconset>path</iconset>`.
std::vector<std::pair<QString, QString>> hostIconRefs(const QByteArray& ui) {
  std::vector<std::pair<QString, QString>> refs;
  QXmlStreamReader xml(ui);
  QStringList owners;
  bool in_icon = false;
  while (!xml.atEnd()) {
    switch (xml.readNext()) {
      case QXmlStreamReader::StartElement:
        if (xml.name() == u"widget" || xml.name() == u"action") {
          owners.push_back(xml.attributes().value(u"name").toString());
        } else if (xml.name() == u"property") {
          in_icon = xml.attributes().value(u"name") == u"icon";
        }
        break;
      case QXmlStreamReader::EndElement:
        if ((xml.name() == u"widget" || xml.name() == u"action") && !owners.isEmpty()) {
          owners.pop_back();
        } else if (xml.name() == u"property") {
          in_icon = false;
        }
        break;
      case QXmlStreamReader::Characters:
        if (in_icon && !xml.isWhitespace() && !owners.isEmpty()) {
          const QString path = xml.text().trimmed().toString();
          if (path.startsWith(u":/resources/svg/")) {
            refs.emplace_back(owners.back(), path);
          }
          in_icon = false;
        }
        break;
      default:
        break;
    }
  }
  return refs;
}

}  // namespace

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

QWidget* PjUiLoader::load(QIODevice* device, QWidget* parent) {
  QBuffer buffer;
  buffer.setData(device->readAll());
  buffer.open(QIODevice::ReadOnly);
  QWidget* root = QUiLoader::load(&buffer, parent);
  if (root == nullptr) {
    return nullptr;
  }
  for (const auto& [name, path] : hostIconRefs(buffer.data())) {
    // An unnamed widget would make findChild match any object.
    if (name.isEmpty()) {
      continue;
    }
    QObject* target = root->objectName() == name ? root : root->findChild<QObject*>(name);
    if (qobject_cast<QAbstractButton*>(target) != nullptr || qobject_cast<QAction*>(target) != nullptr) {
      target->setProperty("_pj_host_icon", path);
    }
  }
  retintHostIcons(root);
  return root;
}

void PjUiLoader::retintHostIcons(QObject* root) {
  const QString theme = currentTheme();
  QList<QObject*> objects = root->findChildren<QObject*>();
  objects.prepend(root);
  for (QObject* object : objects) {
    const QString path = object->property("_pj_host_icon").toString();
    if (path.isEmpty()) {
      continue;
    }
    const QIcon icon(loadSvg(path, theme));
    if (auto* button = qobject_cast<QAbstractButton*>(object)) {
      button->setIcon(icon);
    } else if (auto* action = qobject_cast<QAction*>(object)) {
      action->setIcon(icon);
    }
  }
}

}  // namespace PJ
