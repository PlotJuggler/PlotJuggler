// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "mcp/McpUiTools.h"

#include <QtGui/private/qguiapplication_p.h>
#include <QtGui/private/qhighdpiscaling_p.h>
#include <qpa/qwindowsysteminterface.h>
#include <qwt_plot_curve.h>
#include <qwt_text.h>

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDockWidget>
#include <QGroupBox>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QStyleHints>
#include <QTabBar>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextEdit>
#include <QTimer>
#include <QTreeView>
#include <QWheelEvent>
#include <QWindow>
#include <algorithm>
#include <charconv>
#include <optional>
#include <stdexcept>
#include <unordered_map>

#include "mcp/McpProtocol.h"
#include "pj_plotting/DockWidget.h"
#include "pj_plotting/PlotWidget.h"

using json = nlohmann::json;
using namespace Qt::StringLiterals;

namespace PJ::mcp {

namespace {

/// Caps on one ui_tree answer, which an agent reads in full.
constexpr int kMaxLines = 600;
constexpr int kMaxRowsPerView = 40;
constexpr qsizetype kMaxTextChars = 80;
/// Mouse moves a drag takes between its press and release.
constexpr int kDragSteps = 8;
/// Bounds on the numbers an agent passes: offsets inside a widget, wheel notches.
constexpr double kMaxOffset = 100'000.0;
constexpr int kMaxNotches = 100;

/// A refusal worded for the agent; call() turns it into a tool error.
struct UiError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

std::string clip(QString text) {
  if (Qt::mightBeRichText(text)) {
    text = QTextDocumentFragment::fromHtml(text).toPlainText();
  }
  text.truncate(4 * kMaxTextChars);  // simplified() may shrink it, but never copy a whole document
  text = text.simplified();
  if (text.size() > kMaxTextChars) {
    text = text.left(kMaxTextChars - 1) + u'…';
  }
  return text.toStdString();
}

/// A widget whose children are its own internals (a spin box's line edit, a combo's view), or
/// listed as items: not descended into.
bool leaf(const QWidget* w) {
  return qobject_cast<const QAbstractButton*>(w) || qobject_cast<const QAbstractSpinBox*>(w) ||
         qobject_cast<const QComboBox*>(w) || qobject_cast<const QLineEdit*>(w) ||
         qobject_cast<const QAbstractSlider*>(w) || qobject_cast<const QTabBar*>(w) || qobject_cast<const QMenu*>(w) ||
         qobject_cast<const QMenuBar*>(w) || qobject_cast<const QTextEdit*>(w) ||
         qobject_cast<const QPlainTextEdit*>(w);
}

/// A widget that users act on: always listed, even unnamed. Item views are also descended into.
bool interactive(const QWidget* w) {
  return leaf(w) || qobject_cast<const QAbstractItemView*>(w);
}

/// The start of `doc`, enough for a line of ui_tree, without copying a long document.
QString leadingText(const QTextDocument* doc) {
  QString text;
  for (QTextBlock block = doc->begin(); block.isValid() && text.size() < kMaxTextChars; block = block.next()) {
    text += block.text() + u' ';
  }
  return text;
}

/// The objectName, unless it is one Qt gives its own internals.
QString ownName(const QWidget* w) {
  const QString name = w->objectName();
  return name.startsWith(u"qt_"_s) ? QString() : name;
}

/// What the user reads on `w`.
QString shownText(QWidget* w) {
  if (w->isWindow() || qobject_cast<QDockWidget*>(w) != nullptr) {
    return w->windowTitle().remove(u"[*]"_s);
  }
  if (auto* button = qobject_cast<QAbstractButton*>(w)) {
    // Without mnemonic markers, which some platform themes add ("&OK"): a script reads the same everywhere.
    return button->text().isEmpty() ? button->toolTip() : QString(button->text()).remove(u'&');
  }
  if (auto* label = qobject_cast<QLabel*>(w)) {
    return label->text();
  }
  if (auto* edit = qobject_cast<QLineEdit*>(w)) {
    return edit->text();
  }
  if (auto* spin = qobject_cast<QAbstractSpinBox*>(w)) {
    return spin->text();
  }
  if (auto* combo = qobject_cast<QComboBox*>(w)) {
    return combo->currentText();
  }
  if (auto* text = qobject_cast<QPlainTextEdit*>(w)) {
    return leadingText(text->document());
  }
  if (auto* text = qobject_cast<QTextEdit*>(w)) {
    return leadingText(text->document());
  }
  if (auto* slider = qobject_cast<QAbstractSlider*>(w)) {
    return QString::number(slider->value());
  }
  if (auto* group = qobject_cast<QGroupBox*>(w)) {
    return group->title();
  }
  return {};
}

/// `Class#name "text"`, how ui_tree and the action replies name a widget; `text` is its
/// clip(shownText(w)) when the caller already has it.
std::string describe(QWidget* w, const std::optional<std::string>& text = std::nullopt) {
  std::string out = w->metaObject()->className();
  if (const QString name = ownName(w); !name.isEmpty()) {
    out += "#" + name.toStdString();
  }
  if (const std::string shown = text ? *text : clip(shownText(w)); !shown.empty()) {
    out += " \"" + shown + "\"";
  }
  return out;
}

const char* checkFlag(bool checked) {
  return checked ? "checked" : "unchecked";
}

/// The geometry of `action` in `w`, a menu or a menu bar; empty when it is not shown.
QRect actionRect(QWidget* w, QAction* action) {
  if (auto* menu = qobject_cast<QMenu*>(w)) {
    return menu->actionGeometry(action);
  }
  if (auto* bar = qobject_cast<QMenuBar*>(w)) {
    return bar->actionGeometry(action);
  }
  return {};
}

/// " (a, b)" or "".
std::string flagList(const std::vector<std::string>& flags) {
  if (flags.empty()) {
    return {};
  }
  std::string out = " (";
  for (std::size_t i = 0; i < flags.size(); ++i) {
    out += (i == 0 ? "" : ", ") + flags[i];
  }
  return out + ")";
}

/// A menu action's text without its mnemonic markers.
QString actionText(const QAction* action) {
  return action->text().remove(u'&');
}

Qt::KeyboardModifiers parseModifiers(const std::string& text) {
  Qt::KeyboardModifiers mods;
  for (const QString& part : QString::fromStdString(text).toLower().split(u'+', Qt::SkipEmptyParts)) {
    const QString p = part.trimmed();
    if (p == u"ctrl"_s || p == u"control"_s) {
      mods |= Qt::ControlModifier;
    } else if (p == u"shift"_s) {
      mods |= Qt::ShiftModifier;
    } else if (p == u"alt"_s) {
      mods |= Qt::AltModifier;
    } else if (p == u"meta"_s || p == u"cmd"_s) {
      mods |= Qt::MetaModifier;
    } else {
      throw UiError("unknown modifier '" + p.toStdString() + "' (use Ctrl, Shift, Alt, Meta)");
    }
  }
  return mods;
}

Qt::MouseButton parseButton(const std::string& name) {
  if (name.empty() || name == "left") {
    return Qt::LeftButton;
  }
  if (name == "right") {
    return Qt::RightButton;
  }
  if (name == "middle") {
    return Qt::MiddleButton;
  }
  throw UiError("button must be left, right or middle");
}

/// A point of `host` as the window system reports it: the top-level's QWindow, the point in its
/// coordinates and on screen.
struct WindowPoint {
  QWidget* top = nullptr;
  QWindow* window = nullptr;
  QPointF local;
  QPointF global;
};

WindowPoint windowPoint(QWidget* host, QPoint p) {
  QWidget* top = host->window();
  if (top->windowHandle() == nullptr) {
    throw UiError("its window is not shown");
  }
  return {top, top->windowHandle(), QPointF(host->mapTo(top, p)), QPointF(host->mapToGlobal(p))};
}

/// The widget a press at `point` lands on. While a popup is open, a press elsewhere goes to it.
std::string hitAt(const WindowPoint& point) {
  if (QWidget* popup = QApplication::activePopupWidget(); popup != nullptr && popup != point.top) {
    return "the open popup " + describe(popup) + ", which closes";
  }
  QWidget* hit = point.top->childAt(point.local.toPoint());
  return describe(hit != nullptr ? hit : point.top);
}

/// Whether a modal dialog blocks input to `top`, as Qt decides it (window-modal dialogs block only
/// their parent's window).
bool blockedByModal(QWidget* top) {
  QWindow* window = top->windowHandle();
  return window != nullptr && QGuiApplicationPrivate::instance()->isWindowBlocked(window);
}

/// Queues one mouse event. The window system speaks native pixels, so the points are scaled to
/// them, as a platform plugin would report them.
void mouse(
    ulong timestamp, const WindowPoint& at, Qt::MouseButtons state, Qt::MouseButton button, QEvent::Type type,
    Qt::KeyboardModifiers mods) {
  // Queued, not synchronous: a press that opens a modal dialog must not block the call inside its exec().
  QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::AsynchronousDelivery>(
      at.window, timestamp, QHighDpi::toNativeLocalPosition(at.local, at.window),
      QHighDpi::toNativeGlobalPosition(at.global, at.window), state, button, type, mods);
}

void keyPress(QWindow* window, int key, Qt::KeyboardModifiers mods, const QString& text) {
  QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::AsynchronousDelivery>(
      window, QEvent::KeyPress, key, mods, text);
  QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::AsynchronousDelivery>(
      window, QEvent::KeyRelease, key, mods, text);
}

/// The visible top-levels, the open popup first and then the user's modal dialog.
std::vector<QWidget*> visibleTopLevels() {
  std::vector<QWidget*> tops;
  for (QWidget* w : {QApplication::activePopupWidget(), QApplication::activeModalWidget()}) {
    if (w != nullptr && std::find(tops.begin(), tops.end(), w) == tops.end()) {
      tops.push_back(w);
    }
  }
  for (QWidget* w : QApplication::topLevelWidgets()) {
    if (w->isVisible() && std::find(tops.begin(), tops.end(), w) == tops.end()) {
      tops.push_back(w);
    }
  }
  return tops;
}

}  // namespace

json UiDriver::call(const std::string& tool, const json& args) {
  static const std::unordered_map<std::string, json (UiDriver::*)(const json&)> tools = {
      {"ui_tree", &UiDriver::tree},   {"ui_click", &UiDriver::click}, {"ui_drag", &UiDriver::drag},
      {"ui_wheel", &UiDriver::wheel}, {"ui_key", &UiDriver::key},     {"ui_type", &UiDriver::type},
      {"ui_plot", &UiDriver::plot}};
  const auto found = tools.find(tool);
  if (found == tools.end()) {
    return textResult("unknown tool " + tool, true);
  }
  try {
    return (this->*found->second)(args);
  } catch (const UiError& e) {
    return textResult(e.what(), true);
  }
}

std::string UiDriver::addRef(Target target) {
  refs_.push_back(std::move(target));
  return "w" + std::to_string(ref_base_ + refs_.size());
}

json UiDriver::tree(const json& /*args*/) {
  // Refs are the latest tree's: earlier ones read as stale, and the row refs (persistent indexes the
  // model keeps up to date) of earlier trees are released.
  ref_base_ += refs_.size();
  refs_.clear();
  std::string out;
  int lines = 0;
  // A modal dialog blocks input to the windows behind it: those get one line each, not their tree.
  for (QWidget* top : visibleTopLevels()) {
    if (top != QApplication::activePopupWidget() && blockedByModal(top)) {
      out += "- [" + addRef({.kind = Target::Kind::kWidget, .widget = top}) + "] " + describe(top) +
             " (behind the modal dialog)\n";
      ++lines;
      continue;
    }
    dumpWidget(top, 0, out, lines);
  }
  if (lines >= kMaxLines) {
    out += "... cut at " + std::to_string(kMaxLines) + " lines\n";
  }
  return textResult(out.empty() ? "no visible window" : out);
}

void UiDriver::dumpWidget(QWidget* w, int depth, std::string& out, int& lines) {
  if (lines >= kMaxLines || !w->isVisible() || w->width() <= 0 || w->height() <= 0 ||
      qobject_cast<QScrollBar*>(w) != nullptr || qobject_cast<QHeaderView*>(w) != nullptr) {
    return;
  }
  // Unnamed, textless containers are left out and their children listed in their place; a class
  // of PlotJuggler's own (no Q prefix) is kept, as it is usually what the user looks at.
  const std::string text = clip(shownText(w));
  const bool listed = w->isWindow() || interactive(w) || !ownName(w).isEmpty() || !text.empty() ||
                      !QByteArray(w->metaObject()->className()).startsWith('Q');
  int child_depth = depth;
  if (listed) {
    std::vector<std::string> flags;
    if (!w->isEnabled()) {
      flags.emplace_back("disabled");
    }
    if (w->property("checkable").toBool()) {  // buttons and group boxes
      flags.emplace_back(checkFlag(w->property("checked").toBool()));
    }
    if (w->property("readOnly").toBool()) {  // line, text and spin box edits: typing does nothing
      flags.emplace_back("read-only");
    }
    if (auto* edit = qobject_cast<QLineEdit*>(w);
        edit != nullptr && edit->text().isEmpty() && !edit->placeholderText().isEmpty()) {
      flags.push_back("placeholder \"" + clip(edit->placeholderText()) + "\"");
    }
    if (w->hasFocus()) {
      flags.emplace_back("focus");
    }
    if (w == QApplication::activeModalWidget()) {
      flags.emplace_back("modal");
    }
    if (w == QApplication::activePopupWidget()) {
      flags.emplace_back("popup");
    }
    out += std::string(2 * depth, ' ') + "- [" + addRef({.kind = Target::Kind::kWidget, .widget = w}) + "] " +
           describe(w, text) + flagList(flags) + " " + std::to_string(w->width()) + "x" + std::to_string(w->height()) +
           "\n";
    ++lines;
    child_depth = depth + 1;
    dumpItems(w, child_depth, out, lines);
  }
  if (leaf(w)) {
    return;
  }
  for (QObject* child : w->children()) {
    if (auto* widget = qobject_cast<QWidget*>(child); widget != nullptr && !widget->isWindow()) {
      dumpWidget(widget, child_depth, out, lines);
    }
  }
}

void UiDriver::dumpItems(QWidget* w, int depth, std::string& out, int& lines) {
  const std::string indent(2 * depth, ' ');
  const auto item = [&](Target target, const std::string& what, const std::vector<std::string>& flags,
                        const std::string& suffix = {}, int level = 0) {
    if (lines >= kMaxLines) {
      return;
    }
    out += indent + std::string(2 * level, ' ') + "- [" + addRef(std::move(target)) + "] " + what + flagList(flags) +
           suffix + "\n";
    ++lines;
  };

  if (qobject_cast<QMenu*>(w) != nullptr || qobject_cast<QMenuBar*>(w) != nullptr) {
    for (QAction* action : w->actions()) {
      if (action->isSeparator() || !action->isVisible() || actionRect(w, action).isEmpty()) {
        continue;
      }
      std::vector<std::string> flags;
      if (!action->isEnabled()) {
        flags.emplace_back("disabled");
      }
      if (action->isCheckable()) {
        flags.emplace_back(checkFlag(action->isChecked()));
      }
      std::string suffix = action->menu() != nullptr ? " >" : "";
      if (!action->shortcut().isEmpty()) {
        suffix += " " + action->shortcut().toString(QKeySequence::PortableText).toStdString();
      }
      item(
          {.kind = Target::Kind::kAction, .widget = w, .action = action}, "item \"" + clip(actionText(action)) + "\"",
          flags, suffix);
    }
    return;
  }

  if (auto* tabs = qobject_cast<QTabBar*>(w)) {
    for (int i = 0; i < tabs->count(); ++i) {
      if (!tabs->isTabVisible(i)) {
        continue;
      }
      std::vector<std::string> flags;
      if (i == tabs->currentIndex()) {
        flags.emplace_back("current");
      }
      if (!tabs->isTabEnabled(i)) {
        flags.emplace_back("disabled");
      }
      item(
          {.kind = Target::Kind::kTab, .widget = w, .tab = i, .text = tabs->tabText(i)},
          "tab \"" + clip(tabs->tabText(i)) + "\"", flags);
    }
    return;
  }

  auto* view = qobject_cast<QAbstractItemView*>(w);
  if (view == nullptr || view->model() == nullptr) {
    return;
  }
  const QAbstractItemModel* model = view->model();
  auto* tree = qobject_cast<QTreeView*>(view);
  const int bottom = view->viewport()->height();
  // The row at the top of the viewport, else the first one; rows above the viewport and hidden rows
  // (an empty rect) are skipped.
  QModelIndex row = view->indexAt(QPoint(1, 1)).siblingAtColumn(0);
  if (!row.isValid()) {
    row = model->index(0, 0, view->rootIndex());
  }
  int shown = 0;
  for (; row.isValid() && view->visualRect(row).top() < bottom;
       row = tree != nullptr ? tree->indexBelow(row) : row.sibling(row.row() + 1, 0)) {
    if (const QRect rect = view->visualRect(row); rect.isEmpty() || rect.bottom() < 0) {
      continue;
    }
    if (shown == kMaxRowsPerView || lines >= kMaxLines) {
      out += indent + "- ... more rows below (ui_wheel scrolls)\n";
      ++lines;
      return;
    }
    QString text;
    const int columns = std::min(model->columnCount(row.parent()), 4);
    for (int column = 0; column < columns; ++column) {
      const QString cell = row.siblingAtColumn(column).data(Qt::DisplayRole).toString();
      if (!cell.isEmpty()) {
        text += (text.isEmpty() ? u""_s : u" | "_s) + cell;
      }
    }
    int level = 0;
    for (QModelIndex up = row.parent(); up.isValid() && up != view->rootIndex(); up = up.parent()) {
      ++level;
    }
    std::vector<std::string> flags;
    if (view->selectionModel() != nullptr && view->selectionModel()->isSelected(row)) {
      flags.emplace_back("selected");
    }
    if (const QVariant check = row.data(Qt::CheckStateRole); check.isValid()) {
      flags.emplace_back(checkFlag(check.value<Qt::CheckState>() != Qt::Unchecked));
    }
    if (tree != nullptr && model->hasChildren(row)) {
      flags.emplace_back(tree->isExpanded(row) ? "expanded" : "collapsed");
    }
    item(
        {.kind = Target::Kind::kRow, .widget = w, .index = QPersistentModelIndex(row)}, "row \"" + clip(text) + "\"",
        flags, {}, level);
    ++shown;
  }
}

std::pair<QWidget*, QRect> UiDriver::locate(const Target& target) {
  QWidget* w = target.widget;
  if (w == nullptr) {
    throw UiError("that widget is gone; call ui_tree again");
  }
  if (!w->isVisible()) {
    throw UiError("that widget is hidden; call ui_tree again");
  }
  if (blockedByModal(w->window())) {
    throw UiError("that window is behind a modal dialog: close the dialog first");
  }
  switch (target.kind) {
    case Target::Kind::kWidget:
      return {w, w->rect()};
    case Target::Kind::kAction: {
      if (target.action == nullptr) {
        throw UiError("that menu item is gone; call ui_tree again");
      }
      const QRect rect = actionRect(w, target.action);
      if (rect.isEmpty()) {
        throw UiError("that menu item is not shown; call ui_tree again");
      }
      return {w, rect};
    }
    case Target::Kind::kTab: {
      auto* tabs = qobject_cast<QTabBar*>(w);
      if (target.tab >= tabs->count() || tabs->tabText(target.tab) != target.text || !tabs->isTabVisible(target.tab)) {
        throw UiError("that tab moved or is gone; call ui_tree again");
      }
      return {w, tabs->tabRect(target.tab)};
    }
    case Target::Kind::kRow: {
      auto* view = qobject_cast<QAbstractItemView*>(w);
      if (!target.index.isValid()) {
        throw UiError("that row is gone; call ui_tree again");
      }
      const QRect rect = view->visualRect(target.index);
      if (!rect.intersects(view->viewport()->rect())) {
        throw UiError("that row is scrolled out of view; call ui_tree again");
      }
      return {view->viewport(), rect.intersected(view->viewport()->rect())};
    }
  }
  throw UiError("unknown target");
}

UiDriver::Target UiDriver::resolve(const json& args) const {
  if (const std::string ref = args.value("ref", std::string()); !ref.empty()) {
    const std::string digits = ref.starts_with('w') ? ref.substr(1) : ref;
    std::size_t index = 0;
    const char* end = digits.data() + digits.size();
    if (const auto parsed = std::from_chars(digits.data(), end, index); parsed.ec != std::errc() || parsed.ptr != end) {
      throw UiError("ref '" + ref + "' is not one ui_tree gave");
    }
    if (index <= ref_base_ || index > ref_base_ + refs_.size()) {
      throw UiError("ref '" + ref + "' is unknown or stale; call ui_tree again");
    }
    return refs_[index - ref_base_ - 1];
  }
  if (const std::string name = args.value("name", std::string()); !name.empty()) {
    const QString object_name = QString::fromStdString(name);
    std::vector<QWidget*> found;
    for (QWidget* top : visibleTopLevels()) {
      if (blockedByModal(top)) {
        continue;  // ui_tree does not list them either, and they could not be acted on
      }
      if (top->objectName() == object_name) {
        found.push_back(top);
      }
      // A popup or child dialog is also a QObject child of its owner: count each widget under its own
      // window only, as ui_tree does, so it is found once and its window's modal block applies.
      for (QWidget* w : top->findChildren<QWidget*>(object_name)) {
        if (w->isVisible() && w->window() == top) {
          found.push_back(w);
        }
      }
    }
    if (found.empty()) {
      throw UiError("no visible widget is named '" + name + "'");
    }
    // Which of several a search finds first depends on the run: a repro must not.
    if (found.size() > 1) {
      throw UiError(std::to_string(found.size()) + " visible widgets are named '" + name + "'; use a ref from ui_tree");
    }
    return {.kind = Target::Kind::kWidget, .widget = found.front()};
  }
  throw UiError("give a ref from ui_tree or a widget name");
}

namespace {

QPoint offsetIn(const QRect& rect, double x, double y) {
  return rect.topLeft() +
         QPoint(qRound(std::clamp(x, -kMaxOffset, kMaxOffset)), qRound(std::clamp(y, -kMaxOffset, kMaxOffset)));
}

/// The point `args` ask for (`x`,`y` inside `rect`), else the center of `rect`.
QPoint pointIn(const QRect& rect, const json& args) {
  if (args.contains("x") || args.contains("y")) {
    return offsetIn(rect, args.value("x", 0.0), args.value("y", 0.0));
  }
  return rect.center();
}

std::string at(const WindowPoint& point) {
  return std::to_string(qRound(point.local.x())) + "," + std::to_string(qRound(point.local.y()));
}

/// One key to press: its code, the modifiers held and the text it types.
struct KeyStroke {
  int key = 0;
  Qt::KeyboardModifiers mods;
  QString text;
};

/// The top-level keys go to when no target is named: the open popup, the user's modal dialog, the
/// focused widget's window, else the main window. Changes nothing.
QWidget* defaultKeyWindow() {
  if (QWidget* popup = QApplication::activePopupWidget()) {
    return popup;
  }
  if (QWidget* modal = QApplication::activeModalWidget()) {
    return modal;
  }
  if (QWidget* focus = QApplication::focusWidget()) {
    return focus->window();
  }
  for (QWidget* w : QApplication::topLevelWidgets()) {
    if (qobject_cast<QMainWindow*>(w) != nullptr && w->isVisible()) {
      return w;
    }
  }
  throw UiError("no window to type into");
}

/// Presses `strokes` into `top`'s window from the event loop, after focusing `target` (when one was
/// named) and activating the window, so that its shortcuts fire. Deferred, because focusing and
/// activating deliver events at once: a focus-out handler that opens a modal dialog would otherwise
/// hold the call. Nothing happens if they are gone by then.
void sendKeys(QWidget* target, QWidget* top, std::vector<KeyStroke> strokes) {
  QTimer::singleShot(
      0, top, [target = QPointer<QWidget>(target), named = target != nullptr, top, strokes = std::move(strokes)] {
        if ((named && target == nullptr) || top->windowHandle() == nullptr) {
          return;
        }
        if (target != nullptr) {
          target->setFocus(Qt::OtherFocusReason);
        }
        QWindow* window = top->windowHandle();
        if (QApplication::activePopupWidget() == nullptr && QGuiApplication::focusWindow() != window) {
          top->activateWindow();
          // Activation is asynchronous on xcb: tell Qt now, or the first keys arrive before the window is
          // active and its shortcuts are dropped.
          QWindowSystemInterface::handleFocusWindowChanged<QWindowSystemInterface::AsynchronousDelivery>(
              window, Qt::ActiveWindowFocusReason);
        }
        for (const KeyStroke& stroke : strokes) {
          keyPress(window, stroke.key, stroke.mods, stroke.text);
        }
      });
}

/// What keys go into: the named target, else the focused widget of the window they go to.
std::string keysInto(QWidget* target, QWidget* top) {
  if (target != nullptr) {
    return describe(target);
  }
  return describe(top->focusWidget() != nullptr ? top->focusWidget() : top);
}

}  // namespace

ulong UiDriver::nextStamp() {
  if (!clock_.isValid()) {
    clock_.start();
  }
  const auto gap = static_cast<ulong>(QGuiApplication::styleHints()->mouseDoubleClickInterval()) + 1;
  last_stamp_ = std::max(static_cast<ulong>(clock_.elapsed()), last_stamp_ + gap);
  return last_stamp_;
}

json UiDriver::click(const json& args) {
  const auto [host, rect] = locate(resolve(args));
  const WindowPoint point = windowPoint(host, pointIn(rect, args));
  const Qt::MouseButton button = parseButton(args.value("button", std::string()));
  const Qt::KeyboardModifiers mods = parseModifiers(args.value("modifiers", std::string()));
  const int clicks = args.value("double", false) ? 2 : 1;
  const std::string hit = hitAt(point);
  const ulong stamp = nextStamp();
  mouse(stamp, point, Qt::NoButton, Qt::NoButton, QEvent::MouseMove, mods);
  for (int i = 0; i < clicks; ++i) {
    mouse(stamp + i, point, button, button, QEvent::MouseButtonPress, mods);
    mouse(stamp + i, point, Qt::NoButton, button, QEvent::MouseButtonRelease, mods);
  }
  return textResult("clicked at " + at(point) + " of its window; the press landed on " + hit);
}

json UiDriver::drag(const json& args) {
  const auto [host, rect] = locate(resolve(args));
  const auto read_point = [&](const char* key, QPoint fallback) {
    if (!args.contains(key)) {
      return fallback;
    }
    const json& p = args.at(key);
    return offsetIn(rect, p.at(0).get<double>(), p.at(1).get<double>());
  };
  const WindowPoint from = windowPoint(host, read_point("from", rect.center()));
  QPointF to_global;
  if (const std::string to_ref = args.value("to_ref", std::string()); !to_ref.empty()) {
    const auto [to_host, to_rect] = locate(resolve({{"ref", to_ref}}));
    to_global = QPointF(to_host->mapToGlobal(to_rect.center()));
  } else if (args.contains("to")) {
    to_global = QPointF(host->mapToGlobal(read_point("to", rect.center())));
  } else {
    throw UiError("give `to` or `to_ref`");
  }
  const Qt::MouseButton button = parseButton(args.value("button", std::string()));
  const Qt::KeyboardModifiers mods = parseModifiers(args.value("modifiers", std::string()));
  const std::string hit = hitAt(from);
  // Every event goes to the window of the press, as Qt's implicit grab would deliver them.
  const auto along = [&](double t) {
    const QPointF global = from.global + (to_global - from.global) * t;
    return WindowPoint{from.top, from.window, from.window->mapFromGlobal(global), global};
  };
  const ulong stamp = nextStamp();
  mouse(stamp, from, Qt::NoButton, Qt::NoButton, QEvent::MouseMove, mods);
  mouse(stamp, from, button, button, QEvent::MouseButtonPress, mods);
  for (int step = 1; step <= kDragSteps; ++step) {
    mouse(stamp + step, along(double(step) / kDragSteps), button, Qt::NoButton, QEvent::MouseMove, mods);
  }
  mouse(stamp + kDragSteps, along(1.0), Qt::NoButton, button, QEvent::MouseButtonRelease, mods);
  return textResult(
      "dragged from " + at(from) + " to " + at(along(1.0)) + " of its window; the press landed on " + hit);
}

json UiDriver::wheel(const json& args) {
  const auto [host, rect] = locate(resolve(args));
  const WindowPoint point = windowPoint(host, pointIn(rect, args));
  const int notches =
      static_cast<int>(std::clamp(args.value("notches", 1.0), double(-kMaxNotches), double(kMaxNotches)));
  const Qt::KeyboardModifiers mods = parseModifiers(args.value("modifiers", std::string()));
  // handleWheelEvent has no queued form: deferred instead, like the rest of the input.
  QTimer::singleShot(
      0, point.window, [window = point.window, local = point.local, global = point.global, notches, mods] {
        QWindowSystemInterface::handleWheelEvent(
            window, QHighDpi::toNativeLocalPosition(local, window), QHighDpi::toNativeGlobalPosition(global, window),
            QPoint(), QPoint(0, notches * QWheelEvent::DefaultDeltasPerStep), mods);
      });
  return textResult(
      "turned the wheel " + std::to_string(notches) + " notches at " + at(point) + " over " + hitAt(point));
}

QWidget* UiDriver::keyTarget(const json& args) const {
  if (args.value("ref", std::string()).empty() && args.value("name", std::string()).empty()) {
    return nullptr;
  }
  const Target target = resolve(args);
  static_cast<void>(locate(target));  // refuses a gone, hidden or blocked one
  return target.widget;
}

json UiDriver::key(const json& args) {
  QWidget* target = keyTarget(args);
  QWidget* top = target != nullptr ? target->window() : defaultKeyWindow();
  const QString keys = QString::fromStdString(args.at("keys").get<std::string>());
  const QKeySequence sequence = QKeySequence::fromString(keys, QKeySequence::PortableText);
  std::vector<KeyStroke> strokes;
  for (int i = 0; i < sequence.count(); ++i) {
    const QKeyCombination combo = sequence[i];
    const int code = combo.key();
    if (code == Qt::Key_unknown) {
      strokes.clear();
      break;
    }
    QString text;
    if (code >= Qt::Key_Space && code <= Qt::Key_AsciiTilde &&
        !(combo.keyboardModifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
      const QChar ch(code);
      text = combo.keyboardModifiers() & Qt::ShiftModifier ? QString(ch) : QString(ch.toLower());
    }
    strokes.push_back({code, combo.keyboardModifiers(), text});
  }
  if (strokes.empty()) {
    throw UiError("could not read the key sequence '" + keys.toStdString() + "'");
  }
  const std::string into = keysInto(target, top);
  sendKeys(target, top, std::move(strokes));
  return textResult("pressed " + sequence.toString(QKeySequence::PortableText).toStdString() + " into " + into);
}

json UiDriver::type(const json& args) {
  QWidget* target = keyTarget(args);
  QWidget* top = target != nullptr ? target->window() : defaultKeyWindow();
  const QString text = QString::fromStdString(args.at("text").get<std::string>());
  if (text.size() > kMaxUiTypedChars) {
    throw UiError("type at most " + std::to_string(kMaxUiTypedChars) + " characters per call");
  }
  std::vector<KeyStroke> strokes;
  for (const QChar ch : text) {
    if (ch == u'\n') {
      strokes.push_back({Qt::Key_Return, Qt::NoModifier, u"\r"_s});
    } else {
      strokes.push_back({ch.isLetter() ? ch.toUpper().unicode() : ch.unicode(), Qt::NoModifier, QString(ch)});
    }
  }
  const std::string into = keysInto(target, top);
  sendKeys(target, top, std::move(strokes));
  return textResult("typed " + std::to_string(text.size()) + " characters into " + into);
}

json UiDriver::plot(const json& args) {
  const std::string wanted = args.value("title", std::string());
  // Fixed decimals, so a script can expect the exact text of a range or a value.
  const auto fixed = [](double value) { return QString::number(value, 'f', 3).toStdString(); };
  json plots = json::array();
  for (QWidget* top : visibleTopLevels()) {
    for (DockWidget* dock : top->findChildren<DockWidget*>()) {
      PlotWidget* plot = dock->plotWidget();
      const std::string title = dock->name().toStdString();
      if (plot == nullptr || !plot->isVisible() || (!wanted.empty() && title != wanted)) {
        continue;
      }
      const QRectF view = plot->currentBoundingRect();
      json curves = json::array();
      for (const PlotWidgetBase::CurveInfo& info : plot->curveList()) {
        if (info.curve != nullptr) {
          curves.push_back({{"title", info.curve->title().text().toStdString()}, {"visible", info.curve->isVisible()}});
        }
      }
      // The value box as the user reads it: one row per line, the &nbsp; padding collapsed.
      json tracker = json::array();
      if (plot->trackerValueBoxVisible()) {
        const QString text = QTextDocumentFragment::fromHtml(plot->trackerValueBoxText()).toPlainText();
        for (const QString& row : text.split(u'\n', Qt::SkipEmptyParts)) {
          tracker.push_back(row.simplified().toStdString());
        }
      }
      plots.push_back(
          {{"title", title},
           {"x", {fixed(std::min(view.left(), view.right())), fixed(std::max(view.left(), view.right()))}},
           {"y", {fixed(std::min(view.top(), view.bottom())), fixed(std::max(view.top(), view.bottom()))}},
           {"curves", curves},
           {"tracker", tracker}});
    }
  }
  if (plots.empty()) {
    return textResult(wanted.empty() ? "no visible plot" : "no visible plot titled " + wanted, true);
  }
  return jsonResult(plots);
}

}  // namespace PJ::mcp
