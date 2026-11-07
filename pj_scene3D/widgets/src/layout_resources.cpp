// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_widgets/layout_resources.h"

#include <QCoreApplication>
#include <QDir>
#include <QDomDocument>
#include <QDomElement>
#include <QDomNodeList>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <optional>
#include <vector>

#include "pj_runtime/HttpUrl.h"

using namespace Qt::StringLiterals;

namespace pj::scene3d {
namespace {

bool isAllowedAbsolute(const QUrl& url) {
  return PJ::isHttpUrl(url) || url.scheme() == "file"_L1;
}

// The relative reference to a directory: the trailing slash makes the packages beneath it
// resolve inside it, and a first segment holding ':' would otherwise parse as a scheme.
QUrl relativeRootUrl(const QString& relative_dir) {
  const bool colon_in_first_segment = relative_dir.section(u'/', 0, 0).contains(u':');
  QUrl url;
  url.setPath((colon_in_first_segment ? u"./"_s : QString()) + relative_dir + u'/', QUrl::DecodedMode);
  return url;
}

}  // namespace

std::optional<QMap<QString, QUrl>> parsePackageUrlRoots(const QString& json) {
  QMap<QString, QUrl> roots;
  if (json.isEmpty()) {
    return roots;
  }
  const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8());
  if (!document.isObject()) {
    return std::nullopt;
  }
  const QJsonObject object = document.object();
  for (auto entry = object.begin(); entry != object.end(); ++entry) {
    const QString value = entry.value().toString();
    const QUrl root(value);
    if (entry.key().isEmpty() || value.isEmpty() || !root.isValid() ||
        (!root.isRelative() && !isAllowedAbsolute(root))) {
      return std::nullopt;
    }
    roots.insert(entry.key(), root);
  }
  return roots;
}

QString serializePackageUrlRoots(const QMap<QString, QUrl>& roots) {
  QJsonObject object;
  for (auto root = roots.cbegin(); root != roots.cend(); ++root) {
    object.insert(root.key(), root.value().toString(QUrl::FullyEncoded));
  }
  return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

bool resolveLayoutResourceUrls(QDomDocument& document, const QUrl& layout_location, QString* error) {
  const auto fail = [error](const QString& message) {
    if (error != nullptr) {
      *error = message;
    }
    return false;
  };
  const auto unanchored = [&fail](const QString& reference) {
    return fail(
        QCoreApplication::translate(
            "LayoutResources",
            "robot model reference '%1' is relative, but the layout has no location to resolve it against")
            .arg(reference));
  };
  const auto local = [&fail](const QString& reference) {
    return fail(
        QCoreApplication::translate(
            "LayoutResources", "robot model reference '%1' is a local file, which a web-hosted layout cannot use")
            .arg(reference));
  };
  // Resolving keeps the location's scheme, so an allowed anchor yields allowed URLs.
  const bool anchored = layout_location.isValid() && isAllowedAbsolute(layout_location);
  // A web-hosted layout never points the app at local files.
  const bool web_hosted = anchored && PJ::isHttpUrl(layout_location);

  // Collected first and applied only once every model is known to be valid.
  struct Edit {
    QDomElement element;
    QString attribute;
    std::optional<QString> value;  // nullopt removes the attribute
  };
  std::vector<Edit> edits;
  const QDomNodeList models = document.elementsByTagName(u"robot_model"_s);
  for (int index = 0; index < models.size(); ++index) {
    const QDomElement model = models.at(index).toElement();
    const QString source_type = model.attribute(u"source_type"_s);
    const bool url_source = source_type == "url"_L1;
    const QString source = model.attribute(u"source_value"_s);
    if (web_hosted && source_type == "file"_L1) {
      return local(source);
    }
#ifndef PJ_TARGET_WASM
    // The browser keeps its file label: it re-asks for the bytes.
    if (source_type == "file"_L1 && !source.isEmpty() && !QDir::isAbsolutePath(source) &&
        layout_location.isLocalFile()) {
      const QDir layout_dir = QFileInfo(layout_location.toLocalFile()).absoluteDir();
      edits.push_back({model, u"source_value"_s, QDir::cleanPath(layout_dir.absoluteFilePath(source))});
    }
#endif
    if (url_source && !source.isEmpty() && QUrl(source).isRelative() && !QDir::isAbsolutePath(source)) {
      if (!anchored) {
        return unanchored(source);
      }
      edits.push_back({model, u"source_value"_s, layout_location.resolved(QUrl(source)).toString(QUrl::FullyEncoded)});
    } else if (web_hosted && url_source && !source.isEmpty() && !PJ::isHttpUrl(QUrl(source))) {
      return local(source);
    }
    if (web_hosted) {
      // The URDF this model loads names meshes this check never sees: the layer refuses local ones.
      edits.push_back({model, u"remote_resources_only"_s, u"true"_s});
    } else if (model.hasAttribute(u"remote_resources_only"_s)) {
      // The policy follows where the layout was loaded from, not where it was saved.
      edits.push_back({model, u"remote_resources_only"_s, std::nullopt});
    }

    if (!model.hasAttribute(u"package_url_roots"_s)) {
      continue;
    }
    std::optional<QMap<QString, QUrl>> roots = parsePackageUrlRoots(model.attribute(u"package_url_roots"_s));
    if (!roots.has_value()) {
      return fail(
          QCoreApplication::translate(
              "LayoutResources",
              "a robot model's package_url_roots must be a JSON object mapping package names to http(s) or "
              "file URLs"));
    }
    if (web_hosted) {
      for (const QUrl& root : std::as_const(*roots)) {
        if (!root.isRelative() && !PJ::isHttpUrl(root)) {
          return local(root.toString());
        }
      }
    }
    if (url_source) {
      continue;  // anchored at the URDF's final URL once it is fetched
    }
    bool rewritten = false;
    for (auto root = roots->begin(); root != roots->end(); ++root) {
      if (!root->isRelative()) {
        continue;
      }
      if (!anchored) {
        return unanchored(root->toString());
      }
      *root = layout_location.resolved(*root);
      rewritten = true;
    }
    if (rewritten) {
      edits.push_back({model, u"package_url_roots"_s, serializePackageUrlRoots(*roots)});
    }
  }
  for (Edit& edit : edits) {
    if (edit.value.has_value()) {
      edit.element.setAttribute(edit.attribute, *edit.value);
    } else {
      edit.element.removeAttribute(edit.attribute);
    }
  }
  return true;
}

void relocateLayoutResources(QDomDocument& document, const std::function<QString(const QString&)>& relocate) {
  const QDomNodeList models = document.elementsByTagName(u"robot_model"_s);
  for (int index = 0; index < models.size(); ++index) {
    QDomElement model = models.at(index).toElement();
    const QString source_type = model.attribute(u"source_type"_s);
    if (source_type == "url"_L1) {
      continue;
    }
    const QString source = model.attribute(u"source_value"_s);
    if (source_type == "file"_L1 && !source.isEmpty() && QDir::isAbsolutePath(source)) {
      model.setAttribute(u"source_value"_s, relocate(source));
    }
    std::optional<QMap<QString, QUrl>> roots = parsePackageUrlRoots(model.attribute(u"package_url_roots"_s));
    if (!roots.has_value()) {
      continue;  // the load reports it
    }
    bool rewritten = false;
    for (auto root = roots->begin(); root != roots->end(); ++root) {
      if (!root->isLocalFile()) {
        continue;
      }
      const QString relocated = relocate(QDir::cleanPath(root->toLocalFile()));
      if (!QDir::isAbsolutePath(relocated)) {
        *root = relativeRootUrl(relocated);
        rewritten = true;
      }
    }
    if (rewritten) {
      model.setAttribute(u"package_url_roots"_s, serializePackageUrlRoots(*roots));
    }
  }
}

}  // namespace pj::scene3d
