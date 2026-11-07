// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

// Scene3D's resource references inside a layout file, and the codec of the
// `<robot_model package_url_roots="JSON object">` attribute.
//
// A layout may name robot resources relative to where the layout itself lives,
// so a layout plus its assets can be moved or hosted as one folder. The host
// resolves them ONCE, on the application's copy of the document, before any
// widget restores from it; what widgets then apply, snapshot and save is
// absolute, so undo and reused widgets never need the layout's location.

#include <QMap>
#include <QString>
#include <QUrl>
#include <functional>
#include <optional>

class QDomDocument;

namespace pj::scene3d {

// Rewrites every `robot_model` element's relative references against
// `layout_location` (the layout file's own URL: file:// on the desktop):
//  - a URL source's relative `source_value` (a value naming a local absolute
//    path is a legacy desktop path and stays as it is);
//  - a file source's relative `source_value`, only at a local `layout_location`
//    and never in the browser, which keeps it as a file label: such a URDF is
//    read from the layout's folder, never the process working directory;
//  - the `package_url_roots` of a topic or file source. A URL source keeps its
//    relative roots: they resolve against its fetched URDF's final URL.
// Absolute references and models without relative ones are left untouched.
// A web-hosted layout (an http(s) `layout_location`) never points the app at
// local files: a file source, a URL source naming a local path or non-http(s)
// URL, and a file: root all fail it, and every model gets
// `remote_resources_only="true"` so the meshes its URDF names stay off this
// host as well (RobotModelLayer). Any other layout location removes that
// attribute: a browser-saved robot opened from a local file may use local meshes.
//
// All or nothing: on failure (malformed roots, a non-http(s)/file root, a local
// reference in a web-hosted layout, or a relative reference with no valid
// `layout_location`) the document is left
// exactly as it was, `*error` (if non-null) says why, and false is returned.
// GUI-thread only, like the document it edits.
[[nodiscard]] bool resolveLayoutResourceUrls(QDomDocument& document, const QUrl& layout_location, QString* error);

// The save-side inverse, for a document about to be written to a layout file: a
// file source's absolute `source_value` and the file: `package_url_roots` of a
// topic or file source go through `relocate`, so what lies with the layout moves
// with it. `relocate` takes an absolute local path (a root's has no trailing
// slash) and returns it relative to the layout's folder ("." for the folder
// itself), or unchanged to keep the reference absolute. A URL source is left
// alone: its relative roots anchor at the URDF's final URL, not the layout.
// Malformed roots stay as they are for the load to report.
void relocateLayoutResources(QDomDocument& document, const std::function<QString(const QString&)>& relocate);

// Parses a `package_url_roots` attribute: a JSON object whose keys are
// non-empty package names and whose values are relative references or absolute
// http(s)/file URLs. An empty string is no roots; nullopt means malformed.
[[nodiscard]] std::optional<QMap<QString, QUrl>> parsePackageUrlRoots(const QString& json);

// The compact JSON form parsePackageUrlRoots() reads back.
[[nodiscard]] QString serializePackageUrlRoots(const QMap<QString, QUrl>& roots);

}  // namespace pj::scene3d
