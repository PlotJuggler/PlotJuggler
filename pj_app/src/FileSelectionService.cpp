// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "FileSelectionService.h"

#include <QFileDialog>
#include <QRegularExpression>
#include <string>
#include <utility>

#include "BrowserFileStore.h"
#ifdef Q_OS_WASM
#include <emscripten/emscripten.h>
#include <emscripten/val.h>

#include <QUrl>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <unordered_map>

#include "pj_runtime/BrowserFetch.h"
#endif
using namespace Qt::StringLiterals;

#ifdef Q_OS_WASM
namespace PJ {
namespace {

// Reports a pick: the browser's file name and a blob: URL for the File, both empty on cancel, and its size in bytes.
using PickCompletion = std::function<void(QString name, QString blob_url, qint64 size)>;

// The largest pick the browser can stage: it is copied whole into one JavaScript array, and Chromium allocates
// none larger (2 GiB - 2 MiB); Firefox, which can, then fails to hash one of 2 GiB with WebCrypto. It also keeps
// every offset in the file within a 32-bit `long`, which loaders on wasm32 seek with (ftell/fseek).
constexpr qint64 kMaxBrowserPickBytes = 2145386496;

// The picks whose <input> has not settled yet, by id. Browser main thread only.
std::unordered_map<quint32, PickCompletion>& pendingPicks() {
  static std::unordered_map<quint32, PickCompletion> instance;
  return instance;
}

// Shows the browser's file picker through a detached <input type=file> and hands the chosen File back as an object
// URL, so its bytes can be streamed instead of read whole (Qt's getOpenFileContent holds the file in a QByteArray).
// The input settles once, on `change` or on `cancel`; a browser that never fires `cancel` leaves the pick pending,
// as Qt's own picker does. The macro joins the body into one line: every statement ends in a semicolon.
// clang-format off
EM_JS_DEPS(pj_browser_pick, "$stringToNewUTF8");
EM_JS(void, pj_browser_pick_file, (unsigned id, const char* accept_utf8), {
  const input = document.createElement('input');
  input.type = 'file';
  input.accept = UTF8ToString(accept_utf8);
  input.style.display = 'none';
  let settled = false;
  const settle = (file) => {
    if (settled) {
      return;
    }
    settled = true;
    input.remove();
    const name = stringToNewUTF8(file ? file.name : '');
    const url = stringToNewUTF8(file ? URL.createObjectURL(file) : '');
    Module._pj_browser_pick_done(id, name, url, file ? file.size : 0);
  };
  input.addEventListener('change', () => settle(input.files && input.files.length > 0 ? input.files[0] : null));
  input.addEventListener('cancel', () => settle(null));
  document.body.appendChild(input);
  input.click();
});
// clang-format on

void pickBrowserFile(const QString& accept, PickCompletion completion) {
  static quint32 last_id = 0;
  const quint32 id = ++last_id;
  pendingPicks()[id] = std::move(completion);
  pj_browser_pick_file(id, accept.toUtf8().constData());
}

void revokeObjectUrl(const QString& blob_url) {
  if (!blob_url.isEmpty()) {
    emscripten::val::global("URL").call<void>("revokeObjectURL", blob_url.toStdString());
  }
}

}  // namespace
}  // namespace PJ

/// The <input> of pick `id` settled; `name` and `url` are malloc'ed by the page and freed here.
extern "C" EMSCRIPTEN_KEEPALIVE void pj_browser_pick_done(quint32 id, char* name, char* url, double size) {
  const QString browser_name = QString::fromUtf8(name);
  const QString blob_url = QString::fromUtf8(url);
  free(name);
  free(url);
  auto node = PJ::pendingPicks().extract(id);
  if (!node.empty()) {
    node.mapped()(browser_name, blob_url, static_cast<qint64>(size));
  }
}
#endif

namespace PJ {

#ifdef Q_OS_WASM
namespace {

class ScopedOpenFilePickerFallback {
 public:
  ScopedOpenFilePickerFallback()
      : window_(emscripten::val::global("window")),
        picker_(window_["showOpenFilePicker"]),
        had_own_property_(
            emscripten::val::global("Object")["prototype"]["hasOwnProperty"].call<bool>(
                "call", window_, std::string("showOpenFilePicker"))) {
    window_.set("showOpenFilePicker", emscripten::val::undefined());
  }

  ~ScopedOpenFilePickerFallback() {
    if (had_own_property_) {
      window_.set("showOpenFilePicker", picker_);
    } else {
      emscripten::val::global("Reflect").call<bool>("deleteProperty", window_, emscripten::val("showOpenFilePicker"));
    }
  }

  ScopedOpenFilePickerFallback(const ScopedOpenFilePickerFallback&) = delete;
  ScopedOpenFilePickerFallback& operator=(const ScopedOpenFilePickerFallback&) = delete;

 private:
  emscripten::val window_;
  emscripten::val picker_;
  bool had_own_property_;
};

}  // namespace
#endif

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
  // The aggregate first filter already contains every supported extension.
  const QString browser_filter = browserContentFilter(name_filter);
#ifdef Q_OS_WASM
  // Qt 6.11 prefers the File System Access API when Chromium exposes it, but
  // showOpenFilePicker can reject immediately in the cross-origin-isolated
  // context required by a threaded build. Qt chooses its implementation during
  // this call, so mask only the open-picker probe long enough to select its
  // portable <input type=file> path, then restore the browser API immediately.
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
  // Qt 6.11 gates BOTH its open and save File System Access paths on
  // showOpenFilePicker. In the COOP/COEP context needed by this threaded build,
  // the save picker is not dependable. Mask only the capability probe for the
  // duration of this call so Qt chooses its synchronous Blob/<a download>
  // fallback, then restore the browser API immediately.
  ScopedOpenFilePickerFallback fallback;
#endif
  QFileDialog::saveFileContent(  // NOLINT(pj-canonical-chrome): Qt's browser download API has no PJ wrapper.
      bytes, file_name_hint, parent);
}

void FileSelectionService::selectAndStageFile(
    QWidget* parent, const QString& name_filter, std::shared_ptr<BrowserFileStore> store, StagedCallback callback) {
#ifdef Q_OS_WASM
  Q_UNUSED(parent);
  pickBrowserFile(
      browserAcceptList(name_filter),
      [store = std::move(store), callback = std::move(callback)](QString browser_name, QString blob_url, qint64 size) {
        if (browser_name.isEmpty()) {
          revokeObjectUrl(blob_url);
          callback({});
          return;
        }
        if (size > kMaxBrowserPickBytes) {
          revokeObjectUrl(blob_url);
          callback({
              .browser_name = browser_name,
              .error = u"%1 is too large to open in the browser (%2 MiB; the limit is %3 MiB)."_s.arg(browser_name)
                           .arg(static_cast<double>(size) / (1024.0 * 1024.0), 0, 'f', 1)
                           .arg(static_cast<double>(kMaxBrowserPickBytes) / (1024.0 * 1024.0), 0, 'f', 1),
          });
          return;
        }
        BrowserFileStore::StageResult staged =
            store ? store->reserve(browser_name)
                  : BrowserFileStore::StageResult{.error = QStringLiteral("Browser file staging is unavailable.")};
        if (!staged.input.has_value()) {
          revokeObjectUrl(blob_url);
          callback({.browser_name = browser_name, .error = std::move(staged.error)});
          return;
        }
        const QString path = staged.input->backing_path;
        // A local read never stalls like a network one, so the idle timeout only catches a File that went away.
        browserFetchToFile(
            QUrl(blob_url), path, std::numeric_limits<qint64>::max(), std::chrono::seconds(60),
            [callback, browser_name, blob_url, input = std::move(*staged.input)](BrowserFetchResult result) mutable {
              revokeObjectUrl(blob_url);
              if (!result.error.isEmpty()) {
                // Dropping `input` releases its lease, which removes the partial file.
                callback({
                    .browser_name = browser_name,
                    .error =
                        u"The browser could not read %1 (moved or changed since it was picked?)."_s.arg(browser_name),
                });
                return;
              }
              input.content_sha256 = result.sha256;
              callback({.browser_name = browser_name, .input = std::move(input)});
            });
      });
#else
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
