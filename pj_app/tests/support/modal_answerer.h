#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The one way a pj_app test answers modal dialogs: ModalAnswerer, and matchers and readers for PJ::MessageBox.

#include <QAbstractButton>
#include <QApplication>
#include <QLabel>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

namespace pj_app_test {

// Answers modal dialogs from a polling timer, as a user would: a dialog shown with exec() runs a nested loop, inside
// which the timer still fires. Each tick looks at the visible modal dialogs, the top one first, and hands the first one
// a rule accepts to that rule, once per dialog. A dialog no rule accepts is left open and listed in unmatched(); a rule
// whose match holds later still takes it then. Never act on whatever dialog is on top: a refused file's non-blocking
// "Load failed" box sits on top until the next dialog opens over it, and a tick between the two finds the box instead.
//
// An answer that closes the dialog should queue the click or accept() (QMetaObject::invokeMethod with
// Qt::QueuedConnection), so the dialog's exec() returns outside the tick. PJ_TEST_MODAL_POLL_MS overrides every
// answerer's interval: at 1 ms a tick lands between two dialogs opening back to back, which a slow CI runner does now
// and then at the default.
class ModalAnswerer {
 public:
  using Match = std::function<bool(QWidget*)>;
  using Answer = std::function<void(QWidget*)>;

  explicit ModalAnswerer(int interval_ms = 20) {
    const int override_ms = qEnvironmentVariableIntValue("PJ_TEST_MODAL_POLL_MS");
    timer_.setInterval(override_ms > 0 ? override_ms : interval_ms);
    QObject::connect(&timer_, &QTimer::timeout, &timer_, [this]() { poll(); });
    timer_.start();
  }

  // Answers every dialog `match` accepts, each once.
  ModalAnswerer& every(Match match, Answer answer) {
    rules_.push_back({std::move(match), std::move(answer), /*once=*/false});
    return *this;
  }

  // Answers the next dialog `match` accepts; the rule then retires.
  ModalAnswerer& next(Match match, Answer answer) {
    rules_.push_back({std::move(match), std::move(answer), /*once=*/true});
    return *this;
  }

  void stop() {
    timer_.stop();
  }

  [[nodiscard]] int answered() const {
    return answered_;
  }

  // Each dialog that was on top while no rule accepted it: "<class>#<objectName> <title>".
  [[nodiscard]] const QStringList& unmatched() const {
    return unmatched_;
  }

 private:
  struct Rule {
    Match match;
    Answer answer;
    bool once = false;
    bool spent = false;
  };

  // The top modal first, then every other visible modal window.
  static std::vector<QWidget*> modals() {
    std::vector<QWidget*> found;
    if (QWidget* top = QApplication::activeModalWidget(); top != nullptr) {
      found.push_back(top);
    }
    for (QWidget* window : QApplication::topLevelWidgets()) {
      if (window->isVisible() && window->isModal() && std::find(found.begin(), found.end(), window) == found.end()) {
        found.push_back(window);
      }
    }
    return found;
  }

  void poll() {
    for (QWidget* modal : modals()) {
      if (modal->property("pjModalAnswered").isValid()) {
        continue;
      }
      for (Rule& rule : rules_) {
        if (rule.spent || !rule.match(modal)) {
          continue;
        }
        modal->setProperty("pjModalAnswered", true);
        rule.spent = rule.once;
        ++answered_;
        rule.answer(modal);
        return;
      }
      if (!modal->property("pjModalUnmatched").isValid()) {
        modal->setProperty("pjModalUnmatched", true);
        unmatched_ << QString::fromUtf8(modal->metaObject()->className()) + u'#' + modal->objectName() + u' ' +
                          modal->windowTitle();
      }
    }
  }

  std::vector<Rule> rules_;
  QStringList unmatched_;
  int answered_ = 0;
  QTimer timer_;
};

// PJ::MessageBox: a dialog named "pjMessageBox", its buttons named "pjMessageBoxButton" in addButton order (the first
// is the primary action), its text in the label "pjMessageBoxBody".
inline bool isMessageBox(const QWidget* dialog) {
  return dialog->objectName() == QStringLiteral("pjMessageBox");
}

inline QList<QAbstractButton*> messageBoxButtons(const QWidget* box) {
  return box->findChildren<QAbstractButton*>(QStringLiteral("pjMessageBoxButton"));
}

// The button labelled `label`, or nullptr.
inline QAbstractButton* messageBoxButton(const QWidget* box, const QString& label) {
  for (QAbstractButton* button : messageBoxButtons(box)) {
    if (button->text() == label) {
      return button;
    }
  }
  return nullptr;
}

inline QString messageBoxBody(const QWidget* box) {
  const auto* body = box->findChild<QLabel*>(QStringLiteral("pjMessageBoxBody"));
  return body != nullptr ? body->text() : QString();
}

// A message box with a button labelled one of `labels`.
inline ModalAnswerer::Match messageBoxWithButton(QStringList labels) {
  return [labels = std::move(labels)](QWidget* dialog) {
    if (!isMessageBox(dialog)) {
      return false;
    }
    for (const QString& label : labels) {
      if (messageBoxButton(dialog, label) != nullptr) {
        return true;
      }
    }
    return false;
  };
}

// Clicks the first of `labels` the box has, queued; returns its label (empty when none).
inline QString clickMessageBoxButton(QWidget* box, const QStringList& labels) {
  for (const QString& label : labels) {
    if (QAbstractButton* button = messageBoxButton(box, label); button != nullptr) {
      QMetaObject::invokeMethod(button, &QAbstractButton::click, Qt::QueuedConnection);
      return label;
    }
  }
  return {};
}

// A dialog holding a child of type T named `name`.
template <typename T>
ModalAnswerer::Match hasChild(QString name) {
  return [name = std::move(name)](QWidget* dialog) { return dialog->findChild<T*>(name) != nullptr; };
}

}  // namespace pj_app_test
