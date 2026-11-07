// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/RhiTestHooks.h"

#include <rhi/qrhi.h>
#include <rhi/qshader.h>

#include <QHash>
#include <QObject>
#include <QVariant>
#include <algorithm>

namespace PJ {

#ifdef PJ_RHI_TEST_HOOKS
namespace {

QHash<QShader, QString>& loadedShaderPaths() {
  static QHash<QShader, QString> paths;
  return paths;
}

// The path loadRhiShader() read `shader` from; empty for any other shader.
QString rhiShaderPath(const QShader& shader) {
  return loadedShaderPaths().value(shader);
}

}  // namespace

bool rhiTestFault(const QObject& owner, const char* name) {
  return owner.property("pjTestFailPipeline").toString() == QLatin1StringView(name);
}

bool rhiTestShaderFault(const QRhiGraphicsPipeline& pipeline) {
  const QString suffix = qEnvironmentVariable(kRhiTestFailShaderEnv);
  return !suffix.isEmpty() &&
         std::any_of(pipeline.cbeginShaderStages(), pipeline.cendShaderStages(), [&](const QRhiShaderStage& stage) {
           return rhiShaderPath(stage.shader()).endsWith(suffix);
         });
}

void rhiTestCount(QObject& owner, const char* counter) {
  owner.setProperty(counter, owner.property(counter).toInt() + 1);
}

void rhiTestNoteShaderPath(const QShader& shader, const QString& path) {
  loadedShaderPaths().insert(shader, path);
}
#else
bool rhiTestFault(const QObject& /*owner*/, const char* /*name*/) {
  return false;
}

bool rhiTestShaderFault(const QRhiGraphicsPipeline& /*pipeline*/) {
  return false;
}

void rhiTestCount(QObject& /*owner*/, const char* /*counter*/) {}

void rhiTestNoteShaderPath(const QShader& /*shader*/, const QString& /*path*/) {}
#endif

}  // namespace PJ
