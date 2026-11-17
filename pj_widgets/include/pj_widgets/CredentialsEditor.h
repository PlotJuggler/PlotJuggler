#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;

namespace PJ {

// Composite credentials editor for a plugin .ui's `<widget class="CredentialsEditor">`. It builds three inner inputs
// with the objectNames the PanelEngine sub-dialog harvester expects:
//   - `certPath`      QLineEdit (+ a Browse button, + `certTick` validity label)
//   - `apiKey`        QLineEdit, masked, with a Show/Hide reveal toggle, + `apiKeyTick`
//   - `allowInsecure` QCheckBox (plaintext fallback)
// The plugin pre-fills them with setText / setChecked by name and reads them back through onTextChanged / onToggled on
// sub-dialog OK. The validity ticks (green check, red cross) update live on textChanged through the predicates below,
// with no protocol round trip.
//
// Domain-neutral: it knows no provider's key format. The api-key tick defaults to "non-empty is valid"; a plugin that
// wants a stricter shape declares a regular expression in its own `.ui` through `apiKeyPattern` (applied by QUiLoader
// at construction), e.g.
//   <property name="apiKeyPattern"><string>^key_[a-z0-9]{32}$</string></property>
// so the format policy lives with the plugin that owns it.
class CredentialsEditor : public QWidget {
  Q_OBJECT
  // Regex the api-key must fully match to be valid; empty (the default) means any non-empty key is valid. Set by the
  // plugin's .ui.
  Q_PROPERTY(QString apiKeyPattern READ apiKeyPattern WRITE setApiKeyPattern)
 public:
  explicit CredentialsEditor(QWidget* parent = nullptr);

  [[nodiscard]] QString apiKeyPattern() const {
    return api_key_pattern_;
  }
  // Sets the api-key regex and re-evaluates the live tick; an empty or invalid pattern falls back to the non-empty
  // check.
  void setApiKeyPattern(const QString& pattern);

  // True when `key` is acceptable under `pattern`: an empty pattern accepts any non-empty key, otherwise `key` must
  // fully match. Pure, so tested directly.
  [[nodiscard]] static bool isApiKeyValid(const QString& key, const QString& pattern = QString());
  // True when `path` names an existing, readable file.
  [[nodiscard]] static bool isCertReadable(const QString& path);

 private:
  // Sets `tick` to a green check, a red cross, or blank (when `text` is empty) per `ok`.
  static void applyTick(QLabel* tick, const QString& text, bool ok);
  // Re-runs the api-key tick against the current text and pattern.
  void refreshApiKeyTick();

  QLineEdit* cert_path_ = nullptr;
  QLineEdit* api_key_ = nullptr;
  QCheckBox* allow_insecure_ = nullptr;
  QLabel* cert_tick_ = nullptr;
  QLabel* api_key_tick_ = nullptr;
  QString api_key_pattern_;
};

}  // namespace PJ
