// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "ToolboxChrome.h"

#include <QAbstractButton>
#include <QBoxLayout>
#include <QPointer>
#include <QSplitter>
#include <QWidget>

#include "pj_widgets/SvgButton.h"

using namespace Qt::StringLiterals;

namespace PJ {

namespace {

// A button tagged pjToolboxChromeAction is hidden in the content and stood in
// for by a proxy in the banner / floating title bar: chromeActionIcon is its
// SVG resource path, chromeActionSlot = "leading" sits it before the title.
constexpr const char* kToolboxChromeActionProperty = "pjToolboxChromeAction";
constexpr const char* kToolboxChromeActionIconProperty = "chromeActionIcon";
constexpr const char* kToolboxChromeActionSlotProperty = "chromeActionSlot";

}  // namespace

QSplitter* toolboxDrawerSplitter(QWidget* container) {
  auto* outer = qobject_cast<QBoxLayout*>(container->layout());
  if (outer == nullptr || outer->count() == 0) {
    return nullptr;
  }
  return qobject_cast<QSplitter*>(outer->itemAt(0)->widget());
}

QString toolboxDrawerWidthSettingsKey(const QString& persist_key) {
  return persist_key.isEmpty() ? QString() : u"ToolboxDrawerWidth/%1"_s.arg(persist_key);
}

SvgButton* makeChromeButton(const QString& icon_path, const QString& tooltip, QWidget* parent) {
  auto* button = new SvgButton(icon_path, SvgButton::Size::kDefault, parent);
  button->setToolTip(tooltip);
  button->setCursor(Qt::PointingHandCursor);
  return button;
}

SvgButton* makeChromeActionProxy(QAbstractButton* src, QWidget* parent) {
  QString icon_path = src->property(kToolboxChromeActionIconProperty).toString();
  if (icon_path.isEmpty()) {
    icon_path = u":/resources/svg/help.svg"_s;
  }
  SvgButton* proxy = makeChromeButton(icon_path, src->toolTip(), parent);
  QObject::connect(proxy, &QAbstractButton::clicked, proxy, [src = QPointer<QAbstractButton>(src)]() {
    if (!src.isNull()) {
      src->click();
    }
  });
  return proxy;
}

std::vector<QAbstractButton*> taggedChromeActions(QWidget* content) {
  std::vector<QAbstractButton*> tagged;
  for (auto* src : content->findChildren<QAbstractButton*>()) {
    if (src != nullptr && src->property(kToolboxChromeActionProperty).toBool()) {
      tagged.push_back(src);
    }
  }
  return tagged;
}

bool isLeadingChromeAction(const QAbstractButton* src) {
  return src->property(kToolboxChromeActionSlotProperty).toString() == u"leading"_s;
}

}  // namespace PJ
