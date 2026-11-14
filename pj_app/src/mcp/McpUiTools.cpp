// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "mcp/McpUiTools.h"

#include <qpa/qwindowsysteminterface.h>

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
#include <QTabBar>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextEdit>
#include <QTreeView>
#include <QWheelEvent>
#include <QWindow>
#include <algorithm>
#include <stdexcept>

#include "mcp/McpProtocol.h"

using json = nlohmann::json;
using namespace Qt::StringLiterals;

namespace PJ::mcp {

namespace {

bool g_ui_tools_enabled = false;

/// Caps on one ui_tree answer, which an agent reads in full.
constexpr int kMaxLines = 600;
constexpr int kMaxRowsPerView = 40;
constexpr qsizetype kMaxTextChars = 80;
/// The ref table is cleared past this many (the refs of earlier trees then read as stale).
constexpr std::size_t kMaxRefs = 5000;
constexpr int kDefaultSettleMs = 150;
constexpr int kMaxSettleMs = 5000;
/// Mouse moves a drag takes between its press and release.
constexpr int kDragSteps = 8;

/// A refusal worded for the agent; call() turns it into a tool error.
struct UiError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

std::string clip(QString text) {
  if (Qt::mightBeRichText(text)) {
    text = QTextDocumentFragment::fromHtml(text).toPlainText();
  }
  text = text.simplified();
  if (text.size() > kMaxTextChars) {
    text = text.left(kMaxTextChars - 1) + u'…';
  }
  return text.toStdString();
}

/// A widget that users act on: always listed, even unnamed.
bool interactive(const QWidget* w) {
  return qobject_cast<const QAbstractButton*>(w) || qobject_cast<const QAbstractSlider*>(w) ||
         qobject_cast<const QAbstractSpinBox*>(w) || qobject_cast<const QComboBox*>(w) ||
         qobject_cast<const QLineEdit*>(w) || qobject_cast<const QTextEdit*>(w) ||
         qobject_cast<const QPlainTextEdit*>(w) || qobject_cast<const QAbstractItemView*>(w) ||
         qobject_cast<const QTabBar*>(w) || qobject_cast<const QMenu*>(w) || qobject_cast<const QMenuBar*>(w);
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
    return button->text().isEmpty() ? button->toolTip() : button->text();
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
    return text->toPlainText();
  }
  if (auto* text = qobject_cast<QTextEdit*>(w)) {
    return text->toPlainText();
  }
  if (auto* slider = qobject_cast<QAbstractSlider*>(w)) {
    return QString::number(slider->value());
  }
  if (auto* group = qobject_cast<QGroupBox*>(w)) {
    return group->title();
  }
  return {};
}

/// `Class#name "text"`, how ui_tree and the action replies name a widget.
std::string describe(QWidget* w) {
  std::string out = w->metaObject()->className();
  if (const QString name = ownName(w); !name.isEmpty()) {
    out += "#" + name.toStdString();
  }
  if (const std::string text = clip(shownText(w)); !text.empty()) {
    out += " \"" + text + "\"";
  }
  return out;
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
  QWindow* window = nullptr;
  QPointF local;
  QPointF global;
};

WindowPoint windowPoint(QWidget* host, QPoint p) {
  QWidget* top = host->window();
  if (top->windowHandle() == nullptr) {
    throw UiError("its window is not shown");
  }
  return {top->windowHandle(), QPointF(host->mapTo(top, p)), QPointF(host->mapToGlobal(p))};
}

/// The widget a press at `point` lands on.
std::string hitAt(const WindowPoint& point) {
  for (QWidget* top : QApplication::topLevelWidgets()) {
    if (top->windowHandle() == point.window) {
      QWidget* hit = top->childAt(point.local.toPoint());
      return describe(hit != nullptr ? hit : top);
    }
  }
  return "nothing";
}

void mouse(
    const WindowPoint& at, Qt::MouseButtons state, Qt::MouseButton button, QEvent::Type type,
    Qt::KeyboardModifiers mods) {
  // Queued, not synchronous: a press that opens a modal dialog must not block the call inside its exec().
  QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::AsynchronousDelivery>(
      at.window, at.local, at.global, state, button, type, mods);
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

void setUiToolsEnabled(bool enabled) {
  g_ui_tools_enabled = enabled;
}

bool uiToolsEnabled() {
  return g_ui_tools_enabled;
}

bool isUiTool(const std::string& name) {
  return name.starts_with("ui_");
}

json uiToolEntries() {
  const json target = {
      {"ref", {{"type", "string"}, {"description", "a ref from ui_tree, e.g. w12"}}},
      {"name", {{"type", "string"}, {"description", "or a visible widget's objectName"}}}};
  const json wait = {
      {"type", "integer"},
      {"description", "ms to let the app handle the input before answering (default 150, at most 5000)"}};
  const json point = {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 2}, {"maxItems", 2}};
  const json mods = {{"type", "string"}, {"description", "held modifiers, e.g. Ctrl+Shift"}};
  const auto with = [&](json properties) {
    for (const auto& [key, value] : target.items()) {
      properties[key] = value;
    }
    properties["wait_ms"] = wait;
    return properties;
  };
  return json::array({
      toolEntry(
          "ui_tree",
          "The visible windows as a user sees them, for driving the UI like a user (QA). One line per widget or "
          "item: [ref] Class#objectName \"text\" (flags) WxH; menu actions, tabs and the visible rows of lists and "
          "trees are items with their own ref. The open popup and the modal dialog come first; the windows a "
          "modal dialog blocks get one line each. Refs stay valid "
          "until their widget goes; call again after anything changed. These tools act as the user, also under "
          "their dialogs.",
          schema(json::object()), true),
      toolEntry(
          "ui_click",
          "Click a widget or item like the mouse does (press and release through the window system), at its "
          "center or at x,y inside it. Returns what the press landed on. Open menus by clicking the menu bar "
          "item, then the item in the popup ui_tree lists.",
          schema(with(
              {{"button", {{"type", "string"}, {"enum", {"left", "right", "middle"}}}},
               {"double", {{"type", "boolean"}}},
               {"x", {{"type", "number"}}},
               {"y", {{"type", "number"}}},
               {"modifiers", mods}})),
          false),
      toolEntry(
          "ui_drag",
          "Press at `from` (default: the center) of the target, move to `to` (inside the target) or to the center "
          "of `to_ref`, and release: zoom rectangles, panning, splitters, sliders. Drag-and-drop between widgets "
          "runs the platform's drag loop and may not complete.",
          schema(with(
              {{"from", point},
               {"to", point},
               {"to_ref", {{"type", "string"}}},
               {"button", {{"type", "string"}, {"enum", {"left", "right", "middle"}}}},
               {"modifiers", mods}})),
          false),
      toolEntry(
          "ui_wheel", "Turn the mouse wheel over the target (at its center or x,y): notches > 0 scrolls up / zooms in.",
          schema(with(
              {{"notches", {{"type", "integer"}}},
               {"x", {{"type", "number"}}},
               {"y", {{"type", "number"}}},
               {"modifiers", mods}})),
          false),
      toolEntry(
          "ui_key",
          "Press keys, as a key sequence (\"Ctrl+Z\", \"Return\", \"Escape\", \"Ctrl+S, Ctrl+Q\"), into the "
          "target after focusing it, or into whatever has focus. Window shortcuts fire only in the active "
          "window: the target's is activated first.",
          schema(with({{"keys", {{"type", "string"}}}}), {"keys"}), false),
      toolEntry(
          "ui_type",
          "Type text key by key into the target after focusing it, or into whatever has focus. \\n presses "
          "Return.",
          schema(with({{"text", {{"type", "string"}}}}), {"text"}), false),
  });
}

int UiDriver::settleMs(const std::string& tool, const json& args) {
  if (tool == "ui_tree") {
    return 0;
  }
  return std::clamp(args.value("wait_ms", kDefaultSettleMs), 0, kMaxSettleMs);
}

json UiDriver::call(const std::string& tool, const json& args) {
  try {
    if (tool == "ui_tree") {
      return tree();
    }
    if (tool == "ui_click") {
      return click(args);
    }
    if (tool == "ui_drag") {
      return drag(args);
    }
    if (tool == "ui_wheel") {
      return wheel(args);
    }
    if (tool == "ui_key") {
      return key(args);
    }
    if (tool == "ui_type") {
      return type(args);
    }
    return textResult("unknown tool " + tool, true);
  } catch (const UiError& e) {
    return textResult(e.what(), true);
  }
}

std::string UiDriver::addRef(Target target) {
  refs_.push_back(std::move(target));
  return "w" + std::to_string(ref_base_ + refs_.size());
}

json UiDriver::tree() {
  if (refs_.size() >= kMaxRefs) {
    ref_base_ += refs_.size();
    refs_.clear();
  }
  std::string out;
  int lines = 0;
  // A modal dialog blocks input to the windows behind it: those get one line each, not their tree.
  QWidget* modal = QApplication::activeModalWidget();
  for (QWidget* top : visibleTopLevels()) {
    if (modal != nullptr && top != modal && top != QApplication::activePopupWidget()) {
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
  const bool listed = w->isWindow() || interactive(w) || !ownName(w).isEmpty() || !clip(shownText(w)).empty() ||
                      !QByteArray(w->metaObject()->className()).startsWith('Q');
  int child_depth = depth;
  if (listed) {
    std::vector<std::string> flags;
    if (!w->isEnabled()) {
      flags.emplace_back("disabled");
    }
    if (auto* button = qobject_cast<QAbstractButton*>(w); button != nullptr && button->isCheckable()) {
      flags.emplace_back(button->isChecked() ? "checked" : "unchecked");
    }
    if (auto* group = qobject_cast<QGroupBox*>(w); group != nullptr && group->isCheckable()) {
      flags.emplace_back(group->isChecked() ? "checked" : "unchecked");
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
           describe(w) + flagList(flags) + " " + std::to_string(w->width()) + "x" + std::to_string(w->height()) + "\n";
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
    auto* menu = qobject_cast<QMenu*>(w);
    auto* bar = qobject_cast<QMenuBar*>(w);
    for (QAction* action : w->actions()) {
      const QRect rect = menu != nullptr ? menu->actionGeometry(action) : bar->actionGeometry(action);
      if (action->isSeparator() || !action->isVisible() || rect.isEmpty()) {
        continue;
      }
      std::vector<std::string> flags;
      if (!action->isEnabled()) {
        flags.emplace_back("disabled");
      }
      if (action->isCheckable()) {
        flags.emplace_back(action->isChecked() ? "checked" : "unchecked");
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
      item({.kind = Target::Kind::kTab, .widget = w, .tab = i}, "tab \"" + clip(tabs->tabText(i)) + "\"", flags);
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
  // The row at the top of the viewport: a point mid-width hits a cell of it whatever the indentation.
  QModelIndex row = view->indexAt(QPoint(view->viewport()->width() / 2, 1)).siblingAtColumn(0);
  if (!row.isValid()) {
    row = model->index(0, 0, view->rootIndex());
  }
  int shown = 0;
  for (; row.isValid() && view->visualRect(row).top() < bottom;
       row = tree != nullptr ? tree->indexBelow(row) : row.sibling(row.row() + 1, 0)) {
    if (shown == kMaxRowsPerView || lines >= kMaxLines) {
      out += indent + "- ... more rows below (ui_wheel scrolls)\n";
      ++lines;
      return;
    }
    QString text;
    for (int column = 0; column < std::min(model->columnCount(row.parent()), 4); ++column) {
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
      flags.emplace_back(check.value<Qt::CheckState>() == Qt::Unchecked ? "unchecked" : "checked");
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
  switch (target.kind) {
    case Target::Kind::kWidget:
      return {w, w->rect()};
    case Target::Kind::kAction: {
      if (target.action == nullptr) {
        throw UiError("that menu item is gone; call ui_tree again");
      }
      auto* menu = qobject_cast<QMenu*>(w);
      const QRect rect = menu != nullptr ? menu->actionGeometry(target.action)
                                         : qobject_cast<QMenuBar*>(w)->actionGeometry(target.action);
      if (rect.isEmpty()) {
        throw UiError("that menu item is not shown; call ui_tree again");
      }
      return {w, rect};
    }
    case Target::Kind::kTab: {
      auto* tabs = qobject_cast<QTabBar*>(w);
      if (target.tab >= tabs->count()) {
        throw UiError("that tab is gone; call ui_tree again");
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
    try {
      index = std::stoul(digits);
    } catch (const std::exception&) {
      throw UiError("ref '" + ref + "' is not one ui_tree gave");
    }
    if (index <= ref_base_ || index > ref_base_ + refs_.size()) {
      throw UiError("ref '" + ref + "' is unknown or stale; call ui_tree again");
    }
    return refs_[index - ref_base_ - 1];
  }
  if (const std::string name = args.value("name", std::string()); !name.empty()) {
    const QString object_name = QString::fromStdString(name);
    for (QWidget* top : visibleTopLevels()) {
      if (top->objectName() == object_name) {
        return {.kind = Target::Kind::kWidget, .widget = top};
      }
      for (QWidget* w : top->findChildren<QWidget*>(object_name)) {
        if (w->isVisible()) {
          return {.kind = Target::Kind::kWidget, .widget = w};
        }
      }
    }
    throw UiError("no visible widget is named '" + name + "'");
  }
  throw UiError("give a ref from ui_tree or a widget name");
}

namespace {

/// The point `args` ask for (`x`,`y` inside `rect`), else the center of `rect`.
QPoint pointIn(const QRect& rect, const json& args) {
  if (args.contains("x") || args.contains("y")) {
    return rect.topLeft() + QPoint(qRound(args.value("x", 0.0)), qRound(args.value("y", 0.0)));
  }
  return rect.center();
}

std::string at(const WindowPoint& point) {
  return std::to_string(qRound(point.local.x())) + "," + std::to_string(qRound(point.local.y()));
}

}  // namespace

json UiDriver::click(const json& args) {
  const auto [host, rect] = locate(resolve(args));
  const WindowPoint point = windowPoint(host, pointIn(rect, args));
  const Qt::MouseButton button = parseButton(args.value("button", std::string()));
  const Qt::KeyboardModifiers mods = parseModifiers(args.value("modifiers", std::string()));
  const std::string hit = hitAt(point);
  mouse(point, Qt::NoButton, Qt::NoButton, QEvent::MouseMove, mods);
  for (int i = 0; i < (args.value("double", false) ? 2 : 1); ++i) {
    mouse(point, button, button, QEvent::MouseButtonPress, mods);
    mouse(point, Qt::NoButton, button, QEvent::MouseButtonRelease, mods);
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
    return rect.topLeft() + QPoint(qRound(p.at(0).get<double>()), qRound(p.at(1).get<double>()));
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
    return WindowPoint{from.window, from.window->mapFromGlobal(global), global};
  };
  mouse(from, Qt::NoButton, Qt::NoButton, QEvent::MouseMove, mods);
  mouse(from, button, button, QEvent::MouseButtonPress, mods);
  for (int step = 1; step <= kDragSteps; ++step) {
    mouse(along(double(step) / kDragSteps), button, Qt::NoButton, QEvent::MouseMove, mods);
  }
  mouse(along(1.0), Qt::NoButton, button, QEvent::MouseButtonRelease, mods);
  return textResult(
      "dragged from " + at(from) + " to " + at(along(1.0)) + " of its window; the press landed on " + hit);
}

json UiDriver::wheel(const json& args) {
  const auto [host, rect] = locate(resolve(args));
  const WindowPoint point = windowPoint(host, pointIn(rect, args));
  const int notches = args.value("notches", 1);
  QWindowSystemInterface::handleWheelEvent(
      point.window, point.local, point.global, QPoint(), QPoint(0, notches * QWheelEvent::DefaultDeltasPerStep),
      parseModifiers(args.value("modifiers", std::string())));
  return textResult(
      "turned the wheel " + std::to_string(notches) + " notches at " + at(point) + " over " + hitAt(point));
}

namespace {

/// The window keys go to: the target's after focusing it (activated, so its shortcuts fire), else
/// the open popup's, the user's modal dialog's, or the focused widget's.
QWindow* keyWindow(QWidget* target) {
  QWidget* top = nullptr;
  if (target != nullptr) {
    target->setFocus(Qt::OtherFocusReason);
    top = target->window();
  } else if (QApplication::activePopupWidget() != nullptr) {
    top = QApplication::activePopupWidget();
  } else if (QApplication::activeModalWidget() != nullptr) {
    top = QApplication::activeModalWidget();
  } else if (QApplication::focusWidget() != nullptr) {
    top = QApplication::focusWidget()->window();
  } else {
    for (QWidget* w : QApplication::topLevelWidgets()) {
      if (qobject_cast<QMainWindow*>(w) != nullptr && w->isVisible()) {
        top = w;
      }
    }
  }
  if (top == nullptr || top->windowHandle() == nullptr) {
    throw UiError("no window to type into");
  }
  if (QApplication::activePopupWidget() == nullptr && QApplication::activeWindow() != top) {
    top->activateWindow();
  }
  return top->windowHandle();
}

}  // namespace

QWidget* UiDriver::keyTarget(const json& args) const {
  if (args.value("ref", std::string()).empty() && args.value("name", std::string()).empty()) {
    return nullptr;
  }
  const Target target = resolve(args);
  static_cast<void>(locate(target));  // refuses a gone or hidden one
  return target.widget;
}

json UiDriver::key(const json& args) {
  QWidget* target = keyTarget(args);
  const QString keys = QString::fromStdString(args.at("keys").get<std::string>());
  const QKeySequence sequence = QKeySequence::fromString(keys, QKeySequence::PortableText);
  if (sequence.isEmpty() || sequence[0].key() == Qt::Key_unknown) {
    throw UiError("could not read the key sequence '" + keys.toStdString() + "'");
  }
  QWindow* window = keyWindow(target);
  for (int i = 0; i < sequence.count(); ++i) {
    const QKeyCombination combo = sequence[i];
    const int code = combo.key();
    QString text;
    if (code >= Qt::Key_Space && code <= Qt::Key_AsciiTilde &&
        !(combo.keyboardModifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
      const QChar ch(code);
      text = combo.keyboardModifiers() & Qt::ShiftModifier ? QString(ch) : QString(ch.toLower());
    }
    keyPress(window, code, combo.keyboardModifiers(), text);
  }
  QWidget* focus = QApplication::focusWidget();
  return textResult(
      "pressed " + sequence.toString(QKeySequence::PortableText).toStdString() + "; focus was on " +
      (focus != nullptr ? describe(focus) : std::string("nothing")));
}

json UiDriver::type(const json& args) {
  QWidget* target = keyTarget(args);
  const QString text = QString::fromStdString(args.at("text").get<std::string>());
  QWindow* window = keyWindow(target);
  for (const QChar ch : text) {
    if (ch == u'\n') {
      keyPress(window, Qt::Key_Return, Qt::NoModifier, u"\r"_s);
    } else {
      keyPress(window, ch.isLetter() ? ch.toUpper().unicode() : ch.unicode(), Qt::NoModifier, QString(ch));
    }
  }
  QWidget* focus = QApplication::focusWidget();
  return textResult(
      "typed " + std::to_string(text.size()) + " characters into " +
      (focus != nullptr ? describe(focus) : std::string("nothing")));
}

}  // namespace PJ::mcp
