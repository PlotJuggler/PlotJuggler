// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/layout_resources.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QDomDocument>
#include <QDomElement>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QUrl>
#include <string>

using namespace Qt::StringLiterals;

namespace {

QDomDocument parse(const QString& xml) {
  QDomDocument doc;
  EXPECT_TRUE(doc.setContent(xml));
  return doc;
}

QDomElement model(const QDomDocument& doc, int index) {
  return doc.elementsByTagName(u"robot_model"_s).at(index).toElement();
}

std::string root(const QDomElement& element, const QString& package) {
  return QJsonDocument::fromJson(element.attribute(u"package_url_roots"_s).toUtf8())
      .object()
      .value(package)
      .toString()
      .toStdString();
}

std::string attribute(const QDomElement& element, const QString& name) {
  return element.attribute(name).toStdString();
}

// An absolute path on this platform; a drive-less "/data" is relative on Windows.
QString absoluteBase() {
  return QDir::cleanPath(QDir::tempPath()) + u"/pj_layout_resources"_s;
}

std::string fileUrl(const QString& path) {
  return QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded).toStdString();
}

}  // namespace

// Topic roots anchor at the layout; a relative URL source does too, while its
// own roots wait for the URDF's final URL.
TEST(LayoutResourcesTest, NormalizesTopicRootsAndUrlSources) {
  QDomDocument doc = parse(
      uR"xml(<root><scene3d>
    <layer><robot_model source_type="topic" source_value="/robot_description"
        package_url_roots='{"franka_description":"assets/franka_description/","cdn_pkg":"https://cdn.test/pkg/"}'/></layer>
    <layer><robot_model source_type="url" source_value="robots/arm.urdf"
        package_url_roots='{"arm_description":"../meshes/arm_description/"}'/></layer>
  </scene3d></root>)xml"_s);

  QString error;
  ASSERT_TRUE(
      pj::scene3d::resolveLayoutResourceUrls(doc, QUrl(u"https://host.test/presets/fr3/layout.pj4.xml"_s), &error))
      << error.toStdString();

  EXPECT_EQ(root(model(doc, 0), u"franka_description"_s), "https://host.test/presets/fr3/assets/franka_description/");
  EXPECT_EQ(root(model(doc, 0), u"cdn_pkg"_s), "https://cdn.test/pkg/");
  EXPECT_EQ(attribute(model(doc, 0), u"source_value"_s), "/robot_description");

  EXPECT_EQ(attribute(model(doc, 1), u"source_value"_s), "https://host.test/presets/fr3/robots/arm.urdf");
  EXPECT_EQ(root(model(doc, 1), u"arm_description"_s), "../meshes/arm_description/");

  // A desktop layout anchors at its file:// location.
  QDomDocument local =
      parse(uR"xml(<root><robot_model source_type="topic" package_url_roots='{"pkg":"assets/pkg/"}'/></root>)xml"_s);
  ASSERT_TRUE(pj::scene3d::resolveLayoutResourceUrls(local, QUrl(u"file:///data/layouts/robot.pj4.xml"_s), &error))
      << error.toStdString();
  EXPECT_EQ(root(model(local, 0), u"pkg"_s), "file:///data/layouts/assets/pkg/");
}

// Every failure leaves the whole document as it was, including the models
// that came before the offending one.
TEST(LayoutResourcesTest, FailureDoesNotMutateDocument) {
  const QString xml =
      uR"xml(<root>
    <robot_model source_type="topic" package_url_roots='{"good":"assets/good/"}'/>
    <robot_model source_type="url" source_value="robots/arm.urdf"/>
    <robot_model source_type="topic" package_url_roots='["not","an","object"]'/>
  </root>)xml"_s;
  const QUrl location(u"https://host.test/presets/layout.pj4.xml"_s);
  QString error;

  QDomDocument malformed = parse(xml);
  EXPECT_FALSE(pj::scene3d::resolveLayoutResourceUrls(malformed, location, &error));
  EXPECT_FALSE(error.isEmpty());
  EXPECT_EQ(malformed.toString().toStdString(), parse(xml).toString().toStdString());

  QDomDocument bad_scheme = parse(
      uR"xml(<root><robot_model source_type="topic" package_url_roots='{"pkg":"assets/"}'/>
          <robot_model source_type="topic" package_url_roots='{"pkg":"javascript:alert(1)"}'/></root>)xml"_s);
  const QString bad_scheme_before = bad_scheme.toString();
  error.clear();
  EXPECT_FALSE(pj::scene3d::resolveLayoutResourceUrls(bad_scheme, location, &error));
  EXPECT_FALSE(error.isEmpty());
  EXPECT_EQ(bad_scheme.toString().toStdString(), bad_scheme_before.toStdString());

  // Relative references need a layout location; the page URL is never one.
  QDomDocument unanchored =
      parse(uR"xml(<root><robot_model source_type="topic" package_url_roots='{"pkg":"assets/pkg/"}'/></root>)xml"_s);
  const QString unanchored_before = unanchored.toString();
  error.clear();
  EXPECT_FALSE(pj::scene3d::resolveLayoutResourceUrls(unanchored, QUrl(), &error));
  EXPECT_FALSE(error.isEmpty());
  EXPECT_EQ(unanchored.toString().toStdString(), unanchored_before.toStdString());
}

// Layouts without relative Scene3D references pass through byte for byte, even
// without a layout location (a browser upload).
TEST(LayoutResourcesTest, LeavesLegacyLocalModelsAlone) {
  const QString xml =
      uR"xml(<root><scene3d>
    <layer><robot_model source_type="file" source_value="%1/robot.urdf"/></layer>
    <layer><robot_model source_type="topic" source_value="/robot_description" source_topic_name="/robot_description"/></layer>
    <layer><robot_model source_type="url" source_value="https://host.test/robot.urdf"/></layer>
    <layer><robot_model source_type="url" source_value="file:///home/user/robot.urdf"/></layer>
    <layer><robot_model source_type="url" source_value="/home/user/robot.urdf"/></layer>
    <layer><robot_model source_type="topic" package_url_roots='{"pkg":"https://cdn.test/pkg/"}'/></layer>
  </scene3d></root>)xml"_s.arg(absoluteBase());
  for (const QUrl& location : {QUrl(u"file:///data/layouts/robot.pj4.xml"_s), QUrl()}) {
    QDomDocument doc = parse(xml);
    QString error;
    EXPECT_TRUE(pj::scene3d::resolveLayoutResourceUrls(doc, location, &error)) << error.toStdString();
    EXPECT_EQ(doc.toString().toStdString(), parse(xml).toString().toStdString()) << location.toString().toStdString();
  }
}

// A file source is read from the layout's folder, never the working directory, and only a
// local layout has one: a browser upload has no location and keeps its file label.
TEST(LayoutResourcesTest, RelativeFileSourceAnchorsAtALocalLayoutsFolder) {
  const QString base = absoluteBase();
  const QString xml =
      uR"xml(<root>
    <robot_model source_type="file" source_value="robot/r.urdf"/>
    <robot_model source_type="file" source_value="../shared/my robot.urdf"/>
    <robot_model source_type="file" source_value="%1/abs/r.urdf"/>
    <robot_model source_type="topic" source_value="robot_description"/>
    <robot_model source_value="robot_description"/>
    <robot_model source_type="file" source_value=""/>
  </root>)xml"_s.arg(base);
  QString error;
  QDomDocument doc = parse(xml);
  ASSERT_TRUE(pj::scene3d::resolveLayoutResourceUrls(doc, QUrl::fromLocalFile(base + u"/a/robot.pj4.xml"_s), &error))
      << error.toStdString();
  EXPECT_EQ(model(doc, 0).attribute(u"source_value"_s), base + u"/a/robot/r.urdf"_s);
  EXPECT_EQ(model(doc, 1).attribute(u"source_value"_s), base + u"/shared/my robot.urdf"_s);
  EXPECT_EQ(model(doc, 2).attribute(u"source_value"_s), base + u"/abs/r.urdf"_s);
  EXPECT_EQ(attribute(model(doc, 3), u"source_value"_s), "robot_description");
  EXPECT_EQ(attribute(model(doc, 4), u"source_value"_s), "robot_description");
  EXPECT_EQ(attribute(model(doc, 5), u"source_value"_s), "");

  QDomDocument upload = parse(xml);
  ASSERT_TRUE(pj::scene3d::resolveLayoutResourceUrls(upload, QUrl(), &error)) << error.toStdString();
  EXPECT_EQ(upload.toString().toStdString(), parse(xml).toString().toStdString());
}

// The save side of the same contract: what lies at or beneath the layout folder is stored
// relative to it and reads back through resolveLayoutResourceUrls, wherever the folder went.
TEST(LayoutResourcesTest, RelocatedReferencesResolveBackAtTheNewLocation) {
  const QString base = absoluteBase();
  const QString folder_a = base + u"/a"_s;
  const QString folder_b = base + u"/b"_s;
  const QUrl roots_in_a = QUrl::fromLocalFile(folder_a + u"/meshes/pkg/"_s);
  QMap<QString, QUrl> file_roots{
      {u"in"_s, roots_in_a},
      {u"space"_s, QUrl::fromLocalFile(folder_a + u"/my meshes/pkg/"_s)},
      {u"self"_s, QUrl::fromLocalFile(folder_a + u"/"_s)},
      {u"out"_s, QUrl::fromLocalFile(base + u"/outside/pkg/"_s)},
      {u"web"_s, QUrl(u"https://cdn.test/pkg/"_s)},
  };
#ifndef Q_OS_WIN
  // ':' is not legal in a Windows file name, where "c:d" is a drive-relative path.
  file_roots.insert(u"colon"_s, QUrl::fromLocalFile(folder_a + u"/c:d/pkg/"_s));
#endif
  const QMap<QString, QUrl> one_root{{u"pkg"_s, roots_in_a}};

  QDomDocument doc = parse(u"<root/>"_s);
  const auto add_model = [&doc](const QString& type, const QString& value, const QMap<QString, QUrl>& roots) {
    QDomElement element = doc.createElement(u"robot_model"_s);
    if (!type.isEmpty()) {
      element.setAttribute(u"source_type"_s, type);
    }
    element.setAttribute(u"source_value"_s, value);
    if (!roots.isEmpty()) {
      element.setAttribute(u"package_url_roots"_s, pj::scene3d::serializePackageUrlRoots(roots));
    }
    doc.documentElement().appendChild(element);
  };
  add_model(u"file"_s, folder_a + u"/robot/r.urdf"_s, file_roots);
  add_model(u"file"_s, base + u"/outside/robot.urdf"_s, {});
  add_model(u"file"_s, u"robot/already_relative.urdf"_s, {});
  add_model(u"topic"_s, u"/robot_description"_s, one_root);
  add_model(QString{}, u"/robot_description"_s, one_root);
  add_model(u"file"_s, QString{}, {});

  pj::scene3d::relocateLayoutResources(doc, [&folder_a](const QString& path) {
    if (path == folder_a) {
      return u"."_s;
    }
    return path.startsWith(folder_a + u'/') ? path.mid(folder_a.size() + 1) : path;
  });

  const QDomElement file_model = model(doc, 0);
  EXPECT_EQ(attribute(file_model, u"source_value"_s), "robot/r.urdf");
  EXPECT_EQ(root(file_model, u"in"_s), "meshes/pkg/");
  EXPECT_EQ(root(file_model, u"space"_s), "my%20meshes/pkg/");
  EXPECT_EQ(root(file_model, u"self"_s), "./");
#ifndef Q_OS_WIN
  EXPECT_EQ(root(file_model, u"colon"_s), "./c:d/pkg/");
#endif
  EXPECT_EQ(root(file_model, u"out"_s), fileUrl(base + u"/outside/pkg/"_s));
  EXPECT_EQ(root(file_model, u"web"_s), "https://cdn.test/pkg/");
  EXPECT_EQ(model(doc, 1).attribute(u"source_value"_s), base + u"/outside/robot.urdf"_s);
  EXPECT_EQ(attribute(model(doc, 2), u"source_value"_s), "robot/already_relative.urdf");
  EXPECT_EQ(attribute(model(doc, 3), u"source_value"_s), "/robot_description") << "a topic name is not a path";
  EXPECT_EQ(root(model(doc, 3), u"pkg"_s), "meshes/pkg/");
  EXPECT_EQ(attribute(model(doc, 4), u"source_value"_s), "/robot_description");
  EXPECT_EQ(root(model(doc, 4), u"pkg"_s), "meshes/pkg/");
  EXPECT_EQ(attribute(model(doc, 5), u"source_value"_s), "");

  QString error;
  ASSERT_TRUE(pj::scene3d::resolveLayoutResourceUrls(doc, QUrl::fromLocalFile(folder_b + u"/layout.pj4.xml"_s), &error))
      << error.toStdString();
  const QDomElement resolved = model(doc, 0);
  EXPECT_EQ(resolved.attribute(u"source_value"_s), folder_b + u"/robot/r.urdf"_s);
  EXPECT_EQ(root(resolved, u"in"_s), fileUrl(folder_b + u"/meshes/pkg/"_s));
  EXPECT_EQ(root(resolved, u"space"_s), fileUrl(folder_b + u"/my meshes/pkg/"_s));
  EXPECT_EQ(root(resolved, u"self"_s), fileUrl(folder_b + u"/"_s));
#ifndef Q_OS_WIN
  EXPECT_EQ(root(resolved, u"colon"_s), fileUrl(folder_b + u"/c:d/pkg/"_s));
#endif
  EXPECT_EQ(root(resolved, u"out"_s), fileUrl(base + u"/outside/pkg/"_s));
  EXPECT_EQ(root(model(doc, 3), u"pkg"_s), fileUrl(folder_b + u"/meshes/pkg/"_s));
}

// A URL source's relative roots anchor at the URDF's final URL, not the layout, so saving must
// not turn its roots into layout-relative ones. Malformed roots are the load's error to report.
TEST(LayoutResourcesTest, RelocateLeavesUrlSourcesAndMalformedRootsAlone) {
  const QString xml =
      uR"xml(<root>
    <robot_model source_type="url" source_value="https://host.test/r.urdf" package_url_roots='{"pkg":"file:///data/a/pkg/"}'/>
    <robot_model source_type="url" source_value="/data/a/r.urdf" package_url_roots='{"pkg":"../pkg/"}'/>
    <robot_model source_type="topic" package_url_roots='["not","an","object"]'/>
  </root>)xml"_s;
  QDomDocument doc = parse(xml);

  pj::scene3d::relocateLayoutResources(doc, [](const QString&) { return u"relocated"_s; });

  EXPECT_EQ(doc.toString().toStdString(), parse(xml).toString().toStdString());
}

// A web-hosted layout never points the app at local files: a file source, a
// local URL source and a file: root all fail the whole load there, while the
// same models stay legal in a desktop layout.
TEST(LayoutResourcesTest, NetworkLayoutsCannotNameLocalResources) {
  const QUrl location(u"https://host.test/presets/layout.pj4.xml"_s);
  for (const QString& model : {
           uR"xml(<robot_model source_type="file" source_value="/home/user/robot.urdf"/>)xml"_s,
           uR"xml(<robot_model source_type="file" source_value="robot.urdf"/>)xml"_s,
           uR"xml(<robot_model source_type="url" source_value="file:///home/user/robot.urdf"/>)xml"_s,
           uR"xml(<robot_model source_type="url" source_value="/home/user/robot.urdf"/>)xml"_s,
           uR"xml(<robot_model source_type="url" source_value="C:\robots\robot.urdf"/>)xml"_s,
           uR"xml(<robot_model source_type="topic" package_url_roots='{"pkg":"file:///opt/ros/share/pkg/"}'/>)xml"_s,
           uR"xml(<robot_model source_type="url" source_value="https://host.test/robot.urdf"
               package_url_roots='{"pkg":"file:///opt/ros/share/pkg/"}'/>)xml"_s,
       }) {
    const QString xml = u"<root><robot_model source_type=\"topic\" package_url_roots='{\"good\":\"assets/\"}'/>"_s +
                        model + u"</root>"_s;
    QDomDocument doc = parse(xml);
    QString error;
    EXPECT_FALSE(pj::scene3d::resolveLayoutResourceUrls(doc, location, &error)) << model.toStdString();
    EXPECT_FALSE(error.isEmpty()) << model.toStdString();
    EXPECT_EQ(doc.toString().toStdString(), parse(xml).toString().toStdString()) << model.toStdString();

    QDomDocument desktop = parse(xml);
    EXPECT_TRUE(pj::scene3d::resolveLayoutResourceUrls(desktop, QUrl(u"file:///data/layout.pj4.xml"_s), &error))
        << model.toStdString() << ": " << error.toStdString();
  }
}

// A web-hosted layout's robots carry the network-only policy into the URDFs they load, whose own
// mesh references the layout check cannot see; a desktop layout's robots do not.
TEST(LayoutResourcesTest, NetworkLayoutsRestrictTheirRobotsToNetworkResources) {
  const QString xml =
      uR"xml(<root><scene3d>
    <layer><robot_model source_type="topic" source_value="/robot_description"/></layer>
    <layer><robot_model source_type="url" source_value="robots/arm.urdf"/></layer>
  </scene3d></root>)xml"_s;
  QDomDocument web = parse(xml);
  QString error;
  ASSERT_TRUE(pj::scene3d::resolveLayoutResourceUrls(web, QUrl(u"https://host.test/presets/layout.pj4.xml"_s), &error))
      << error.toStdString();
  for (int index = 0; index < 2; ++index) {
    EXPECT_EQ(attribute(model(web, index), u"remote_resources_only"_s), "true") << index;
  }

  QDomDocument desktop = parse(xml);
  ASSERT_TRUE(pj::scene3d::resolveLayoutResourceUrls(desktop, QUrl(u"file:///data/layout.pj4.xml"_s), &error));
  for (int index = 0; index < 2; ++index) {
    EXPECT_FALSE(model(desktop, index).hasAttribute(u"remote_resources_only"_s)) << index;
  }
}

// The policy follows where the layout was loaded from, not where it was saved: a browser-saved
// layout opened from a local file drops the network-only policy, so its local meshes load.
TEST(LayoutResourcesTest, LocalLayoutsDropTheNetworkOnlyPolicyAWebLayoutSaved) {
  const QString xml =
      uR"xml(<root><scene3d>
    <layer><robot_model source_type="topic" source_value="/robot_description" remote_resources_only="true"/></layer>
    <layer><robot_model source_type="url" source_value="https://host.test/arm.urdf" remote_resources_only="true"/></layer>
  </scene3d></root>)xml"_s;
  for (const QUrl& location : {QUrl(u"file:///data/layout.pj4.xml"_s), QUrl()}) {
    QDomDocument doc = parse(xml);
    QString error;
    ASSERT_TRUE(pj::scene3d::resolveLayoutResourceUrls(doc, location, &error)) << error.toStdString();
    for (int index = 0; index < 2; ++index) {
      EXPECT_FALSE(model(doc, index).hasAttribute(u"remote_resources_only"_s))
          << index << " " << location.toString().toStdString();
    }
  }

  QDomDocument web = parse(xml);
  QString error;
  ASSERT_TRUE(pj::scene3d::resolveLayoutResourceUrls(web, QUrl(u"https://host.test/presets/layout.pj4.xml"_s), &error))
      << error.toStdString();
  for (int index = 0; index < 2; ++index) {
    EXPECT_EQ(attribute(model(web, index), u"remote_resources_only"_s), "true") << index;
  }
}
