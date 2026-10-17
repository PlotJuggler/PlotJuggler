// SPDX-License-Identifier: MPL-2.0
#include "gui_test_env.h"

#include <chrono>

#include "pj_plotting/PlotWidgetBase.h"
#include "pj_runtime/BatchEngine.h"

namespace pj_app_test {

void GuiTestEnvironment::SetUp() {
  PJ::PlotWidgetBase::setGpuCanvasDisabledOverride(true);
  // A layout load waits a bounded time for each marker rule; under ctest -j that run may
  // outlive the 8 ms budget, so every case waits for it to finish instead. A test of the
  // bounded wait sets the production budget itself.
  PJ::BatchEngine::setCreateBudgetForTest(std::chrono::seconds(30));
}

bool isTestSuiteSelected(std::string_view suite_name) {
  const auto* unit_test = ::testing::UnitTest::GetInstance();
  for (int i = 0; i < unit_test->total_test_suite_count(); ++i) {
    const auto* suite = unit_test->GetTestSuite(i);
    if (suite->should_run() && suite_name == suite->name()) {
      return true;
    }
  }
  return false;
}

namespace {
static auto* const kEnv = ::testing::AddGlobalTestEnvironment(new GuiTestEnvironment);
}  // namespace

}  // namespace pj_app_test
