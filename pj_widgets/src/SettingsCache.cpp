// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_widgets/SettingsCache.h"

#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QSettings>

namespace PJ {
namespace {

struct Cache {
  QMutex mutex;
  QHash<QString, QVariant> values;  // an invalid QVariant: the key is absent
};

Cache& cache() {
  static Cache instance;
  return instance;
}

}  // namespace

QVariant cachedSetting(const QString& key, const QVariant& fallback) {
  Cache& c = cache();
  QMutexLocker lock(&c.mutex);
  auto it = c.values.constFind(key);
  if (it == c.values.cend()) {
    it = c.values.insert(key, QSettings().value(key));
  }
  return it->isValid() ? *it : fallback;
}

void writeCachedSetting(const QString& key, const QVariant& value) {
  Cache& c = cache();
  QMutexLocker lock(&c.mutex);
  QSettings().setValue(key, value);
  c.values.insert(key, value);
}

void invalidateCachedSettings() {
  Cache& c = cache();
  QMutexLocker lock(&c.mutex);
  c.values.clear();
}

}  // namespace PJ
