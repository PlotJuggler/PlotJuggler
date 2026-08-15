// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "DebugUi.h"

#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QKeySequence>
#include <QLatin1String>
#include <QMainWindow>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QPoint>
#include <QRect>
#include <QSettings>
#include <QShortcut>
#include <QWidget>
#include <functional>

#include "Theme.h"
#include "pj_widgets/CurveTreeView.h"
#include "pj_widgets/FrameworkTokens.h"

namespace PJ {

namespace {

constexpr auto kQssSettingsKey = "DebugUi/qssLayerEnabled";

// Pesticide-style debug stylesheet appended over the active theme when
// the QSS layer is toggled on. Borders shift layout by 1px — accepted.
QString debugQss() {
  return QStringLiteral(R"qss(
* { border: 1px dotted %1; }

QMainWindow      { border: 1px dashed %2; }
QDialog          { border: 1px dashed %3; }
QWidget          { border: 1px dashed %4; }
QFrame           { border: 1px dashed %5; }
QSplitter        { border: 1px dashed %6; }
QSplitter::handle{ border: 1px dashed %7; }
QTabWidget       { border: 1px dashed %8; }
QTabWidget::pane { border: 1px dashed %9; }
QTabBar::tab     { border: 1px dashed %10; }

QMenuBar         { border: 1px dashed %11; }
QMenuBar::item   { border: 1px dashed %12; }
QMenu            { border: 1px dashed %13; }
QMenu::item      { border: 1px dashed %14; }
QToolTip         { border: 1px dashed %15; }

QLineEdit        { border: 1px dashed %16; }
QPlainTextEdit   { border: 1px dashed %17; }
QTextBrowser     { border: 1px dashed %18; }
QComboBox        { border: 1px dashed %19; }
QAbstractSpinBox { border: 1px dashed %20; }
QCheckBox        { border: 1px dashed %21; }
QRadioButton     { border: 1px dashed %22; }
QGroupBox        { border: 1px dashed %23; }
QLabel           { border: 1px dashed %24; }

QPushButton,
QPushButton:hover,
QPushButton:pressed,
QPushButton:checked,
QPushButton:disabled,
QPushButton:checked:hover,
QPushButton:checked:disabled { border: 1px dashed %25; }

QListView        { border: 1px dashed %26; }
QTreeView        { border: 1px dashed %27; }
QHeaderView::section { border: 1px dashed %28; }

QScrollBar:horizontal,
QScrollBar:vertical { border: 1px dashed %29; }
QScrollBar::handle:horizontal,
QScrollBar::handle:vertical { border: 1px dashed %30; }
QSlider::groove:horizontal { border: 1px dashed %31; }
QSlider::handle:horizontal { border: 1px dashed %32; }
QSlider::sub-page:horizontal { border: 1px dashed %33; }

PlotWidget       { border: 1px dashed %34; }
QwtPlot          { border: 1px dashed %35; }

TitleBar         { border: 1px dashed %36; }
TitleBar QToolButton { border: 1px dashed %37; }
)qss")
      .arg(theme::diagnostic(theme::Diagnostic::Fallback).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::MainWindow).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::Dialog).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::Widget).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::Frame).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::Splitter).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::SplitterHandle).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::TabWidget).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::TabPane).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::Tab).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::MenuBar).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::MenuBarItem).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::Menu).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::MenuItem).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::ToolTip).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::LineEdit).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::PlainTextEdit).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::TextBrowser).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::ComboBox).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::SpinBox).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::CheckBox).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::RadioButton).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::GroupBox).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::Label).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::PushButton).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::ListView).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::TreeView).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::HeaderSection).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::ScrollBar).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::ScrollHandle).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::SliderGroove).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::SliderHandle).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::SliderSubPage).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::PlotWidget).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::QwtPlot).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::TitleBar).name(QColor::HexArgb))
      .arg(theme::diagnostic(theme::Diagnostic::TitleBarButton).name(QColor::HexArgb));
}

QColor colourForDepth(int depth) {
  return QColor::fromHsl((depth * 47) % 360, 220, 130, 220);
}

// Pesticide overlay: paints over the host's subtree, mouse-transparent,
// no layout impact. Repaints on geometry / show / hide events from any
// widget so nested moves keep the dashed outlines in sync.
class PesticideOverlay : public QWidget {
 public:
  explicit PesticideOverlay(QWidget* host) : QWidget(host), host_(host) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::NoFocus);
    hide();
  }

  void setEnabled(bool on) {
    if (enabled_ == on) {
      return;
    }
    enabled_ = on;
    if (enabled_) {
      qApp->installEventFilter(this);
      resizeToHost();
      raise();
      show();
      update();
    } else {
      qApp->removeEventFilter(this);
      hide();
    }
  }

  [[nodiscard]] bool isEnabled() const {
    return enabled_;
  }

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (!enabled_) {
      return false;
    }
    switch (event->type()) {
      case QEvent::Resize:
      case QEvent::Move:
      case QEvent::Show:
      case QEvent::Hide:
      case QEvent::ChildAdded:
      case QEvent::ChildRemoved:
      case QEvent::LayoutRequest:
        if (watched == host_) {
          resizeToHost();
        }
        update();
        break;
      default:
        break;
    }
    return false;
  }

  void paintEvent(QPaintEvent* /*event*/) override {
    if (!enabled_) {
      return;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setBrush(Qt::NoBrush);

    std::function<void(QWidget*, int)> walk = [&](QWidget* widget, int depth) {
      if (widget == this || !widget->isVisible()) {
        return;
      }
      const QPoint top_left = widget->mapTo(host_, QPoint(0, 0));
      QRect rect(top_left, widget->size());
      rect.adjust(0, 0, -1, -1);
      painter.setPen(QPen(colourForDepth(depth), 1, Qt::DashLine));
      painter.drawRect(rect);
      for (QObject* child : widget->children()) {
        if (auto* child_widget = qobject_cast<QWidget*>(child)) {
          walk(child_widget, depth + 1);
        }
      }
    };
    walk(host_, 0);
  }

 private:
  void resizeToHost() {
    setGeometry(host_->rect());
    raise();
  }

  QWidget* host_;
  bool enabled_ = false;
};

}  // namespace

void DebugUi::installInto(QMainWindow* host, Theme* theme) {
  // Parented to the host so destruction is automatic.
  new DebugUi(host, theme);
}

DebugUi::DebugUi(QMainWindow* host, Theme* theme) : QObject(host), host_(host), theme_(theme) {
  overlay_ = new PesticideOverlay(host);
  qss_enabled_ = QSettings().value(kQssSettingsKey, false).toBool();

  auto* overlay_shortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D), host);
  overlay_shortcut->setContext(Qt::ApplicationShortcut);
  connect(overlay_shortcut, &QShortcut::activated, this, &DebugUi::toggleOverlay);

  auto* qss_shortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Q), host);
  qss_shortcut->setContext(Qt::ApplicationShortcut);
  connect(qss_shortcut, &QShortcut::activated, this, &DebugUi::toggleQssLayer);

  // Re-apply our debug layer whenever the base theme rewrites the global
  // stylesheet, so the overlay survives theme switches.
  if (theme_ != nullptr) {
    connect(theme_, &Theme::qssChanged, this, &DebugUi::applyQss);
  }

  // Restore persisted state.
  if (qss_enabled_) {
    applyQss();
  }
}

void DebugUi::toggleOverlay() {
  // overlay_ is a PesticideOverlay we constructed in the ctor; the type
  // is private to this translation unit so we stash it as QWidget* and
  // recover with static_cast. PesticideOverlay has no Q_OBJECT (it
  // doesn't need signals/slots) so qobject_cast wouldn't work anyway.
  auto* pesticide = static_cast<PesticideOverlay*>(overlay_);
  const bool enabled = !pesticide->isEnabled();
  pesticide->setEnabled(enabled);

  // Tree ROWS are model items, not child widgets, so the pesticide walk cannot
  // see them at all. The views draw their own row/cell/decoration outlines on
  // the same toggle, so one shortcut covers both.
  CurveTreeView::setGeometryDebugEnabled(enabled);
  for (CurveTreeView* tree : host_->findChildren<CurveTreeView*>()) {
    tree->viewport()->update();
  }
}

void DebugUi::toggleQssLayer() {
  qss_enabled_ = !qss_enabled_;
  QSettings().setValue(kQssSettingsKey, qss_enabled_);
  applyQss();
}

void DebugUi::applyQss() {
  // Theme owns the canonical stylesheet; we layer our debug rules on top
  // (or strip them off) by writing the global stylesheet directly. Order
  // of slot dispatch on qssChanged: Theme's own listener (which sets
  // qApp->styleSheet to expandedQss()) runs first because it was
  // connected during MainWindow construction; ours runs after.
  if (theme_ == nullptr) {
    return;
  }
  QString stylesheet = theme_->expandedQss();
  if (qss_enabled_) {
    stylesheet.append(QLatin1Char('\n'));
    stylesheet.append(debugQss());
  }
  qApp->setStyleSheet(stylesheet);
}

}  // namespace PJ
