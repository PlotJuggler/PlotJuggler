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

/// Minimal Python syntax highlighter for QPlainTextEdit code editors.
class PythonSyntaxHighlighter : public QSyntaxHighlighter {
 public:
  explicit PythonSyntaxHighlighter(QTextDocument* parent) : QSyntaxHighlighter(parent) {
    const auto fw_theme = theme::themeFor(QGuiApplication::palette().window().color().lightness() >= 128);

    QTextCharFormat keyword_fmt;
    keyword_fmt.setForeground(theme::syntaxInk(theme::SyntaxRole::Keyword, fw_theme));
    keyword_fmt.setFontWeight(QFont::Bold);
    const char* keywords[] = {
        "and",      "as",      "assert", "break", "class",  "continue", "def",    "del", "elif",  "else",   "except",
        "False",    "finally", "for",    "from",  "global", "if",       "import", "in",  "is",    "lambda", "None",
        "nonlocal", "not",     "or",     "pass",  "raise",  "return",   "True",   "try", "while", "with",   "yield"};
    for (const char* kw : keywords) {
      rules_.append({QRegularExpression("\\b" + QString(kw) + "\\b"), keyword_fmt});
    }

    rules_.append({QRegularExpression("@\\w+"), keyword_fmt});

    QTextCharFormat number_fmt;
    number_fmt.setForeground(theme::syntaxInk(theme::SyntaxRole::Number, fw_theme));
    rules_.append({QRegularExpression("\\b[0-9]+(\\.[0-9]+)?([eE][+-]?[0-9]+)?\\b"), number_fmt});

    string_fmt_.setForeground(theme::syntaxInk(theme::SyntaxRole::String, fw_theme));
    rules_.append({QRegularExpression("\"[^\"]*\""), string_fmt_});
    rules_.append({QRegularExpression("'[^']*'"), string_fmt_});

    comment_fmt_.setForeground(theme::syntaxInk(theme::SyntaxRole::Comment, fw_theme));
    comment_fmt_.setFontItalic(true);
    rules_.append({QRegularExpression("#[^\n]*"), comment_fmt_});

    QTextCharFormat builtin_fmt;
    builtin_fmt.setForeground(theme::syntaxInk(theme::SyntaxRole::Builtin, fw_theme));
    const char* builtins[] = {"print", "len",   "range", "type",      "int",        "float",   "str",    "list",
                              "dict",  "tuple", "set",   "enumerate", "zip",        "map",     "filter", "sorted",
                              "abs",   "min",   "max",   "sum",       "isinstance", "hasattr", "getattr"};
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

    // Triple-quoted strings span blocks. Block state: 0 normal, 1 inside """, 2 inside '''.
    handleTripleQuote(text, "\"\"\"", 1);
    handleTripleQuote(text, "'''", 2);
  }

 private:
  void handleTripleQuote(const QString& text, const QString& delimiter, int state) {
    qsizetype start_index = 0;
    if (previousBlockState() != state) {
      start_index = text.indexOf(delimiter);
    }
    while (start_index >= 0) {
      qsizetype end_index = text.indexOf(delimiter, start_index + 3);
      qsizetype length;
      if (end_index == -1) {
        setCurrentBlockState(state);
        length = text.length() - start_index;
      } else {
        length = end_index - start_index + 3;
      }
      setFormat(static_cast<int>(start_index), static_cast<int>(length), string_fmt_);
      start_index = text.indexOf(delimiter, start_index + length);
    }
  }

  struct Rule {
    QRegularExpression pattern;
    QTextCharFormat format;
  };
  QList<Rule> rules_;
  QTextCharFormat string_fmt_;
  QTextCharFormat comment_fmt_;
};

}  // namespace PJ
