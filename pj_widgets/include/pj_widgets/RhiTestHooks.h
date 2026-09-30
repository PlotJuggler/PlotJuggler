#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>

class QObject;
class QRhiGraphicsPipeline;
class QShader;

namespace PJ {

// The GPU widgets' test-only hooks (Scene3D, plots, the media viewer), in one
// place. They act only in builds with tests (`PJ_RHI_TEST_HOOKS`, never a
// release): elsewhere a fault query is false, a counter a no-op and a path
// empty. A fault makes a creation fail as a driver would; what the widget does
// about it is its own production failure policy (GraphicsApi.h's
// RhiFailureLatch and each widget's fallbacks).
//
// Named faults:
// - `owner`'s dynamic property `pjTestFailPipeline` names one of its pipelines
//   or bindings (rhiTestFault(), createRhiPipeline());
// - the environment variable `PJ_RHI_TEST_FAIL_SHADER` (kRhiTestFailShaderEnv)
//   holds a suffix of a shader path: every pipeline with a shader
//   loadRhiShader() read from a matching path fails (rhiTestShaderFault()).
// Counters are dynamic properties on the widget: `pjTestRhiAdoptions`,
// `pjTestRhiReleases`, `pjTestRhiStale` (noteRhiAdoption()/noteRhiRelease())
// and `pjTestPipelineAttempts` (createRhiPipeline()).

inline constexpr char kRhiTestFailShaderEnv[] = "PJ_RHI_TEST_FAIL_SHADER";

/// True while `owner`'s `pjTestFailPipeline` equals `name`.
[[nodiscard]] bool rhiTestFault(const QObject& owner, const char* name);

/// True when one of `pipeline`'s shaders matches `PJ_RHI_TEST_FAIL_SHADER`.
[[nodiscard]] bool rhiTestShaderFault(const QRhiGraphicsPipeline& pipeline);

/// Adds one to `owner`'s dynamic property `counter`.
void rhiTestCount(QObject& owner, const char* counter);

/// Records that loadRhiShader() read `shader` from `path`.
void rhiTestNoteShaderPath(const QShader& shader, const QString& path);

}  // namespace PJ
