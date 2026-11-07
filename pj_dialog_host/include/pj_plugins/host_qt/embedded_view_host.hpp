// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QString>
#include <QWidget>
#include <functional>
#include <pj_plugins/sdk/widget_data.hpp>
#include <string>

namespace PJ {

/// Host-side factory for the views a plugin panel embeds in one of its QFrames
/// (WidgetData::setSceneView). The dialog host stays domain-neutral and links no
/// scene library: it only creates, feeds topics to and deletes an opaque QWidget the
/// application builds. All three callbacks must be set (see `operator bool`); every
/// call is [main-thread], made while widget data is being applied.
struct EmbeddedViewHost {
  /// Build a view of `kind` ("3d" / "2d") as a child of `parent` (the frame, which
  /// owns it). Null: this host cannot show that kind.
  std::function<QWidget*(const QString& kind, QWidget* parent)> create = {};
  /// Bind `topic` (resolved in the dataset named `dataset`, or by name when empty) to
  /// `view`. Returns false and fills `error` when the topic cannot be attached.
  std::function<bool(QWidget* view, const QString& topic, const QString& dataset, std::string* error)> attach = {};
  /// Undo a successful attach. Never called for a topic whose attach failed.
  std::function<void(QWidget* view, const QString& topic, const QString& dataset)> detach = {};

  explicit operator bool() const {
    return static_cast<bool>(create) && static_cast<bool>(attach) && static_cast<bool>(detach);
  }
};

/// A `scene_view` request of the frame named `frame` that could not be honoured,
/// returned by applyWidgetData for the engine to deliver to the plugin as the
/// `scene_view_error` widget event once the whole update is applied.
struct EmbeddedViewFailure {
  std::string frame;
  SceneViewError error;
};

}  // namespace PJ
