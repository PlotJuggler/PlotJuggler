#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The UI tools: an agent sees the widget tree and clicks, drags, scrolls and types like a user, for
// automated exploratory QA. Off unless the run asked for them (`--mcp-ui-tools`): they act as the
// user, under the user's dialogs too, which no ordinary agent may.

#include <QPersistentModelIndex>
#include <QPointer>
#include <QRect>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

class QAction;
class QWidget;

namespace PJ::mcp {

/// Turns the UI tools on for this process (main, from `--mcp-ui-tools`; tests). Read by toolList
/// and the server; never turned off again.
void setUiToolsEnabled(bool enabled);
[[nodiscard]] bool uiToolsEnabled();

/// Whether `name` is one of the UI tools (`ui_*`).
[[nodiscard]] bool isUiTool(const std::string& name);

/// Their tools/list entries.
[[nodiscard]] nlohmann::json uiToolEntries();

/// Runs the UI tools for one server. Input goes through QWindowSystemInterface, the path a real
/// mouse and keyboard take (popups, grabs, focus, shortcuts, double-click synthesis), and is queued:
/// a call returns before the input is handled, so a click that opens a modal dialog does not block
/// it. The server answers after the call's settle time. GUI thread only.
class UiDriver {
 public:
  /// Runs `tool` with `args`; returns its CallToolResult. Throws on a wrongly typed argument.
  [[nodiscard]] nlohmann::json call(const std::string& tool, const nlohmann::json& args);

  /// Milliseconds the server waits after `tool` before answering, so the input it queued is handled
  /// (the `wait_ms` argument, clamped; 0 for ui_tree).
  [[nodiscard]] static int settleMs(const std::string& tool, const nlohmann::json& args);

 private:
  /// What a ref names: a widget, or one item inside it (a menu action, a tab, a view row).
  struct Target {
    enum class Kind { kWidget, kAction, kTab, kRow } kind = Kind::kWidget;
    /// The widget, or the menu, menu bar, tab bar or item view holding the item.
    QPointer<QWidget> widget{};
    QPointer<QAction> action{};
    int tab = -1;
    QPersistentModelIndex index{};
  };
  /// Where `target` is on screen now: the widget that receives the input and the target's rect in
  /// its coordinates. Throws a message for the agent when it is gone or hidden.
  [[nodiscard]] static std::pair<QWidget*, QRect> locate(const Target& target);

  [[nodiscard]] nlohmann::json tree();
  void dumpWidget(QWidget* widget, int depth, std::string& out, int& lines);
  void dumpItems(QWidget* widget, int depth, std::string& out, int& lines);
  [[nodiscard]] std::string addRef(Target target);
  /// The target `args` select (`ref` from ui_tree, or `name`, a visible widget's objectName);
  /// throws a message for the agent when none matches.
  [[nodiscard]] Target resolve(const nlohmann::json& args) const;
  /// The widget `args` name for ui_key / ui_type (a row's view itself), or null for "whatever has focus".
  [[nodiscard]] QWidget* keyTarget(const nlohmann::json& args) const;
  [[nodiscard]] nlohmann::json click(const nlohmann::json& args);
  [[nodiscard]] nlohmann::json drag(const nlohmann::json& args);
  [[nodiscard]] nlohmann::json wheel(const nlohmann::json& args);
  [[nodiscard]] nlohmann::json key(const nlohmann::json& args);
  [[nodiscard]] nlohmann::json type(const nlohmann::json& args);

  /// The refs handed out since the table was last cleared: `w<ref_base_ + i + 1>` at index i. Refs
  /// stay valid across ui_tree calls while their object lives. ui_tree clears the table once it grew
  /// past a few thousand; numbering goes on, so an older ref reads as stale, never as another widget.
  std::vector<Target> refs_;
  std::size_t ref_base_ = 0;
};

}  // namespace PJ::mcp
