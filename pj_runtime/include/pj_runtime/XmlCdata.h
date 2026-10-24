#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Header-only so pj_app's DOM-only test runner can use them without linking
// pj_runtime.

#include <QDomDocument>
#include <QDomElement>
#include <QDomNode>
#include <QLatin1StringView>
#include <QString>

namespace PJ {

// CDATA sections cannot contain "]]>"; QDomDocument::createCDATASection
// does not escape it. Splits the payload across adjacent CDATA sections
// at each "]]>" boundary so QDomElement::text() concatenates them back
// transparently on read.
inline void appendJsonAsCdata(QDomDocument& doc, QDomElement& parent, const QString& json) {
  // Indices kept as qsizetype to avoid narrowing on -Werror builds; Qt 6's
  // QString APIs return qsizetype throughout.
  qsizetype start = 0;
  while (true) {
    const qsizetype hit = json.indexOf(QLatin1StringView("]]>"), start);
    if (hit < 0) {
      parent.appendChild(doc.createCDATASection(json.mid(start)));
      return;
    }
    // End this section AFTER "]]" so the next section begins with ">".
    parent.appendChild(doc.createCDATASection(json.mid(start, hit + 2 - start)));
    start = hit + 2;
  }
}

// Concatenates ONLY the direct CDATA/text child nodes of `element`, skipping any
// nested child *elements*. QDomElement::text() recurses the entire subtree, so an
// element that holds its own CDATA payload alongside a child element with its own
// CDATA (e.g. a <processor> carrying params-CDATA plus a <source_fallback> child
// holding the filter's Luau source) cannot read its own payload via text()
// without the child's leaking in. This reads the element's own payload only.
[[nodiscard]] inline QString directCdataText(const QDomElement& element) {
  QString out;
  for (QDomNode n = element.firstChild(); !n.isNull(); n = n.nextSibling()) {
    if (n.isCDATASection() || n.isText()) {
      out += n.nodeValue();  // child *elements* (e.g. <source_fallback>) are skipped
    }
  }
  return out;
}

}  // namespace PJ
