#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The driver behind the UI tools (McpProtocol.h: uiToolsEnabled, uiToolEntries): an agent sees the
// widget tree and clicks, drags, scrolls and types like a user, for automated exploratory QA.
// Desktop only.

#include <QElapsedTimer>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QRect>
#include <QString>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

class QAction;
class QWidget;

namespace PJ::mcp {

/// Runs the UI tools for one server. Input goes through QWindowSystemInterface, the path a real
/// mouse and keyboard take (popups, grabs, focus, shortcuts, double-click synthesis), in native
/// pixels, and is queued or deferred to the event loop: a call returns before the input is handled,
/// so a click that opens a modal dialog does not block it. The server answers after the call's
/// settle time (mcp::uiSettleMs). A target in a window a modal dialog blocks is refused, as the
/// window system would drop its input. GUI thread only.
class UiDriver {
 public:
  /// Returns the CallToolResult. Throws on a wrongly typed argument.
  [[nodiscard]] nlohmann::json call(const std::string& tool, const nlohmann::json& args);

 private:
  /// What a ref names: a widget, or one item inside it (a menu action, a tab, a view row).
  struct Target {
    enum class Kind { kWidget, kAction, kTab, kRow } kind = Kind::kWidget;
    /// The widget, or the menu, menu bar, tab bar or item view holding the item.
    QPointer<QWidget> widget{};
    QPointer<QAction> action{};
    int tab = -1;
    /// A tab's text when listed: a tab ref whose index now holds another tab reads as stale.
    QString text{};
    QPersistentModelIndex index{};
  };
  /// Where `target` is on screen now: the widget that receives the input and the target's rect in
  /// its coordinates. Throws a message for the agent when it is gone or hidden.
  [[nodiscard]] static std::pair<QWidget*, QRect> locate(const Target& target);

  [[nodiscard]] nlohmann::json tree(const nlohmann::json& args);
  void dumpWidget(QWidget* widget, int depth, std::string& out, int& lines);
  void dumpItems(QWidget* widget, int depth, std::string& out, int& lines);
  [[nodiscard]] std::string addRef(Target target);
  /// The target `args` select (`ref` from ui_tree, or `name`, a visible widget's objectName);
  /// throws a message for the agent when none matches.
  [[nodiscard]] Target resolve(const nlohmann::json& args) const;
  /// The widget `args` name for ui_key / ui_type (a row's view itself), or null for "whatever has focus".
  [[nodiscard]] QWidget* keyTarget(const nlohmann::json& args) const;
  /// The timestamp of the next call's mouse input: at least a double-click interval after the
  /// previous call's, so two calls never make a double-click.
  [[nodiscard]] ulong nextStamp();
  [[nodiscard]] nlohmann::json click(const nlohmann::json& args);
  [[nodiscard]] nlohmann::json drag(const nlohmann::json& args);
  [[nodiscard]] nlohmann::json wheel(const nlohmann::json& args);
  [[nodiscard]] nlohmann::json key(const nlohmann::json& args);
  [[nodiscard]] nlohmann::json type(const nlohmann::json& args);

  /// The latest ui_tree's refs: `w<ref_base_ + i + 1>` at index i. Each ui_tree starts a new table
  /// and numbering goes on, so an older ref reads as stale, never as another widget.
  std::vector<Target> refs_;
  std::size_t ref_base_ = 0;
  QElapsedTimer clock_;
  ulong last_stamp_ = 0;
};

}  // namespace PJ::mcp
