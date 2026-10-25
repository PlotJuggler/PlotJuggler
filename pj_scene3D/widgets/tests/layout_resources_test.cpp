// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/layout_resources.h"

#include <gtest/gtest.h>

#include <QDomDocument>
#include <QDomElement>
#include <QJsonDocument>
#include <QJsonObject>
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
    <layer><robot_model source_type="file" source_value="/home/user/robot.urdf"/></layer>
    <layer><robot_model source_type="file" source_value="relative/robot.urdf"/></layer>
    <layer><robot_model source_type="topic" source_value="/robot_description" source_topic_name="/robot_description"/></layer>
    <layer><robot_model source_type="url" source_value="https://host.test/robot.urdf"/></layer>
    <layer><robot_model source_type="url" source_value="file:///home/user/robot.urdf"/></layer>
    <layer><robot_model source_type="url" source_value="/home/user/robot.urdf"/></layer>
    <layer><robot_model source_type="topic" package_url_roots='{"pkg":"https://cdn.test/pkg/"}'/></layer>
  </scene3d></root>)xml"_s;
  for (const QUrl& location : {QUrl(u"file:///data/layouts/robot.pj4.xml"_s), QUrl()}) {
    QDomDocument doc = parse(xml);
    QString error;
    EXPECT_TRUE(pj::scene3d::resolveLayoutResourceUrls(doc, location, &error)) << error.toStdString();
    EXPECT_EQ(doc.toString().toStdString(), parse(xml).toString().toStdString()) << location.toString().toStdString();
  }
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
