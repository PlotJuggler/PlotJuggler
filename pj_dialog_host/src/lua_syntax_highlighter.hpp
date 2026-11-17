#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QGuiApplication>
#include <QPalette>
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>

#include "pj_widgets/FrameworkTokens.h"

namespace PJ {

/// Minimal Lua syntax highlighter for QPlainTextEdit code editors.
class LuaSyntaxHighlighter : public QSyntaxHighlighter {
 public:
  explicit LuaSyntaxHighlighter(QTextDocument* parent) : QSyntaxHighlighter(parent) {
    const auto fw_theme = theme::themeFor(QGuiApplication::palette().window().color().lightness() >= 128);

    QTextCharFormat keyword_fmt;
    keyword_fmt.setForeground(theme::syntaxInk(theme::SyntaxRole::Keyword, fw_theme));
    keyword_fmt.setFontWeight(QFont::Bold);
    const char* keywords[] = {"and",      "break",  "do",   "else", "elseif", "end",  "false", "for",
                              "function", "goto",   "if",   "in",   "local",  "nil",  "not",   "or",
                              "repeat",   "return", "then", "true", "until",  "while"};
    for (const char* kw : keywords) {
      rules_.append({QRegularExpression("\\b" + QString(kw) + "\\b"), keyword_fmt});
    }

    QTextCharFormat number_fmt;
    number_fmt.setForeground(theme::syntaxInk(theme::SyntaxRole::Number, fw_theme));
    rules_.append({QRegularExpression("\\b[0-9]+(\\.[0-9]+)?([eE][+-]?[0-9]+)?\\b"), number_fmt});

    QTextCharFormat string_fmt;
    string_fmt.setForeground(theme::syntaxInk(theme::SyntaxRole::String, fw_theme));
    rules_.append({QRegularExpression("\"[^\"]*\""), string_fmt});
    rules_.append({QRegularExpression("'[^']*'"), string_fmt});

    comment_fmt_.setForeground(theme::syntaxInk(theme::SyntaxRole::Comment, fw_theme));
    comment_fmt_.setFontItalic(true);
    rules_.append({QRegularExpression("--[^\n]*"), comment_fmt_});

    QTextCharFormat builtin_fmt;
    builtin_fmt.setForeground(theme::syntaxInk(theme::SyntaxRole::Builtin, fw_theme));
    const char* builtins[] = {"print", "type",  "tostring", "tonumber", "pairs", "ipairs",
                              "math",  "table", "string",   "assert",   "error", "pcall"};
    for (const char* bi : builtins) {
      rules_.append({QRegularExpression("\\b" + QString(bi) + "\\b"), builtin_fmt});
    }
  }

 protected:
  void highlightBlock(const QString& text) override {
    for (const auto& rule : rules_) {
      auto it = rule.pattern.globalMatch(text);
      while (it.hasNext()) {
        auto match = it.next();
        setFormat(static_cast<int>(match.capturedStart()), static_cast<int>(match.capturedLength()), rule.format);
      }
    }

    // Multi-line comments --[[ ... ]] carry state across blocks.
    setCurrentBlockState(0);
    qsizetype start_index = 0;
    if (previousBlockState() != 1) {
      start_index = text.indexOf("--[[");
    }
    while (start_index >= 0) {
      qsizetype end_index = text.indexOf("]]", start_index + 4);
      qsizetype length;
      if (end_index == -1) {
        setCurrentBlockState(1);
        length = text.length() - start_index;
      } else {
        length = end_index - start_index + 2;
      }
      setFormat(static_cast<int>(start_index), static_cast<int>(length), comment_fmt_);
      start_index = text.indexOf("--[[", start_index + length);
    }
  }

 private:
  struct Rule {
    QRegularExpression pattern;
    QTextCharFormat format;
  };
  QList<Rule> rules_;
  QTextCharFormat comment_fmt_;
};

}  // namespace PJ
