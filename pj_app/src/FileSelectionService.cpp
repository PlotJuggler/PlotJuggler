// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "FileSelectionService.h"

#include <QFileDialog>
#include <QRegularExpression>
#include <memory>
#include <string>
#include <utility>

#include "BrowserFileStore.h"
#ifdef Q_OS_WASM
#include <pj_plugins/host_qt/browser_file_picker_fallback.hpp>

#include "BrowserPickedFile.h"
#endif
using namespace Qt::StringLiterals;

namespace PJ {

QString FileSelectionService::browserContentFilter(const QString& name_filter) {
  return name_filter.section(u";;"_s, 0, 0);
}

QString FileSelectionService::browserAcceptList(const QString& name_filter) {
  static const QRegularExpression pattern(u"(?<![^\\s(])\\*(\\.[^\\s()*]+)?(?=[\\s)]|$)"_s);
  QStringList extensions;
  for (const QRegularExpressionMatch& match : pattern.globalMatch(browserContentFilter(name_filter))) {
    if (!match.hasCaptured(1)) {
      return {};
    }
    extensions.append(match.captured(1));
  }
  extensions.removeDuplicates();
  return extensions.join(u',');
}

void FileSelectionService::selectFileContent(QWidget* parent, const QString& name_filter, Callback callback) {
  const QString browser_filter = browserContentFilter(name_filter);
#ifdef Q_OS_WASM
  // Workaround (Qt 6.11 observed): Qt prefers the File System Access API when Chromium has it, but
  // showOpenFilePicker can reject at once in the cross-origin-isolated context a threaded build needs. Qt picks
  // its path during this call, so hide the probe only for the call to get the <input type=file> path.
  ScopedOpenFilePickerFallback fallback;
#endif
  QFileDialog::getOpenFileContent(  // NOLINT(pj-canonical-chrome): Qt's browser byte API has no PJ wrapper.
      browser_filter,
      [callback = std::move(callback)](const QString& name, const QByteArray& bytes) mutable {
        callback(Selection{.browser_name = name, .bytes = bytes});
      },
      parent);
}

void FileSelectionService::saveFileContent(QWidget* parent, const QByteArray& bytes, const QString& file_name_hint) {
#ifdef Q_OS_WASM
  // Workaround (Qt 6.11 observed): Qt gates its save File System Access path on showOpenFilePicker too, and the
  // save picker is not dependable under COOP/COEP. Hide the probe for this call to get the Blob/<a download> path.
  ScopedOpenFilePickerFallback fallback;
#endif
  QFileDialog::saveFileContent(  // NOLINT(pj-canonical-chrome): Qt's browser download API has no PJ wrapper.
      bytes, file_name_hint, parent);
}

void FileSelectionService::selectAndStageFile(
    QWidget* parent, const QString& name_filter, std::shared_ptr<BrowserFileStore> store, StagedCallback callback,
    Fingerprint fingerprint) {
#ifdef Q_OS_WASM
  Q_UNUSED(parent);
  browser_picked_file::pick(
      browserAcceptList(name_filter), [store = std::move(store), callback = std::move(callback), fingerprint](
                                          quint32 pick_id, QString browser_name) mutable {
        if (browser_name.isEmpty()) {
          callback({});
          return;
        }
        BrowserFileStore::StageResult staged =
            store ? store->reserve(browser_name)
                  : BrowserFileStore::StageResult{.error = QStringLiteral("Browser file staging is unavailable.")};
        if (!staged.input.has_value()) {
          browser_picked_file::forget(pick_id);
          callback({.browser_name = browser_name, .error = std::move(staged.error)});
          return;
        }
        LoadInput input = std::move(*staged.input);
        if (!browser_picked_file::mount(pick_id, input.backing_path)) {
          callback(
              {.browser_name = browser_name, .error = u"The browser lost the picked file %1."_s.arg(browser_name)});
          return;
        }
        if (fingerprint == Fingerprint::kInBackground) {
          callback({.browser_name = browser_name, .input = std::move(input)});
          return;
        }
        const QString path = input.backing_path;
        browser_picked_file::hash(
            path, [callback = std::move(callback), browser_name,
                   input = std::make_shared<LoadInput>(std::move(input))](QString sha256) mutable {
              if (sha256.isEmpty()) {
                // Dropping `input` releases its lease, which removes the mounted file.
                callback({
                    .browser_name = browser_name,
                    .error =
                        u"The browser could not read %1 (moved or changed since it was picked?)."_s.arg(browser_name),
                });
                return;
              }
              input->content_sha256 = std::move(sha256);
              callback({.browser_name = browser_name, .input = std::move(*input)});
            });
      });
#else
  Q_UNUSED(fingerprint);
  selectFileContent(
      parent, name_filter, [store = std::move(store), callback = std::move(callback)](Selection selection) mutable {
        if (selection.browser_name.isEmpty()) {
          callback({});
          return;
        }
        if (!store) {
          callback({
              .browser_name = selection.browser_name,
              .error = QStringLiteral("Browser file staging is unavailable."),
          });
          return;
        }
        const QString browser_name = selection.browser_name;
        store->stageAsync(
            selection.browser_name, std::move(selection.bytes),
            [callback = std::move(callback), browser_name](BrowserFileStore::StageResult result) mutable {
              callback({
                  .browser_name = browser_name,
                  .input = std::move(result.input),
                  .error = std::move(result.error),
              });
            });
      });
#endif
}

}  // namespace PJ
