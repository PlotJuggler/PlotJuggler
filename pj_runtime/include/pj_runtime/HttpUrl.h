#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Kept apart from HttpGet.h so code that only classifies URLs needs no Qt6::Network.

#include <QString>
#include <QUrl>

namespace PJ {

// A valid absolute http(s) URL with a host: the only URLs the browser-facing callers hand to a
// network stack that would also read file:// natively.
[[nodiscard]] inline bool isHttpUrl(const QUrl& url) {
  const QString scheme = url.scheme();
  return url.isValid() && !url.host().isEmpty() &&
         (scheme.compare(QLatin1StringView("http"), Qt::CaseInsensitive) == 0 ||
          scheme.compare(QLatin1StringView("https"), Qt::CaseInsensitive) == 0);
}

}  // namespace PJ
