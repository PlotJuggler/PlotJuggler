// SPDX-License-Identifier: MPL-2.0
#pragma once

// A temp dir for an extension store root. StoreSession and ExtensionManager keep their state in siblings of the root
// (<root>.artifacts, .extraction, .profiles, .state, .install_stage, the .lock file), so a store rooted at a
// QTemporaryDir's own path puts those beside it, where nothing removes them and they pile up in TMPDIR. path() here
// is a "store" directory inside the temp dir: the siblings land inside it too and go away with it. Use it for every
// test store root; path() is canonical (the macOS temp dir sits behind a symlink and the store keeps canonical paths).

#include <QDir>
#include <QString>
#include <QTemporaryDir>

namespace PJ::test {

class StoreTempDir {
 public:
  StoreTempDir() {
    init();
  }
  explicit StoreTempDir(const QString& path_template) : dir_(path_template) {
    init();
  }

  [[nodiscard]] bool isValid() const {
    return !store_.isEmpty();
  }
  [[nodiscard]] const QString& path() const {
    return store_;
  }

 private:
  void init() {
    if (!dir_.isValid()) {
      return;
    }
    const QString store = QDir(dir_.path()).canonicalPath() + QStringLiteral("/store");
    if (QDir().mkpath(store)) {
      store_ = store;
    }
  }

  QTemporaryDir dir_;
  QString store_;  // empty when the temp dir or the store dir could not be created
};

}  // namespace PJ::test
